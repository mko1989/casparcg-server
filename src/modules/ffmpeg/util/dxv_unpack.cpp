/*
 * HighAsCG: CPU half of the native DXV GPU decode (see dxv_unpack.h). A line-by-line port of the texture
 * decompression stage of FFmpeg 8.0.1 libavcodec/dxv.c (dxv_decode l.846-1018 and everything it calls up to, but
 * not including, texturedsp) and libavcodec/lzf.c. Line numbers below refer to those n8.0.1 files.
 *
 * Original code:
 *   Resolume DXV decoder
 *   Copyright (C) 2015 Vittorio Giovara <vittorio.giovara@gmail.com>
 *   Copyright (C) 2018 Paul B Mahol
 *
 *   lzf decompression algorithm
 *   Copyright (c) 2015 Luca Barbato
 *
 *   This file is part of FFmpeg.
 *
 *   FFmpeg is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   FFmpeg is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with FFmpeg; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 *
 * This port is distributed with CasparCG under the GNU GPL v3 (LGPL-2.1+ permits relicensing as GPL).
 *
 * Porting rules (so the output is byte-identical to FFmpeg, also for damaged packets FFmpeg accepts):
 *   - `reader` reproduces GetByteContext exactly: reads past the end return 0 and park the cursor at the end, skip /
 *     seek clip, get_buffer copies what is there.
 *   - Integer arithmetic that can overflow `int` in FFmpeg (run lengths built from 0xFFFF chains) is done in uint32
 *     and converted back, i.e. the wrap-around FFmpeg gets in practice, without the UB.
 *   - Pointers into tex_data become byte offsets; hash-table "NULL" is -1.
 *   - Where FFmpeg writes 8 bytes and only then checks they were inside the texture (dxv_decompress_cgo l.529) we
 *     check first; the verdict is identical and nothing is written outside `tex`.
 *   - lzf.c grows the output buffer without bound; we stop storing at tex_size but keep counting, so accept/reject
 *     decisions (back-reference distance checks) are unchanged and memory stays bounded.
 */

#include "dxv_unpack.h"

#include <algorithm>
#include <climits>
#include <cstring>

namespace caspar { namespace ffmpeg { namespace dxv {

namespace {

// dxv.h: MKBETAG('D','X','T','1') etc., compared against a little-endian 32-bit read of the packet start.
constexpr uint32_t mkbetag(char a, char b, char c, char d)
{
    return static_cast<uint32_t>(static_cast<uint8_t>(d)) | static_cast<uint32_t>(static_cast<uint8_t>(c)) << 8 |
           static_cast<uint32_t>(static_cast<uint8_t>(b)) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(a)) << 24;
}
constexpr uint32_t FMT_DXT1 = mkbetag('D', 'X', 'T', '1');
constexpr uint32_t FMT_DXT5 = mkbetag('D', 'X', 'T', '5');
constexpr uint32_t FMT_YCG6 = mkbetag('Y', 'C', 'G', '6');
constexpr uint32_t FMT_YG10 = mkbetag('Y', 'G', '1', '0');

inline uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | p[1] << 8); }
inline uint32_t rd32(const uint8_t* p)
{
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}
inline void wr16(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
}
inline void wr32(uint8_t* p, uint32_t v)
{
    p[0] = static_cast<uint8_t>(v);
    p[1] = static_cast<uint8_t>(v >> 8);
    p[2] = static_cast<uint8_t>(v >> 16);
    p[3] = static_cast<uint8_t>(v >> 24);
}

// libavcodec/bytestream.h GetByteContext semantics.
struct reader
{
    const uint8_t* start;
    const uint8_t* cur;
    const uint8_t* end;

    unsigned left() const { return static_cast<unsigned>(end - cur); } // bytestream2_get_bytes_left
    int      tell() const { return static_cast<int>(cur - start); }    // bytestream2_tell

