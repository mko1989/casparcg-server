/*
 * HighAsCG WO-993: GPU half of the native DXV decode. Turns a payload plane (core/frame/dxv.h, filled on the CPU by the
 * ffmpeg producer) into an RGBA16 texture of the picture (channels in natural order, read by the image kernel as bgra
 * like every bgra upload).
 */

#pragma once

#include <common/array.h>
#include <common/memory.h>

#include <core/frame/pixel_format.h>

#include <future>
#include <memory>

namespace caspar { namespace accelerator { namespace ogl {

class device;
class texture;

// The pixel_format_desc the image kernel must use for the decoded texture of a dxv frame (bgra, 16 bit, picture size
// from the payload header; straight alpha for DXV3 HQ with alpha, premultiplied otherwise). False without a header.
bool dxv_decoded_desc(const array<const std::uint8_t>& plane, core::pixel_format_desc& desc);

// Uploads the payload plane described by `desc` (a dxv desc) and decodes it on the GL thread.
std::shared_future<std::shared_ptr<texture>> dxv_decode_async(const spl::shared_ptr<device>&   ogl,
                                                              const array<const std::uint8_t>& plane,
                                                              const core::pixel_format_desc&   desc);

}}} // namespace caspar::accelerator::ogl
