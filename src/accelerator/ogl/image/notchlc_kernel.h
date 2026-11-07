/*
 * HighAsCG WO-820: GPU half of the NotchLC decode. Turns a payload plane (core/frame/notchlc.h, filled on the CPU by the
 * ffmpeg producer) into an RGBA16 texture of the picture (channels in natural order, read by the image kernel as bgra like every bgra upload).
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

// The pixel_format_desc the image kernel must use for the decoded texture of a notchlc frame (bgra, 16 bit, picture
// size read from the payload header). Returns false when the frame carries no usable header.
bool notchlc_decoded_desc(const array<const std::uint8_t>& plane, core::pixel_format_desc& desc);

// Uploads the payload plane described by `desc` (a notchlc desc) and decodes it on the GL thread.
std::shared_future<std::shared_ptr<texture>> notchlc_decode_async(const spl::shared_ptr<device>&   ogl,
                                                                  const array<const std::uint8_t>& plane,
                                                                  const core::pixel_format_desc&   desc);

}}} // namespace caspar::accelerator::ogl
