/*
 * HighAsCG WO-993: GPU half of the native DXV decode (see dxv_kernel.h).
 *
 * One fragment per output pixel decodes that pixel straight from its BC block, with the integer maths of FFmpeg 8.0.1
 * libavcodec/texturedsp.c (dxt1_block, dxt5 colour/alpha, rgtc1u_gray_block), so the 8-bit values equal FFmpeg's
 * except where this path deliberately differs:
 *  - DXT5 (Resolume "DXV with alpha") stores premultiplied colour. FFmpeg un-premultiplies it (dxt4_block); Caspar
 *    works premultiplied, so the raw palette colour is output as is.
 *  - DXV3 HQ (YCoCg): FFmpeg stops at raw yuv420p/yuva420p planes tagged YCgCo, which Caspar's CPU path then reads as
 *    BT.709 YCbCr. Here Y/Co/Cg (full range, chroma offset 128) become RGB: R = Y + Co - Cg, G = Y + Cg, B = Y - Co - Cg
 *    (checked on the FATE colour-bar sample: bars within 1 code of 0/255). Chroma is taken from the half-res sample
 *    covering the pixel; a last chroma column/row FFmpeg leaves undecoded (coded size not a multiple of 8) is clamped
 *    to the previous block instead.
 */

#include "dxv_kernel.h"

#include "../util/device.h"
#include "../util/shader.h"
#include "../util/texture.h"

#include <common/gl/gl_check.h>

#include <core/frame/dxv.h>

#include <GL/glew.h>

namespace caspar { namespace accelerator { namespace ogl {

namespace dx = core::dxv;

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
uniform int       fmt;     // core::dxv::format
uniform int       coded_w;
uniform int       coded_h;
uniform int       tex;     // word index of the full-res blocks
uniform int       ctex;    // word index of the half-res Co|Cg blocks

// One texel = one little-endian 32-bit word (uploaded as BGRA 8:8:8:8_REV: byte0 -> b ... byte3 -> a).
uint word_at(int w)
{
    uvec4 b = uvec4(round(texelFetch(payload, ivec2(w & 1023, w >> 10), 0) * 255.0));
    return b.b | (b.g << 8u) | (b.r << 16u) | (b.a << 24u);
}

// texturedsp.c extract_color: 565 -> 888.
int expand5(int c) { int t = c * 255 + 16; return (t / 32 + t) / 32; }
int expand6(int c) { int t = c * 255 + 32; return (t / 64 + t) / 64; }

// BC1 colour block (words w, w+1) for pixel i. four = dxt5's colour half (always 4-colour), else colour0 > colour1.
ivec3 bc1(int w, int i, bool four_always)
{
    uint  w0 = word_at(w);
    int   c0 = int(w0 & 0xFFFFu), c1 = int(w0 >> 16u);
    ivec3 e0 = ivec3(expand5(c0 >> 11), expand6((c0 & 0x07E0) >> 5), expand5(c0 & 0x1F));
    ivec3 e1 = ivec3(expand5(c1 >> 11), expand6((c1 & 0x07E0) >> 5), expand5(c1 & 0x1F));
    int   sel = int((word_at(w + 1) >> uint(2 * i)) & 3u);
    if (sel == 0) return e0;
    if (sel == 1) return e1;
    if (four_always || c0 > c1)
        return sel == 2 ? (2 * e0 + e1) / 3 : (2 * e1 + e0) / 3;
    return sel == 2 ? (e0 + e1) / 2 : ivec3(0);
}

// BC4 channel / BC3 alpha block (words w, w+1, 8 bytes: v0, v1, 16 x 3-bit indices from byte 2) for pixel i.
int bc4(int w, int i)
{
    uint w0 = word_at(w), w1 = word_at(w + 1);
    int  v0 = int(w0 & 255u), v1 = int((w0 >> 8u) & 255u);
    uint lo = (w0 >> 16u) | (w1 << 16u); // index bits 0..31
    uint hi = w1 >> 16u;                 // index bits 32..47
    uint bp = uint(3 * i);
    uint k  = bp + 3u <= 32u ? (lo >> bp) & 7u : (bp >= 32u ? (hi >> (bp - 32u)) & 7u : ((lo >> bp) | (hi << (32u - bp))) & 7u);
    int  c  = int(k);
    if (c == 0) return v0;
    if (c == 1) return v1;
    if (v0 > v1) return ((8 - c) * v0 + (c - 1) * v1) / 7;
    if (c == 6) return 0;
    if (c == 7) return 255;
    return ((6 - c) * v0 + (c - 1) * v1) / 5;
}

void main()
{
    if (!valid) {
        fragColor = vec4(0.0);
        return;
    }
    int x = int(gl_FragCoord.x);
    int y = int(gl_FragCoord.y); // texture row 0 = picture line 0, as every Caspar upload
    int i = (y & 3) * 4 + (x & 3);
    int block = (y >> 2) * (coded_w >> 2) + (x >> 2);

    if (fmt == 1) { // DXT1
        fragColor = vec4(vec3(bc1(tex + block * 2, i, false)) / 255.0, 1.0);
        return;
    }
    if (fmt == 2) { // DXT5, premultiplied: alpha half then colour half
        int w = tex + block * 4;
        fragColor = vec4(vec3(bc1(w + 2, i, true)), float(bc4(w, i))) / 255.0;
        return;
    }

    // DXV3 HQ: Y (+A) at full res, Co|Cg at half res.
    int Y, A = 255;
    if (fmt == 3) {
        Y = bc4(tex + block * 2, i);
    } else {
        int w = tex + block * 4;
        Y = bc4(w, i);
        A = bc4(w + 2, i);
    }
    int cbw = max((coded_w / 2) / 4, 1), cbh = max((coded_h / 2) / 4, 1);
    int cx = x >> 1, cy = y >> 1;
    int ci = (cy & 3) * 4 + (cx & 3);
    int cw = ctex + (min(cy >> 2, cbh - 1) * cbw + min(cx >> 2, cbw - 1)) * 4;
    int Co = bc4(cw, ci) - 128;
    int Cg = bc4(cw + 2, ci) - 128;
    ivec3 rgb = clamp(ivec3(Y + Co - Cg, Y + Cg, Y - Co - Cg), 0, 255);
    fragColor = vec4(vec3(rgb), float(A)) / 255.0; // straight alpha (desc.is_straight_alpha for yg10)
}
)GLSL";

struct dxv_program
{
    std::unique_ptr<shader> program;
    GLuint                  vao = 0;

