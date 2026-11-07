/*
 * HighAsCG: CPU half of the native DXV (Resolume DXV2 / DXV3) GPU decode. Undoes DXV's own compression (LZF, stored,
 * the DXT1/DXT5 back-reference schemes and the DXV3 HQ opcode/FSE scheme) of one packet down to raw BC texture
 * blocks; the OpenGL mixer decodes the BC1/BC3/BC4/BC5 blocks in a shader.
 *
 * Port of FFmpeg 8.0.1 libavcodec/dxv.c and libavcodec/lzf.c (LGPL-2.1-or-later, see dxv_unpack.cpp for the original
 * copyright notices). No FFmpeg headers are used.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace caspar { namespace ffmpeg { namespace dxv {

enum class tex_format
{
    none,
    dxt1, // BC1 blocks (8 bytes / 4x4). FFmpeg dxt1_block: opaque, colour 3 of the 3-colour mode is opaque black.
    dxt5, // BC3 blocks (16 bytes / 4x4) holding PREMULTIPLIED alpha. FFmpeg decodes with dxt4_block (un-premultiplies).
    ycg6, // tex: BC4 Y (8 bytes / 4x4, full res); ctex: BC5 Co|Cg (16 bytes / 4x4 of the half-res chroma planes).
    yg10  // tex: BC5 Y|A (16 bytes / 4x4, full res); ctex: BC5 Co|Cg as for ycg6.
};

struct layout
{
    tex_format fmt    = tex_format::none;
    int        width  = 0; // coded size (FFALIGN(width, 4) / FFALIGN(height, 4), as dxv_init() sets coded_width/height)
    int        height = 0;
    size_t     tex_size  = 0; // bytes of full-res blocks: BC1 (dxt1), BC3 (dxt5), BC4 Y (ycg6), BC5 Y+A (yg10)
    size_t     ctex_size = 0; // bytes of half-res BC5 Co/Cg blocks (ycg6/yg10), else 0. FFmpeg's expression
                              // w/2/4*h/2/4*16 (left to right, truncating); when coded height % 8 == 4 this exceeds
                              // (w/8)*(h/8)*16 by part of a block row that is decoded but never displayed.
};

// Reusable per-stream buffers. Keep one per decoder instance (not thread-safe to share).
struct scratch
{
    struct opcode_entry
    {
        int16_t next;
        uint8_t val1;
        uint8_t val2;
    };
    std::vector<uint8_t> op_data[4];   // dxv.c op_data[0..3]
    opcode_entry         optable[1024]; // dxv.c get_opcodes() OpcodeTable optable[1024]
    uint32_t             ltable[256];   // dxv.c dxv_decompress_opcodes() uint32_t table[256]
};

// Parse + decompress one packet. On success fills `tex` (exactly out.tex_size bytes) and `ctex` (exactly
// out.ctex_size bytes), block order exactly as FFmpeg's tex_data / ctex_data before texturedsp:
//   blocks in raster order, (width/4) blocks per row (ctex: (width/2/4) per row, (height/2/4) rows).
// coded_width/coded_height must be multiples of 4 (pass FFALIGN(w, 4), FFALIGN(h, 4)); like FFmpeg, coded_height
// must be >= 8. For ycg6/yg10 FFmpeg does not require more: a coded width/height that is not a multiple of 8 leaves
// the last chroma column/row of the half-res plane undecoded (floor((w/2)/4) blocks), and we mirror that.
// On any malformed input returns false and sets `error`; never reads or writes out of bounds.
//
// Reuse `tex` / `ctex` across packets of one stream: bytes a malformed-but-accepted packet leaves unwritten (short LZF
// output, a stored ycg6/yg10 packet's ctex, unknown HQ opcodes >= 18) keep their previous content, exactly like
// FFmpeg's reused ctx->tex_data / ctex_data (fresh bytes start at 0 in both).
bool unpack(const uint8_t*        pkt,
            size_t                size,
            int                   coded_width,
            int                   coded_height,
            layout&               out,
            std::vector<uint8_t>& tex,
            std::vector<uint8_t>& ctex,
            scratch&              s,
            std::string&          error);

}}} // namespace caspar::ffmpeg::dxv
