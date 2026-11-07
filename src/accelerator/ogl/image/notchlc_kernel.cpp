/*
 * HighAsCG WO-820: GPU half of the NotchLC decode (see notchlc_kernel.h).
 *
 * One fragment per output pixel decodes that pixel straight from the payload words, following FFmpeg 8.0.1
 * libavcodec/notchlc.c decode_blocks (l.245-464): 12-bit luma from 4x4 blocks with 1-4 bits per pixel (bit position from
 * the CPU-built block table), chroma from 16x16 blocks (one BC1-like endpoint pair, or per 8x8 quadrant, or per 4x4),
 * alpha from 16x16 blocks. Colour = full-range BT.601 YCbCr -> RGB, which is what swscale applies to this stream on the
 * CPU path (yuva444p12, colorspace "gbr", fitted on INTRO_v3.mov: WO-820).
 */

#include "notchlc_kernel.h"

#include "../util/device.h"
#include "../util/shader.h"
#include "../util/texture.h"

#include <common/gl/gl_check.h>
#include <common/log.h>

#include <core/frame/notchlc.h>

#include <GL/glew.h>

namespace caspar { namespace accelerator { namespace ogl {

namespace nl = core::notchlc;

namespace {

const char* const vertex_source = R"GLSL(#version 450
void main()
{
    vec2 p      = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)GLSL";

const char* const fragment_source = R"GLSL(#version 450
out vec4 fragColor;

uniform sampler2D payload;
uniform bool      valid;
uniform bool      opaque_alpha;
uniform int       width;
uniform int       table;
uniform int       y_ctrl;
uniform int       uv_off;
uniform int       uv_data;
uniform int       a_ctrl;
uniform int       a_data;

const int BASE = 16; // core::notchlc::header_words

// One texel = one little-endian 32-bit word (uploaded as BGRA 8:8:8:8_REV: byte0 -> b ... byte3 -> a).
uint word_at(int w)
{
    uvec4 b = uvec4(round(texelFetch(payload, ivec2(w & 1023, w >> 10), 0) * 255.0));
    return b.b | (b.g << 8u) | (b.r << 16u) | (b.a << 24u);
}

uint pword(uint byte_off) { return word_at(BASE + int(byte_off >> 2u)); }

// LSB-first bit reader (BITSTREAM_READER_LE) over the whole plane.
uint bits_at(uint bitpos, uint n)
{
    int  w = int(bitpos >> 5u);
    uint s = bitpos & 31u;
    uint v = word_at(w) >> s;
    if (s + n > 32u)
        v |= word_at(w + 1) << (32u - s);
    return v & ((1u << n) - 1u);
}

int tdiv3(int n) { return n >= 0 ? n / 3 : -((-n) / 3); } // C division truncates toward zero

int expand8(uint b) { return int((b << 4u) | (b & 15u)); }

ivec2 uv_pair(uint pos, uint idx)
{
    uint e   = pword(pos);
    uint loc = pword(pos + 4u);
    int  s   = int((loc >> (2u * idx)) & 3u);
    int  u0  = expand8(e & 255u);
    int  v0  = expand8((e >> 8u) & 255u);
    int  u1  = expand8((e >> 16u) & 255u);
    int  v1  = expand8(e >> 24u);
    return ivec2(u0 + tdiv3((u1 - u0) * s + 2), v0 + tdiv3((v1 - v0) * s + 2));
}

void main()
{
    if (!valid) {
        fragColor = vec4(0.0);
        return;
    }

    uint x = uint(gl_FragCoord.x);
    uint y = uint(gl_FragCoord.y); // texture row 0 = picture line 0, as every Caspar upload
    uint W = uint(width);

    // Luma: 4x4 block control word = y_min:12 y_max:12 then 2 bits (bits-per-pixel - 1) per block row.
    uint bi   = (y >> 2u) * ((W + 3u) >> 2u) + (x >> 2u);
    uint item = pword(uint(y_ctrl) + 4u * bi);
    uint r    = y & 3u;
    uint pre  = 0u;
    for (uint k = 0u; k < r; ++k)
        pre += 4u * (((item >> (24u + 2u * k)) & 3u) + 1u);
    uint nb    = ((item >> (24u + 2u * r)) & 3u) + 1u;
    uint v     = bits_at(uint(word_at(table + int(bi))) + pre + (x & 3u) * nb, nb);
    uint ymin  = item & 4095u;
    uint ydiff = ((item >> 12u) & 4095u) - ymin;
    uint div   = (1u << nb) - 1u;
    int  Y     = clamp(int(ymin + (ydiff * v + div - 1u) / div), 0, 4095);

    // Chroma: 16x16 blocks.
    uint b16 = (y >> 4u) * ((W + 15u) >> 4u) + (x >> 4u);
    uint ly  = y & 15u;
    uint lx  = x & 15u;
    uint d   = uint(uv_data) + pword(uint(uv_off) + 4u * b16) * 4u;
    uint hdr = pword(d);
    uint is8 = hdr & 0xFFFFu;
    uint esc = hdr >> 16u;
    ivec2 uv = ivec2(0);
    if (esc == 0u && is8 == 0u) {
        uv = uv_pair(d + 4u, (ly >> 2u) * 4u + (lx >> 2u));
    } else {
        uint q   = (ly >> 3u) * 2u + (lx >> 3u);
        uint pos = d + 4u;
        for (uint k = 0u; k < q; ++k)
            pos += ((is8 >> k) & 1u) != 0u ? 8u : (esc != 0u ? 32u : 0u);
        uint qy = ly & 7u;
        uint qx = lx & 7u;
        if (((is8 >> q) & 1u) != 0u)
            uv = uv_pair(pos, (qy >> 1u) * 4u + (qx >> 1u));
        else if (esc != 0u)
            uv = uv_pair(pos + 8u * ((qy >> 2u) * 2u + (qx >> 2u)), (qy & 3u) * 4u + (qx & 3u));
    }

    // Alpha: 16x16 blocks, 2-bit mode + 3-bit index per 4x4 cell.
    uint A = 4095u;
    if (!opaque_alpha) {
        uint m    = pword(uint(a_ctrl) + 8u * b16);
        uint ao   = pword(uint(a_ctrl) + 8u * b16 + 4u) * 4u + uint(uv_data) + uint(a_data);
        uint c0   = pword(ao);
        uint c1   = pword(ao + 4u);
        uint sub  = (ly >> 2u) * 4u + (lx >> 2u);
        uint mode = (m >> (2u * sub)) & 3u;
        uint bp   = 16u + 3u * sub;
        uint idx  = bp >= 32u ? (c1 >> (bp - 32u)) & 7u
                              : (bp + 3u > 32u ? ((c0 >> bp) | (c1 << (32u - bp))) & 7u : (c0 >> bp) & 7u);
        uint a0   = c0 & 255u;
        uint a1   = (c0 >> 8u) & 255u;
        A = mode == 1u ? 4095u : (mode == 2u ? ((a0 + (a1 - a0) * idx) << 4u) & 0xFFFFu : 0u);
    }

    float Yf  = float(Y) / 4095.0;
    float Cb  = float(uv.x - 2048) / 4095.0;
    float Cr  = float(uv.y - 2048) / 4095.0;
    vec3  rgb = clamp(vec3(Yf + 1.402 * Cr, Yf - 0.344136 * Cb - 0.714136 * Cr, Yf + 1.772 * Cb), 0.0, 1.0);
    // A bgra frame's texture holds the channels in natural order (uploaded as GL_BGRA), so write rgba here and let the
    // image kernel read the texture as pixel_format::bgra like any other bgra frame (checked on the box: WO-820).
    fragColor = vec4(rgb, float(min(A, 4095u)) / 4095.0);
}
)GLSL";

struct notchlc_program
{
    std::unique_ptr<shader> program;
    GLuint                  vao = 0;

