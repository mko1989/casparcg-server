/*
 * HighAsCG WO-993: native DXV (Resolume DXV2 / DXV3) GPU decode — layout of the payload frame shared by the ffmpeg
 * producer (which fills it on the CPU: modules/ffmpeg/util/dxv_unpack undoes DXV's own compression down to raw BC
 * blocks) and the OpenGL mixer (which decodes the BC1/BC3/BC4/BC5 blocks per pixel in a shader, dxv_kernel.cpp).
 *
 * One plane of 32-bit little-endian words, uploaded as an 8-bit BGRA texture dxv::row_words texels wide (one texel =
 * one word), exactly like the NotchLC payload (core/frame/notchlc.h). Words 0..header_words-1 are the header below; the
 * full-resolution blocks ("tex") start at word header[h_tex], the half-resolution Co/Cg blocks ("ctex", HQ only) at
 * word header[h_ctex]. Both keep dxv_unpack's raster block order.
 */

#pragma once

#include <cstdint>
#include <cstring>

namespace caspar { namespace core { namespace dxv {

constexpr uint32_t magic        = 0x56584443; // "CDXV"
constexpr int      row_words    = 1024;       // texels per texture row (4096 bytes)
constexpr int      header_words = 16;

enum format : uint32_t
{
    fmt_none = 0,
    fmt_dxt1, // BC1, opaque
    fmt_dxt5, // BC3, premultiplied alpha
    fmt_ycg6, // BC4 Y + half-res BC5 Co|Cg
    fmt_yg10, // BC5 Y|A + half-res BC5 Co|Cg
};

enum header_field
{
    h_magic = 0,
    h_valid,   // 1 = decodable, 0 = draw transparent
    h_format,  // dxv::format
    h_width,   // picture size (what FFmpeg outputs: the stream size, cropped from the coded size)
    h_height,
    h_coded_w, // coded size (multiples of 4): block addressing
    h_coded_h,
    h_tex,     // word index of the full-res blocks (= header_words)
    h_ctex,    // word index of the half-res Co/Cg blocks (0 when there are none)
    h_used,    // bytes of the plane in use
};

// Largest payload a coded_w x coded_h stream can produce: 1 byte/pixel of full-res blocks (BC3 / BC5) + FFmpeg's
// ctex size expression (w/2/4 * h/2/4 * 16, plus slack for its odd-height rounding), plus header and read-ahead.
inline int64_t max_bytes(int coded_w, int coded_h)
{
    const int64_t tex  = static_cast<int64_t>(coded_w) * coded_h;
    const int64_t ctex = static_cast<int64_t>(coded_w / 8 + 1) * (coded_h / 8 + 1) * 16;
    return header_words * 4 + tex + ctex + 64;
}

inline int plane_rows(int coded_w, int coded_h)
{
    return static_cast<int>((max_bytes(coded_w, coded_h) + row_words * 4 - 1) / (row_words * 4));
}

inline uint32_t read_word(const uint8_t* plane, int index)
{
    uint32_t v;
    std::memcpy(&v, plane + static_cast<size_t>(index) * 4, 4);
    return v;
}

}}} // namespace caspar::core::dxv
