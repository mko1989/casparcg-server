/*
 * HighAsCG WO-820: NotchLC GPU decode — layout of the payload frame shared by the ffmpeg producer (which fills it on the
 * CPU: LZ4/LZF only, plus a per-block luma bit-offset table) and the OpenGL mixer (which decodes it in a shader).
 *
 * The frame has one plane of 32-bit little-endian words, uploaded as an 8-bit BGRA texture that is
 * notchlc::row_words texels wide (one texel = one word). Word 0..header_words-1 is the header below, the NotchLC payload
 * (the bytes after the LZ4/LZF step, starting with its 40-byte header) starts at word header_words, and the luma table
 * (one word per 4x4 luma block, raster order: absolute bit position of the block's first bit in the plane) follows the
 * payload at word header[table].
 */

#pragma once

#include <cstdint>
#include <cstring>

namespace caspar { namespace core { namespace notchlc {

constexpr uint32_t magic        = 0x434C4E43; // "CNLC"
constexpr int      row_words    = 1024;       // texels per texture row (4096 bytes)
constexpr int      header_words = 16;

enum header_field
{
    h_magic = 0,
    h_valid,      // 1 = decodable, 0 = draw transparent
    h_width,      // picture size
    h_height,
    h_payload,    // word index of the NotchLC payload (= header_words)
    h_size,       // payload bytes
    h_table,      // word index of the luma block table
    h_used,       // bytes of the plane in use (header + payload + table)
    h_y_ctrl,     // byte offsets inside the payload, as notchlc.c computes them
    h_uv_off,
    h_uv_data,
    h_a_ctrl,
    h_a_data,     // a_data_offset (alpha offsets are relative to uv_data + a_data)
    h_y_data,
    h_opaque,
    h_reserved,
};

// Largest NotchLC payload (bytes after LZ4/LZF) the plane accepts. The format does not bound it: FFmpeg reads 8 bytes
// of alpha per 16x16 block, but the encoder stores far more (Notch content measured up to 1.67 bytes/pixel overall,
// 0.5 of it alpha: s_INTRO.mov frame 2250, WO-820). 4 bytes/pixel is half of raw yuva444p12; the decoder's buffers are
// only touched as far as a frame's real payload, and the mixer copies/uploads only the rows in use (av_util make_frame).
inline int64_t max_payload_bytes(int width, int height) { return static_cast<int64_t>(width) * height * 4 + 65536; }

// Rows of the decoder's payload frame for a width x height stream: header + max payload + luma table, whole rows.
inline int plane_rows(int width, int height)
{
    const int64_t table = static_cast<int64_t>(width / 4) * (height / 4) * 4;
    const int64_t bytes = header_words * 4 + max_payload_bytes(width, height) + table;
    return static_cast<int>((bytes + row_words * 4 - 1) / (row_words * 4));
}

inline uint32_t read_word(const uint8_t* plane, int index)
{
    uint32_t v;
    std::memcpy(&v, plane + static_cast<size_t>(index) * 4, 4);
    return v;
}

}}} // namespace caspar::core::notchlc