    uint8_t u8()
    {
        if (end - cur < 1) {
            cur = end;
            return 0;
        }
        return *cur++;
    }
    uint8_t  peek_u8() const { return end - cur < 1 ? 0 : *cur; }
    uint16_t le16()
    {
        if (end - cur < 2) {
            cur = end;
            return 0;
        }
        const uint16_t v = rd16(cur);
        cur += 2;
        return v;
    }
    uint32_t le32()
    {
        if (end - cur < 4) {
            cur = end;
            return 0;
        }
        const uint32_t v = rd32(cur);
        cur += 4;
        return v;
    }
    void skip(unsigned n) { cur += std::min<size_t>(static_cast<size_t>(end - cur), n); } // bytestream2_skip
    void seek_set(int64_t off)                                                           // bytestream2_seek SEEK_SET
    {
        cur = start + std::clamp<int64_t>(off, 0, end - start);
    }
    void seek_cur(int64_t off) // bytestream2_seek SEEK_CUR
    {
        cur += std::clamp<int64_t>(off, -(cur - start), end - cur);
    }
    size_t get_buffer(uint8_t* dst, size_t n) // bytestream2_get_buffer
    {
        const size_t k = std::min<size_t>(static_cast<size_t>(end - cur), n);
        if (k)
            std::memcpy(dst, cur, k);
        cur += k;
        return k;
    }
};

// dxv.c CHECKPOINT(x) l.53-89. Returns the 2-bit op (0..3) or -1 on error; sets idx for op 1..3.
inline int checkpoint(reader& g, uint32_t& value, int& state, int64_t& idx, int64_t pos, int x)
{
    if (state == 0) {
        if (g.left() < 4)
            return -1;
        value = g.le32();
        state = 16;
    }
    const int op = static_cast<int>(value & 0x3);
    value >>= 2;
    state--;
    switch (op) {
        case 1:
            idx = x;
            break;
        case 2:
            idx = (static_cast<int64_t>(g.u8()) + 2) * x;
            if (idx > pos)
                return -1;
            break;
        case 3:
            idx = (static_cast<int64_t>(g.le16()) + 0x102) * x;
            if (idx > pos)
                return -1;
            break;
        default:
            break;
    }
    return op;
}

// dxv.c dxv_decompress_dxt1 l.91-138. Elements are 32-bit words; pos/idx count words.
bool decompress_dxt1(reader& g, uint8_t* tex, size_t tex_size)
{
    uint32_t      value = 0;
    int           state = 0;
    int64_t       idx   = 0;
    int64_t       pos   = 2;
    const int64_t n     = static_cast<int64_t>(tex_size / 4);

    wr32(tex, g.le32());
    wr32(tex + 4, g.le32());

    while (pos + 2 <= n) {
        int op = checkpoint(g, value, state, idx, pos, 2);
        if (op < 0)
            return false;
        if (op) {
            wr32(tex + 4 * pos, rd32(tex + 4 * (pos - idx)));
            pos++;
            wr32(tex + 4 * pos, rd32(tex + 4 * (pos - idx)));
            pos++;
        } else {
            op = checkpoint(g, value, state, idx, pos, 2);
            if (op < 0)
                return false;
            wr32(tex + 4 * pos, op ? rd32(tex + 4 * (pos - idx)) : g.le32());
            pos++;

            op = checkpoint(g, value, state, idx, pos, 2);
            if (op < 0)
                return false;
            wr32(tex + 4 * pos, op ? rd32(tex + 4 * (pos - idx)) : g.le32());
            pos++;
        }
    }
    return true;
}

// dxv.c dxv_decompress_dxt5 l.671-826.
bool decompress_dxt5(reader& g, uint8_t* tex, size_t tex_size)
{
    uint32_t      value = 0;
    int           state = 0;
    int64_t       idx   = 0;
    int64_t       pos   = 4;
    uint32_t      run   = 0; // int in FFmpeg; only tested for != 0 and decremented
    const int64_t n     = static_cast<int64_t>(tex_size / 4);

    auto copy_back = [&](int64_t back) {
        wr32(tex + 4 * pos, rd32(tex + 4 * (pos - back)));
        pos++;
    };

    wr32(tex + 0, g.le32());
    wr32(tex + 4, g.le32());
    wr32(tex + 8, g.le32());
    wr32(tex + 12, g.le32());

    while (pos + 2 <= n) {
        if (run) {
            run--;
            copy_back(4);
            copy_back(4);
        } else {
            if (g.left() < 1)
                return false;
            if (state == 0) {
                value = g.le32();
                state = 16;
            }
            const uint32_t op0 = value & 0x3;
            value >>= 2;
            state--;

            switch (op0) {
                case 0: { // long copy (l.710-741)
                    uint32_t check = static_cast<uint32_t>(g.u8()) + 1;
                    if (check == 256) {
                        uint32_t probe;
                        do {
                            probe = g.le16();
                            check += probe;
                        } while (probe == 0xFFFF);
                    }
                    while (check && pos + 4 <= n) {
                        copy_back(4);
                        copy_back(4);
                        copy_back(4);
                        copy_back(4);
                        check--;
                    }
                    continue;
                }
                case 1: { // new run value (l.742-760)
                    run = g.u8();
                    if (run == 255) {
                        uint32_t probe;
                        do {
                            probe = g.le16();
                            run += probe;
                        } while (probe == 0xFFFF);
                    }
                    copy_back(4);
                    copy_back(4);
                    break;
                }
                case 2: // copy two words from a previous index (l.761-773)
                    idx = 8 + 4 * static_cast<int64_t>(g.le16());
                    if (idx > pos || (pos - idx) + 2 > n)
                        return false;
                    copy_back(idx);
                    copy_back(idx);
                    break;
                default: // 3: two words from the input (l.774-783)
                    wr32(tex + 4 * pos, g.le32());
                    pos++;
                    wr32(tex + 4 * pos, g.le32());
                    pos++;
                    break;
            }
        }

        int op = checkpoint(g, value, state, idx, pos, 4);
        if (op < 0)
            return false;
        if (pos + 2 > n)
            return false;

        if (op) {
            if (idx > pos || (pos - idx) + 2 > n)
                return false;
            copy_back(idx);
            copy_back(idx);
        } else {
            op = checkpoint(g, value, state, idx, pos, 4);
            if (op < 0)
                return false;
            if (op && (idx > pos || (pos - idx) + 2 > n))
                return false;
            wr32(tex + 4 * pos, op ? rd32(tex + 4 * (pos - idx)) : g.le32());
            pos++;

            op = checkpoint(g, value, state, idx, pos, 4);
            if (op < 0)
                return false;
            wr32(tex + 4 * pos, op ? rd32(tex + 4 * (pos - idx)) : g.le32());
            pos++;
        }
    }
    return true;
}

// lzf.c ff_lzf_uncompress l.55-104. Output beyond tex_size is counted, not stored (FFmpeg stores it in a grown
// buffer that texturedsp never reads). Bytes of tex past the produced length are left as they were.
bool decompress_lzf(reader& g, uint8_t* tex, size_t tex_size)
{
    constexpr int LZF_LITERAL_MAX  = 1 << 5;
    constexpr int LZF_LONG_BACKREF = 7 + 2; // lzf.c `#define LZF_LONG_BACKREF 7 + 2` (used as `l == 7 + 2`)
    int64_t       len              = 0;

    while (g.left() > 2) {
        int s = g.u8();
        if (s < LZF_LITERAL_MAX) {
            s++;
            // bytestream2_get_buffer copies what is there; a short read is then AVERROR_INVALIDDATA (the partial
            // copy stays in the buffer, as in FFmpeg).
            const int avail = static_cast<int>(std::min<unsigned>(g.left(), static_cast<unsigned>(s)));
            for (int i = 0; i < avail; i++) {
                const uint8_t c = *g.cur++;
                if (len + i < static_cast<int64_t>(tex_size))
                    tex[len + i] = c;
            }
            if (avail != s)
                return false;
            len += s;
        } else {
            int l   = 2 + (s >> 5);
            int off = ((s & 0x1f) << 8) + 1;
            if (l == LZF_LONG_BACKREF)
                l += g.u8();
            off += g.u8();
            if (off > len)
                return false;
            // av_memcpy_backptr(p, off, l): overlapping forward copy == byte-by-byte copy from p - off.
            for (int i = 0; i < l; i++, len++) {
                if (len < static_cast<int64_t>(tex_size))
                    tex[len] = tex[len - off];
            }
        }
    }
    return true;
}

// dxv.c fill_ltable l.146-188.
bool fill_ltable(reader& g, uint32_t* table, int& nb_elements)
{
    uint32_t half = 512, bits = 1023, left = 1024, input, mask;
    int      value, counter = 0, rshift = 10, lshift = 30;

    mask = g.le32() >> 2;
    while (left) {
        if (counter >= 256)
            return false;
        value = static_cast<int>(bits & mask);
        left -= bits & mask;
        mask >>= rshift; // rshift is 1..10 while the loop runs
        lshift -= rshift;
        table[counter++] = static_cast<uint32_t>(value);
        if (lshift < 16) { // lshift is 6..15 here
            if (g.left() <= 0)
                return false;
            input = g.le16();
            mask += input << lshift;
            lshift += 16;
        }
        if (left < half) {
            half >>= 1;
            bits >>= 1;
            rshift--;
        }
    }

    // l.175: `for (; !table[counter - 1]; counter--) if (counter <= 0) error;` (a successful loop exit implies a
    // non-zero entry, so FFmpeg never indexes table[-1]; we bound it anyway).
    while (counter > 0 && !table[counter - 1])
        counter--;
    if (counter <= 0)
        return false;

    nb_elements = counter;
    if (counter < 256)
        std::memset(&table[counter], 0, 4 * (256 - counter));

    if (lshift >= 16)
        g.seek_cur(-2);
    return true;
}

inline int floor_log2(uint32_t v) // 31 - ff_clz(v), v > 0
{
    int r = 0;
    while (v >>= 1)
        r++;
    return r;
}

// dxv.c fill_optable l.190-229.
bool fill_optable(const uint32_t* table0, scratch::opcode_entry* table1, int nb_elements)
{
    uint32_t table2[256] = {0};
    unsigned x           = 0;
    int      i, j, k = 0;

    table2[0] = table0[0];
    for (i = 0; i < nb_elements - 1; i++)
        table2[i + 1] = table0[i + 1] + table2[i];

    if (!table2[0]) {
        do {
            k++;
        } while (k < 256 && !table2[k]); // FFmpeg has no k < 256 bound (unreachable: table sums to 1024)
        if (k >= 256)
            return false;
    }

    j = 2;
    for (i = 1024; i > 0; i--) {
        table1[x].val1 = static_cast<uint8_t>(k);
        for (; k < 256 && static_cast<unsigned>(j) > table2[k]; k++)
            ;
        x = (x - 383) & 0x3FF;
        j++;
    }

    if (nb_elements > 0)
        std::memcpy(&table2[0], table0, 4 * static_cast<size_t>(nb_elements));

    for (i = 0; i < 1024; i++) {
        const int      val0 = table1[i].val1;
        const uint32_t val1 = table2[val0];
        table2[val0]++;
        if (val1 == 0) // ff_clz(0) is undefined in FFmpeg; unreachable for tables fill_ltable accepts
            return false;
        x = static_cast<unsigned>(floor_log2(val1));
        if (x > 10)
            return false;
        table1[i].val2 = static_cast<uint8_t>(10 - x);
        table1[i].next = static_cast<int16_t>(static_cast<int>(val1 << table1[i].val2) - 1024);
    }
    return true;
}

// dxv.c get_opcodes l.231-271 (an FSE/tANS decoder reading the bit stream backwards).
bool get_opcodes(reader& g, scratch& s, uint8_t* dst, int op_size, int nb_elements)
{
    const uint8_t* src = g.cur;

    if (!fill_optable(s.ltable, s.optable, nb_elements))
        return false;

    const int64_t  size_in_bits = g.le32();
    const uint32_t endoffset    = static_cast<uint32_t>(((size_in_bits + 7) >> 3) - 4);
    if (static_cast<int32_t>(endoffset) <= 0 || g.left() < endoffset)
        return false;

    // src + endoffset + 4 <= end here, and offset stays in [0, endoffset], so every rd32 below is in bounds.
    uint32_t offset = endoffset;
    uint32_t next   = rd32(src + endoffset);
    const int rshift = static_cast<int>((((size_in_bits & 0xFF) - 1) & 7) + 15);
    int       lshift = 32 - rshift;
    int       idx    = static_cast<int>((next >> rshift) & 0x3FF);
    for (int i = 0; i < op_size; i++) {
        dst[i]                = s.optable[idx].val1;
        const int      val    = s.optable[idx].val2;
        const int      sum    = val + lshift;
        const int      x      = static_cast<int>((next << lshift) >> 1 >> (31 - val));
        const uint32_t newoff = offset - static_cast<uint32_t>(sum >> 3);
        lshift                = sum & 7;
        idx                   = (x + s.optable[idx].next) & 0x3FF; // provably < 1024 already; mask is a no-op
        offset                = newoff;
        if (offset > endoffset)
            return false;
        next = rd32(src + offset);
    }

    g.skip(endoffset); // bytestream2_skip(gb, (size_in_bits + 7 >> 3) - 4)
    return true;
}

// dxv.c dxv_decompress_opcodes l.273-298. Returns bytes consumed or -1.
int decompress_opcodes(reader& g, scratch& s, uint8_t* dst, size_t op_size)
{
    const int pos  = g.tell();
    const int flag = g.peek_u8();

    if ((flag & 3) == 0) {
        g.skip(1);
        if (g.get_buffer(dst, op_size) != op_size)
            return -1;
    } else if ((flag & 3) == 1) {
        g.skip(1);
        const uint8_t v = g.u8();
        if (op_size)
            std::memset(dst, v, op_size);
    } else {
        int elements = 0;
        if (!fill_ltable(g, s.ltable, elements))
            return -1;
        if (!get_opcodes(g, s, dst, static_cast<int>(op_size), elements))
            return -1;
    }
    return g.tell() - pos;
}

inline unsigned hash16(uint32_t v16) { return (0x9E3779B1u * v16) >> 24; }              // 0x9E3779B1 * (uint16) >> 24
inline unsigned hash24(const uint8_t* p) { return (0x9E3779B1u * (rd32(p) & 0xFFFFFFu)) >> 24; } // RL32(p) & 0xFFFFFF

// State of one opcode stream of dxv_decompress_cgo (oindex, state, tab0, tab1).
struct cgo_stream
{
    const uint8_t* op;
    int            op_size;
    int            oi;
    int            state;
    int32_t*       tab0; // offsets of 8-byte elements, keyed by hash of their first 16 bits; -1 = NULL
    int32_t*       tab1; // offsets of element+2, keyed by hash of bytes 2..4
};

// dxv.c dxv_decompress_cgo l.300-538. Produces one 8-byte element at tex + dst.
bool decompress_cgo(reader& g, uint8_t* tex, size_t tex_size, cgo_stream& st, size_t& dstpos, int offset)
{
    const size_t dst = dstpos;
    if (dst + 8 > tex_size) // FFmpeg l.529 (checked after the write there; same verdict)
        return false;
    uint8_t* const    d    = tex + dst;
    const int         back = 8 + offset; // dst >= back: yo starts at 8 (offset 0), cocg at 16 (offset 8)
    int32_t* const    tab0 = st.tab0;
    int32_t* const    tab1 = st.tab1;
    const int32_t     cur  = static_cast<int32_t>(dst);

    auto copy_prev = [&] {
        wr32(d, rd32(d - back));
        wr32(d + 4, rd32(d - back + 4));
    };
    auto upd0 = [&] { tab0[hash16(rd16(d))] = cur; };
    auto upd1 = [&] { tab1[hash24(d + 2)] = cur + 2; };
    // Back-reference vv elements' worth of bytes: valid if 0 <= vv <= dst.
    auto backref = [&](int64_t& vv) -> bool {
        vv = static_cast<int64_t>(8 + offset) * (static_cast<int64_t>(g.le16()) + 1);
        return vv >= 0 && vv <= static_cast<int64_t>(dst);
    };
    auto tab = [&](int32_t* t, int32_t& o) -> bool {
        o = t[g.u8()];
        return o >= 0;
    };

    if (st.state <= 0) {
        if (st.oi >= st.op_size)
            return false;
        const int opcode = st.op[st.oi++];
        if (!opcode) {
            uint32_t v = g.u8();
            if (v == 255) {
                uint32_t o;
                do {
                    if (g.left() <= 0)
                        return false;
                    o = g.le16();
                    v += o;
                } while (o == 0xFFFF);
            }
            copy_prev();
            // state = v + 4; goto done -> identical copy again, state-- (int wrap-around as in practice)
            st.state = static_cast<int32_t>(v + 3u);
        } else {
            int32_t t0, t1, t3;
            int64_t vv;
            switch (opcode) {
                case 1:
                    copy_prev();
                    break;
                case 2: {
                    if (!backref(vv))
                        return false;
                    const uint8_t* p = d - vv;
                    wr32(d, rd32(p));
                    wr32(d + 4, rd32(p + 4));
                    upd0();
                    upd1();
                    break;
                }
                case 3:
                    wr32(d, g.le32());
                    wr32(d + 4, g.le32());
                    upd0();
                    upd1();
                    break;
                case 4:
                    if (!tab(tab1, t3))
                        return false;
                    wr16(d, g.le16());
                    wr16(d + 2, rd16(tex + t3));
                    d[4] = tex[t3 + 2];
                    wr16(d + 5, g.le16());
                    d[7] = g.u8();
                    upd0();
                    break;
                case 5:
                    if (!tab(tab1, t3))
                        return false;
                    wr16(d, g.le16());
                    wr16(d + 2, g.le16());
                    d[4] = g.u8();
                    wr16(d + 5, rd16(tex + t3));
                    d[7] = tex[t3 + 2];
                    upd0();
                    upd1();
                    break;
                case 6:
                    if (!tab(tab1, t0))
                        return false;
                    if (!tab(tab1, t1))
                        return false;
                    wr16(d, g.le16());
                    wr16(d + 2, rd16(tex + t0));
                    d[4] = tex[t0 + 2];
                    wr16(d + 5, rd16(tex + t1));
                    d[7] = tex[t1 + 2];
                    upd0();
                    break;
                case 7: {
                    if (!backref(vv))
                        return false;
                    const uint8_t* p = d - vv;
                    wr16(d, g.le16());
                    wr16(d + 2, rd16(p + 2));
                    wr32(d + 4, rd32(p + 4));
                    upd0();
                    upd1();
                    break;
                }
                case 8:
                    if (!tab(tab0, t1))
                        return false;
                    wr16(d, rd16(tex + t1));
                    wr16(d + 2, g.le16());
                    wr32(d + 4, g.le32());
                    upd1();
                    break;
                case 9:
                    if (!tab(tab0, t1))
                        return false;
                    if (!tab(tab1, t3))
                        return false;
                    wr16(d, rd16(tex + t1));
                    wr16(d + 2, rd16(tex + t3));
                    d[4] = tex[t3 + 2];
                    wr16(d + 5, g.le16());
                    d[7] = g.u8();
                    upd1();
                    break;
                case 10:
                    if (!tab(tab0, t1))
                        return false;
                    if (!tab(tab1, t3))
                        return false;
                    wr16(d, rd16(tex + t1));
                    wr16(d + 2, g.le16());
                    d[4] = g.u8();
                    wr16(d + 5, rd16(tex + t3));
                    d[7] = tex[t3 + 2];
                    upd1();
                    break;
                case 11:
                    if (!tab(tab0, t0))
                        return false;
                    if (!tab(tab1, t3))
                        return false;
                    if (!tab(tab1, t1))
                        return false;
                    wr16(d, rd16(tex + t0));
                    wr16(d + 2, rd16(tex + t3));
                    d[4] = tex[t3 + 2];
                    wr16(d + 5, rd16(tex + t1));
                    d[7] = tex[t1 + 2];
                    break;
                case 12: {
                    if (!tab(tab0, t1))
                        return false;
                    if (!backref(vv))
                        return false;
                    const uint8_t* p = d - vv;
                    wr16(d, rd16(tex + t1));
                    wr16(d + 2, rd16(p + 2));
                    wr32(d + 4, rd32(p + 4));
                    upd1();
                    break;
                }
                case 13:
                    wr16(d, rd16(d - back));
                    wr16(d + 2, g.le16());
                    wr32(d + 4, g.le32());
                    upd1();
                    break;
                case 14:
                    if (!tab(tab1, t3))
                        return false;
                    wr16(d, rd16(d - back));
                    wr16(d + 2, rd16(tex + t3));
                    d[4] = tex[t3 + 2];
                    wr16(d + 5, g.le16());
                    d[7] = g.u8();
                    upd1();
                    break;
                case 15:
                    if (!tab(tab1, t3))
                        return false;
                    wr16(d, rd16(d - back));
                    wr16(d + 2, g.le16());
                    d[4] = g.u8();
                    wr16(d + 5, rd16(tex + t3));
                    d[7] = tex[t3 + 2];
                    upd1();
                    break;
                case 16:
                    if (!tab(tab1, t3))
                        return false;
                    if (!tab(tab1, t1))
                        return false;
                    wr16(d, rd16(d - back));
                    wr16(d + 2, rd16(tex + t3));
                    d[4] = tex[t3 + 2];
                    wr16(d + 5, rd16(tex + t1));
                    d[7] = tex[t1 + 2];
                    break;
                case 17: {
                    if (!backref(vv))
                        return false;
                    const uint8_t* p = d - vv;
                    wr16(d, rd16(d - back));
                    wr16(d + 2, rd16(p + 2));
                    wr32(d + 4, rd32(p + 4));
                    upd1();
                    break;
                }
                default: // 18..255: FFmpeg writes nothing; the element keeps the buffer's previous bytes
                    break;
            }
        }
    } else {
        copy_prev();
        st.state--;
    }

    dstpos = dst + 8;
    return true;
}

// dxv.c dxv_decompress_cocg l.540-594 (two interleaved opcode streams, 16-byte BC5 blocks).
bool decompress_cocg(reader& g,
                     scratch& s,
                     uint8_t* tex,
                     size_t   tex_size,
                     uint8_t* op_data0,
                     uint8_t* op_data1,
                     int      max_op_size0,
                     int      max_op_size1)
{
    int32_t tab0[256], tab1[256], tab2[256], tab3[256];
    std::fill(std::begin(tab0), std::end(tab0), -1);
    std::fill(std::begin(tab1), std::end(tab1), -1);
    std::fill(std::begin(tab2), std::end(tab2), -1);
    std::fill(std::begin(tab3), std::end(tab3), -1);

    const int32_t  op_offset  = static_cast<int32_t>(g.le32());
    const uint32_t op_size0   = g.le32();
    const uint32_t op_size1   = g.le32();
    const int      data_start = g.tell();

    if (op_offset < 12 || static_cast<uint32_t>(op_offset - 12) > g.left())
        return false;

    g.skip(static_cast<unsigned>(op_offset - 12));
    if (op_size0 > static_cast<uint32_t>(max_op_size0))
        return false;
    const int skip0 = decompress_opcodes(g, s, op_data0, op_size0);
    if (skip0 < 0)
        return false;
    if (op_size1 > static_cast<uint32_t>(max_op_size1))
        return false;
    const int skip1 = decompress_opcodes(g, s, op_data1, op_size1);
    if (skip1 < 0)
        return false;
    g.seek_set(data_start);

    uint8_t first[16];
    wr32(first, g.le32());
    wr32(first + 4, g.le32());
    wr32(first + 8, g.le32());
    wr32(first + 12, g.le32());
    if (tex_size >= 16) { // else FFmpeg writes these into its buffer padding and the loop below never runs
        std::memcpy(tex, first, 16);
        tab0[hash16(rd16(tex))]      = 0;
        tab1[hash24(tex + 2)]        = 2;
        tab2[hash16(rd16(tex + 8))]  = 8;
        tab3[hash24(tex + 10)]       = 10;
    }

    cgo_stream a{op_data0, static_cast<int>(op_size0), 0, 0, tab0, tab1};
    cgo_stream b{op_data1, static_cast<int>(op_size1), 0, 0, tab2, tab3};
    size_t     dst = 16;
    while (dst + 10 < tex_size) {
        if (!decompress_cgo(g, tex, tex_size, a, dst, 8))
            return false;
        if (!decompress_cgo(g, tex, tex_size, b, dst, 8))
            return false;
    }

    g.seek_set(static_cast<int64_t>(data_start) - 12 + op_offset + skip0 + skip1);
    return true;
}

// dxv.c dxv_decompress_yo l.596-636 (one opcode stream, 8-byte BC4 blocks).
bool decompress_yo(reader& g, scratch& s, uint8_t* tex, size_t tex_size, uint8_t* op_data, int max_op_size)
{
    int32_t table0[256], table1[256];
    std::fill(std::begin(table0), std::end(table0), -1);
    std::fill(std::begin(table1), std::end(table1), -1);

    const int32_t  op_offset  = static_cast<int32_t>(g.le32());
    const uint32_t op_size    = g.le32();
    const int      data_start = g.tell();

    if (op_offset < 8 || static_cast<uint32_t>(op_offset - 8) > g.left())
        return false;

    g.skip(static_cast<unsigned>(op_offset - 8));
    if (op_size > static_cast<uint32_t>(max_op_size))
        return false;
    const int skip = decompress_opcodes(g, s, op_data, op_size);
    if (skip < 0)
        return false;
    g.seek_set(data_start);

    if (tex_size < 8) // unreachable (coded_height >= 8 -> tex_size >= 16)
        return false;
    const uint32_t v  = g.le32();
    wr32(tex, v);
    const uint32_t vv = g.le32();
    table0[hash16(v & 0xFFFF)] = 0;
    wr32(tex + 4, vv);
    table1[hash24(tex + 2)] = 2;

    cgo_stream st{op_data, static_cast<int>(op_size), 0, 0, table0, table1};
    size_t     dst = 8;
    while (dst < tex_size) {
        if (!decompress_cgo(g, tex, tex_size, st, dst, 0))
            return false;
    }

    g.seek_set(static_cast<int64_t>(data_start) + op_offset + skip - 8);
    return true;
}

bool decompress_raw(reader& g, uint8_t* tex, size_t tex_size) // dxv.c dxv_decompress_raw l.834-844
{
    if (g.left() < tex_size)
        return false;
    g.get_buffer(tex, tex_size);
    return true;
}

enum class method
{
    raw,
    lzf,
    dxt1,
    dxt5,
    ycg6,
    yg10
};

} // namespace

