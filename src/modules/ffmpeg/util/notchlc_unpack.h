/*
 * HighAsCG WO-820: CPU half of the NotchLC GPU decode. Undoes the outer compression (LZ4 / LZF / stored) of one NotchLC
 * packet into a payload plane (layout in core/frame/notchlc.h) and builds the luma block bit-offset table; the OpenGL
 * mixer decodes the blocks and converts to RGB in a shader. Mirrors the checks of FFmpeg 8.0.1 libavcodec/notchlc.c.
 */

#pragma once

#include <cstddef>
#include <cstdint>

namespace caspar { namespace ffmpeg { namespace notchlc {

// Fills `plane` (`plane_bytes` long, at least header_words*4). Always writes a header; returns false and sets `error`
// when the packet cannot be decoded (the header is then marked invalid and the mixer draws the frame transparent).
bool unpack(const uint8_t* packet,
            int            packet_size,
            int            width,
            int            height,
            uint8_t*       plane,
            size_t         plane_bytes,
            const char**   error);

}}} // namespace caspar::ffmpeg::notchlc