    notchlc_program()
        : program(new shader(vertex_source, fragment_source))
    {
        GL(glGenVertexArrays(1, &vao));
    }
    ~notchlc_program() { glDeleteVertexArrays(1, &vao); }
};

// Created and used on the OpenGL device thread only (there is one device per process).
notchlc_program& gl_program()
{
    static notchlc_program* p = new notchlc_program(); // never freed: lives as long as the GL context
    return *p;
}

int hdr(const array<const std::uint8_t>& plane, nl::header_field f) { return static_cast<int>(nl::read_word(plane.data(), f)); }

} // namespace

bool notchlc_decoded_desc(const array<const std::uint8_t>& plane, core::pixel_format_desc& desc)
{
    if (plane.size() < nl::header_words * 4 || nl::read_word(plane.data(), nl::h_magic) != nl::magic)
        return false;
    const int w = hdr(plane, nl::h_width), h = hdr(plane, nl::h_height);
    if (w <= 0 || h <= 0)
        return false;
    desc        = core::pixel_format_desc(core::pixel_format::bgra);
    desc.planes = {core::pixel_format_desc::plane(w, h, 4, common::bit_depth::bit16)};
    return true;
}

std::shared_future<std::shared_ptr<texture>> notchlc_decode_async(const spl::shared_ptr<device>&   ogl,
                                                                  const array<const std::uint8_t>& plane,
                                                                  const core::pixel_format_desc&   desc)
{
    const auto& p0 = desc.planes.at(0);
    auto        payload =
        ogl->copy_async(plane, p0.width, p0.height, p0.stride, p0.depth).share(); // queued before the decode below

    struct params
    {
        bool valid, opaque;
        int  width, height, table, y_ctrl, uv_off, uv_data, a_ctrl, a_data;
    } u{};
    if (plane.size() >= nl::header_words * 4 && nl::read_word(plane.data(), nl::h_magic) == nl::magic) {
        u.valid   = hdr(plane, nl::h_valid) == 1;
        u.opaque  = hdr(plane, nl::h_opaque) == 1;
        u.width   = hdr(plane, nl::h_width);
        u.height  = hdr(plane, nl::h_height);
        u.table   = hdr(plane, nl::h_table);
        u.y_ctrl  = hdr(plane, nl::h_y_ctrl);
        u.uv_off  = hdr(plane, nl::h_uv_off);
        u.uv_data = hdr(plane, nl::h_uv_data);
        u.a_ctrl  = hdr(plane, nl::h_a_ctrl);
        u.a_data  = hdr(plane, nl::h_a_data);
    }
    if (u.width <= 0 || u.height <= 0) {
        u.valid  = false;
        u.width  = 16;
        u.height = 16;
    }

    return ogl
        ->dispatch_async([ogl, payload, u]() -> std::shared_ptr<texture> {
            auto src = payload.get();
            auto dst = ogl->create_texture(u.width, u.height, 4, common::bit_depth::bit16);

            auto& prog = gl_program();
            prog.program->use();
            prog.program->set("payload", 0);
            prog.program->set("valid", u.valid);
            prog.program->set("opaque_alpha", u.opaque);
            prog.program->set("width", u.width);
            prog.program->set("table", u.table);
            prog.program->set("y_ctrl", u.y_ctrl);
            prog.program->set("uv_off", u.uv_off);
            prog.program->set("uv_data", u.uv_data);
            prog.program->set("a_ctrl", u.a_ctrl);
            prog.program->set("a_data", u.a_data);

            src->bind(0);
            dst->attach();
            GL(glViewport(0, 0, u.width, u.height));
            GL(glDisable(GL_BLEND));
            GL(glDisable(GL_SCISSOR_TEST));
            GL(glDisable(GL_DEPTH_TEST));
            GL(glBindVertexArray(prog.vao));
            GL(glDrawArrays(GL_TRIANGLES, 0, 3));
            GL(glBindVertexArray(0));
            GL(glTextureBarrier());
            src->unbind();
            return dst;
        })
        .share();
}

}}} // namespace caspar::accelerator::ogl
