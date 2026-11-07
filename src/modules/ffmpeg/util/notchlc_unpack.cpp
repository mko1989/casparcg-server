/*
 * HighAsCG WO-820: CPU half of the NotchLC GPU decode (see notchlc_unpack.h). Format and checks follow FFmpeg 8.0.1
 * libavcodec/notchlc.c (decode_frame l.469-527, decode_blocks l.154-227) and lzf.c.
 */

#include "notchlc_unpack.h"

#include <core/frame/notchlc.h>

#include <algorithm>
#include <climits>
#include <cstring>

namespace caspar { namespace ffmpeg { namespace notchlc {

namespace nl = core::notchlc;

static inline uint32_t rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

static inline void wr32(uint8_t* p, uint32_t v) { std::memcpy(p, &v, 4); }

// LZ4 block format into a flat buffer. Same result as notchlc.c lz4_decompress for valid streams.
static bool lz4_block(const uint8_t* ip, size_t n, uint8_t* dst, size_t cap, size_t& out)
{
    const uint8_t* const iend = ip + n;
    size_t               op   = 0;

    while (ip < iend) {
        const unsigned token = *ip++;
        size_t         lit   = token >> 4;
        if (lit == 15) {
            unsigned c;
            do {
                if (ip >= iend)
                    return false;
                c = *ip++;
                lit += c;
            } while (c == 255);
        }
        if (lit > static_cast<size_t>(iend - ip) || lit > cap - op)
            return false;
        std::memcpy(dst + op, ip, lit);
        ip += lit;
        op += lit;

        if (ip >= iend)
            break; // last sequence: literals only

        if (iend - ip < 2)
            return false;
        const size_t off = static_cast<size_t>(ip[0]) | static_cast<size_t>(ip[1]) << 8;
        ip += 2;
        if (off == 0 || off > op)
            return false;

        size_t ml = 4 + (token & 15);
        if ((token & 15) == 15) {
            unsigned c;
            do {
                if (ip >= iend)
                    return false;
                c = *ip++;
                ml += c;
            } while (c == 255);
        }
        if (ml > cap - op)
            return false;

        uint8_t*       d = dst + op;
        const uint8_t* s = d - off;
        if (off >= ml) {
            std::memcpy(d, s, ml);
        } else {
            for (size_t i = 0; i < ml; ++i)
                d[i] = s[i];
        }
        op += ml;
    }

    out = op;
    return true;
}

// LZF as FFmpeg's ff_lzf_uncompress (loop runs while more than 2 input bytes are left).
static bool lzf_block(const uint8_t* ip, size_t n, uint8_t* dst, size_t cap, size_t& out)
{
    const uint8_t* const iend = ip + n;
    size_t               op   = 0;

    while (iend - ip > 2) {
        const unsigned s = *ip++;
        if (s < 32) {
            const size_t lit = s + 1;
            if (lit > static_cast<size_t>(iend - ip) || lit > cap - op)
                return false;
            std::memcpy(dst + op, ip, lit);
            ip += lit;
            op += lit;
        } else {
            size_t l   = 2 + (s >> 5);
            size_t off = ((s & 0x1f) << 8) + 1;
            if (l == 7 + 2)
                l += *ip++;
            off += *ip++;
            if (off > op || l > cap - op)
                return false;
            for (size_t i = 0; i < l; ++i)
                dst[op + i] = dst[op - off + i];
            op += l;
        }
    }

    out = op;
    return true;
}

bool unpack(const uint8_t* packet,
            int            packet_size,
            int            width,
            int            height,
            uint8_t*       plane,
            size_t         plane_bytes,
            const char**   error)
{
    auto fail = [&](const char* msg) {
        *error = msg;
        return false;
    };

    std::memset(plane, 0, nl::header_words * 4);
    wr32(plane + nl::h_magic * 4, nl::magic);
    wr32(plane + nl::h_width * 4, static_cast<uint32_t>(width));
    wr32(plane + nl::h_height * 4, static_cast<uint32_t>(height));
    wr32(plane + nl::h_payload * 4, nl::header_words);

    if (!packet || packet_size <= 40)
        return fail("packet too small");
    if (rd32(packet) != (('N' << 24) | ('L' << 16) | ('C' << 8) | '1'))
        return fail("bad magic");

    const uint32_t usz    = rd32(packet + 4);
    const uint32_t format = rd32(packet + 12);
    if (format > 2)
        return fail("unknown format");

    const size_t nblk  = static_cast<size_t>((width + 3) / 4) * ((height + 3) / 4);
    const size_t nb16  = static_cast<size_t>((width + 15) / 16) * ((height + 15) / 16);
    const size_t avail = plane_bytes - nl::header_words * 4;
    if (static_cast<int64_t>(usz) > nl::max_payload_bytes(width, height) || usz + 4 + nblk * 4 > avail)
        return fail("payload larger than the plane");

    uint8_t* const pl  = plane + nl::header_words * 4;
    const uint8_t* src = packet + 16;
    const size_t   n   = static_cast<size_t>(packet_size) - 16;
    size_t         out = 0;

    if (format == 1) {
        if (n < usz / 255)
            return fail("lz4 input too short");
        if (!lz4_block(src, n, pl, usz, out) || out != usz)
            return fail("lz4 error");
    } else if (format == 0) {
        if (!lzf_block(src, n, pl, usz + 4 + nblk * 4, out) || out < usz)
            return fail("lzf error");
    } else {
        if (n < usz)
            return fail("stored payload short");
        std::memcpy(pl, src, usz);
    }

    if (usz < 40)
        return fail("payload header short");

    // decode_blocks l.164-227
    const uint32_t tx = rd32(pl), ty = rd32(pl + 4);
    if (tx != static_cast<uint32_t>(width) || ty != static_cast<uint32_t>(height))
        return fail("frame size differs from the stream");

    auto off4 = [&](int at, uint32_t& v) {
        v = rd32(pl + at);
        if (v >= UINT_MAX / 4)
            return false;
        v *= 4;
        return v < usz;
    };
    uint32_t uv_off, y_ctrl, a_ctrl, uv_data, a_data, a_count;
    if (!off4(8, uv_off) || !off4(12, y_ctrl) || !off4(16, a_ctrl) || !off4(20, uv_data))
        return fail("bad offsets");
    const uint32_t y_size = rd32(pl + 24);
    if (y_size >= UINT_MAX / 4)
        return fail("bad y size");
    if (!off4(28, a_data) || !off4(32, a_count))
        return fail("bad alpha offsets");
    const uint32_t data_end = rd32(pl + 36);
    if (data_end > usz || data_end <= y_size)
        return fail("bad data end");
    const uint32_t y_data = data_end - y_size;
    if (y_data <= a_data)
        return fail("bad y data offset");
    const bool opaque = (y_data - a_data) == a_ctrl;

    // Bounds the shader relies on.
    const size_t rows4 = static_cast<size_t>((height + 3) / 4);
    if (40 + rows4 * 4 > usz || y_ctrl + nblk * 4 > usz || uv_off + nb16 * 4 > usz ||
        (!opaque && a_ctrl + nb16 * 8 > usz))
        return fail("tables past the payload");

    // Luma block table: absolute bit position (in the plane) of each 4x4 block's bits.
    const uint32_t table = nl::header_words + (usz + 3) / 4;
    uint8_t* const tab   = plane + static_cast<size_t>(table) * 4;
    const size_t   bpr   = static_cast<size_t>((width + 3) / 4);
    for (size_t r = 0; r < rows4; ++r) {
        const uint32_t row_off = rd32(pl + 40 + r * 4);
        if (static_cast<uint64_t>(y_data) + row_off >= data_end)
            return fail("bad luma row offset");
        uint32_t       bit  = (nl::header_words * 4 + y_data + row_off) * 8;
        const uint8_t* ctrl = pl + y_ctrl + r * bpr * 4;
        uint8_t*       t    = tab + r * bpr * 4;
        for (size_t c = 0; c < bpr; ++c) {
            wr32(t + c * 4, bit);
            const uint32_t item = rd32(ctrl + c * 4);
            bit += 4 * (((item >> 24) & 3) + ((item >> 26) & 3) + ((item >> 28) & 3) + ((item >> 30) & 3) + 4);
        }
    }

    const uint32_t used = (table + static_cast<uint32_t>(nblk)) * 4;
    wr32(plane + nl::h_size * 4, usz);
    wr32(plane + nl::h_table * 4, table);
    wr32(plane + nl::h_used * 4, used);
    wr32(plane + nl::h_y_ctrl * 4, y_ctrl);
    wr32(plane + nl::h_uv_off * 4, uv_off);
    wr32(plane + nl::h_uv_data * 4, uv_data);
    wr32(plane + nl::h_a_ctrl * 4, a_ctrl);
    wr32(plane + nl::h_a_data * 4, a_data);
    wr32(plane + nl::h_y_data * 4, y_data);
    wr32(plane + nl::h_opaque * 4, opaque ? 1 : 0);
    wr32(plane + nl::h_valid * 4, 1);
    return true;
}

}}} // namespace caspar::ffmpeg::notchlc