    dxv_program()
        : program(new shader(vertex_source, fragment_source))
    {
        GL(glGenVertexArrays(1, &vao));
    }
    ~dxv_program() { glDeleteVertexArrays(1, &vao); }
};

// Created and used on the OpenGL device thread only (there is one device per process).
dxv_program& gl_program()
{
    static dxv_program* p = new dxv_program(); // never freed: lives as long as the GL context
    return *p;
}

bool has_header(const array<const std::uint8_t>& plane)
{
    return plane.size() >= dx::header_words * 4 && dx::read_word(plane.data(), dx::h_magic) == dx::magic;
}

int hdr(const array<const std::uint8_t>& plane, dx::header_field f)
{
    return static_cast<int>(dx::read_word(plane.data(), f));
}

} // namespace

bool dxv_decoded_desc(const array<const std::uint8_t>& plane, core::pixel_format_desc& desc)
{
    if (!has_header(plane))
        return false;
    const int w = hdr(plane, dx::h_width), h = hdr(plane, dx::h_height);
    if (w <= 0 || h <= 0)
        return false;
    desc                   = core::pixel_format_desc(core::pixel_format::bgra);
    desc.planes            = {core::pixel_format_desc::plane(w, h, 4, common::bit_depth::bit16)};
    desc.is_straight_alpha = hdr(plane, dx::h_valid) == 1 && hdr(plane, dx::h_format) == dx::fmt_yg10;
    return true;
}

std::shared_future<std::shared_ptr<texture>> dxv_decode_async(const spl::shared_ptr<device>&   ogl,
                                                              const array<const std::uint8_t>& plane,
                                                              const core::pixel_format_desc&   desc)
{
    const auto& p0 = desc.planes.at(0);
    auto        payload =
        ogl->copy_async(plane, p0.width, p0.height, p0.stride, p0.depth).share(); // queued before the decode below

    struct params
    {
        bool valid;
        int  fmt, width, height, coded_w, coded_h, tex, ctex;
    } u{};
    if (has_header(plane)) {
        u.valid   = hdr(plane, dx::h_valid) == 1;
        u.fmt     = hdr(plane, dx::h_format);
        u.width   = hdr(plane, dx::h_width);
        u.height  = hdr(plane, dx::h_height);
        u.coded_w = hdr(plane, dx::h_coded_w);
        u.coded_h = hdr(plane, dx::h_coded_h);
        u.tex     = hdr(plane, dx::h_tex);
        u.ctex    = hdr(plane, dx::h_ctex);
    }
    if (u.fmt < dx::fmt_dxt1 || u.fmt > dx::fmt_yg10 || u.coded_w < u.width || u.coded_h < u.height)
        u.valid = false;
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
            prog.program->set("fmt", u.fmt);
            prog.program->set("coded_w", u.coded_w);
            prog.program->set("coded_h", u.coded_h);
            prog.program->set("tex", u.tex);
            prog.program->set("ctex", u.ctex);

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