bool unpack(const uint8_t*        pkt,
            size_t                size,
            int                   coded_width,
            int                   coded_height,
            layout&               out,
            std::vector<uint8_t>& tex,
            std::vector<uint8_t>& ctex,
            scratch&              s,
            std::string&          error)
{
    out = layout{};
    error.clear();

    if (!pkt || size == 0 || size > static_cast<size_t>(INT_MAX)) {
        error = "empty or oversized packet";
        return false;
    }
    if (coded_width <= 0 || coded_height <= 0 || (coded_width & 3) || (coded_height & 3)) {
        error = "coded size must be positive multiples of 4";
        return false;
    }
    // av_image_check_size() in dxv_init (bounds every size below well inside int).
    if (static_cast<uint64_t>(coded_width + 128) * static_cast<uint64_t>(coded_height + 128) >= INT_MAX / 8) {
        error = "invalid image size";
        return false;
    }

    reader g{pkt, pkt, pkt + size};

    method     decompress;
    tex_format fmt;
    int        old_type = 0;
    uint32_t   hdr_size = 0;

    const uint32_t tag = g.le32();
    switch (tag) {
        case FMT_DXT1:
            decompress = method::dxt1;
            fmt        = tex_format::dxt1;
            break;
        case FMT_DXT5:
            decompress = method::dxt5;
            fmt        = tex_format::dxt5;
            break;
        case FMT_YCG6:
            decompress = method::ycg6;
            fmt        = tex_format::ycg6;
            break;
        case FMT_YG10:
            decompress = method::yg10;
            fmt        = tex_format::yg10;
            break;
        default: {
            // Old version (DXV2) has no real header, just size and type (l.909-941).
            hdr_size                = tag & 0x00FFFFFF;
            old_type                = static_cast<int>(tag >> 24);
            const int version_major = (old_type & 0x0F) - 1;
            decompress              = (old_type & 0x80) ? method::raw : method::lzf;
            if (old_type & 0x40) {
                fmt = tex_format::dxt5;
            } else if ((old_type & 0x20) || version_major == 1) {
                fmt = tex_format::dxt1;
            } else {
                error = "unsupported header";
                return false;
            }
            break;
        }
    }
    if (coded_height / 2 / 4 < 1) { // l.943
        error = "coded height < 8";
        return false;
    }

    if (!old_type) { // new 12-byte header (l.951-964)
        g.u8();      // version_major + 1
        g.u8();      // version_minor
        if (g.u8())  // "encoder copies texture data when compression is not advantageous"
            decompress = method::raw;
        g.skip(1);
        hdr_size = g.le32();
    }
    if (hdr_size != g.left()) { // l.969 (int vs unsigned compare -> unsigned)
        error = "incomplete or invalid packet (header size mismatch)";
        return false;
    }

    // Sizes, l.976-1015, same integer expression order.
    const int64_t w   = coded_width;
    const int64_t h   = coded_height;
    const bool    yuv = fmt == tex_format::ycg6 || fmt == tex_format::yg10;
    int64_t       tex_ratio;
    switch (fmt) {
        case tex_format::dxt1:
        case tex_format::ycg6:
            tex_ratio = 8;
            break;
        default:
            tex_ratio = 16;
            break;
    }
    // raw_ratio / (rgba ? 4 : 1) is 4 for every format.
    const int64_t tex_size  = w / 4 * h / 4 * tex_ratio;
    const int64_t ctex_size = yuv ? w / 2 / 4 * h / 2 / 4 * 16 : 0;
    int64_t       op_size[4] = {w * h / 16, w * h / 32, w * h / 32, w * h / 16};

    tex.resize(static_cast<size_t>(tex_size));
    ctex.resize(static_cast<size_t>(ctex_size));
    if (yuv)
        for (int i = 0; i < 4; i++)
            if (s.op_data[i].size() < static_cast<size_t>(op_size[i]))
                s.op_data[i].resize(static_cast<size_t>(op_size[i]));

    out.fmt       = fmt;
    out.width     = coded_width;
    out.height    = coded_height;
    out.tex_size  = static_cast<size_t>(tex_size);
    out.ctex_size = static_cast<size_t>(ctex_size);

    bool ok = false;
    switch (decompress) {
        case method::raw:
            ok = decompress_raw(g, tex.data(), tex.size());
            break;
        case method::lzf:
            ok = decompress_lzf(g, tex.data(), tex.size());
            break;
        case method::dxt1:
            ok = decompress_dxt1(g, tex.data(), tex.size());
            break;
        case method::dxt5:
            ok = decompress_dxt5(g, tex.data(), tex.size());
            break;
        case method::ycg6: // dxv_decompress_ycg6 l.638-652
            ok = decompress_yo(g, s, tex.data(), tex.size(), s.op_data[0].data(), static_cast<int>(op_size[0])) &&
                 decompress_cocg(g,
                                 s,
                                 ctex.data(),
                                 ctex.size(),
                                 s.op_data[1].data(),
                                 s.op_data[2].data(),
                                 static_cast<int>(op_size[1]),
                                 static_cast<int>(op_size[2]));
            break;
        case method::yg10: // dxv_decompress_yg10 l.654-669
            ok = decompress_cocg(g,
                                 s,
                                 tex.data(),
                                 tex.size(),
                                 s.op_data[0].data(),
                                 s.op_data[3].data(),
                                 static_cast<int>(op_size[0]),
                                 static_cast<int>(op_size[3])) &&
                 decompress_cocg(g,
                                 s,
                                 ctex.data(),
                                 ctex.size(),
                                 s.op_data[1].data(),
                                 s.op_data[2].data(),
                                 static_cast<int>(op_size[1]),
                                 static_cast<int>(op_size[2]));
            break;
    }
    if (!ok) {
        error = "corrupt DXV texture data";
        return false;
    }
    return true;
}

}}} // namespace caspar::ffmpeg::dxv
