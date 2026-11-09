/*
 * Copyright (c) 2011 Sveriges Television AB <info@casparcg.com>
 *
 * This file is part of CasparCG (www.casparcg.com).
 *
 * CasparCG is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CasparCG is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CasparCG. If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Robert Nagy, ronag89@gmail.com
 */

#include "StdAfx.h"

#include "video_format.h"

#include <common/except.h>
#include <common/log.h>

#include <boost/algorithm/string.hpp>
#include <boost/range/adaptors.hpp>
#include <list>
#include <mutex>
#include <shared_mutex>
#include <utility>

namespace caspar { namespace core {

struct video_format_repository::impl
{
  private:
    // HighAsCG WO-1020: VIDEO-MODE ADD writes at runtime while producers/AMCP read from other threads.
    mutable std::shared_mutex                 mutex_;
    std::map<std::wstring, video_format_desc> formats_;

  public:
    impl()
        : formats_()
    {
        const std::vector<video_format_desc> default_formats = {
            {video_format::pal, 2, 720, 576, 1024, 576, 25000, 1000, L"PAL", {1920 / 2}},
            {video_format::ntsc,
             2,
             720,
             486,
             720,
             540,
             30000,
             1001,
             L"NTSC",
             {801, 801, 801, 800, 801, 801, 801, 800, 801, 801}},
            {video_format::x576p2500, 1, 720, 576, 1024, 576, 25000, 1000, L"576p2500", {1920}},
            {video_format::x720p2398, 1, 1280, 720, 1280, 720, 24000, 1001, L"720p2398", {2002}},
            {video_format::x720p2400, 1, 1280, 720, 1280, 720, 24000, 1000, L"720p2400", {2000}},
            {video_format::x720p2500, 1, 1280, 720, 1280, 720, 25000, 1000, L"720p2500", {1920}},
            {video_format::x720p2997,
             1,
             1280,
             720,
             1280,
             720,
             30000,
             1001,
             L"720p2997",
             {1602, 1601, 1602, 1601, 1602}},
            {video_format::x720p3000, 1, 1280, 720, 1280, 720, 30000, 1000, L"720p3000", {1600}},
            {video_format::x720p5000, 1, 1280, 720, 1280, 720, 50000, 1000, L"720p5000", {960}},
            {video_format::x720p5994, 1, 1280, 720, 1280, 720, 60000, 1001, L"720p5994", {801, 800, 801, 801, 801}},
            {video_format::x720p6000, 1, 1280, 720, 1280, 720, 60000, 1000, L"720p6000", {800}},
            {video_format::x1080i5000, 2, 1920, 1080, 1920, 1080, 25000, 1000, L"1080i5000", {1920 / 2}},
            {video_format::x1080i5994,
             2,
             1920,
             1080,
             1920,
             1080,
             30000,
             1001,
             L"1080i5994",
             {801, 801, 801, 800, 801, 801, 801, 800, 801, 801}},
            {video_format::x1080i6000, 2, 1920, 1080, 1920, 1080, 30000, 1000, L"1080i6000", {1600 / 2}},
            {video_format::x1080p2398, 1, 1920, 1080, 1920, 1080, 24000, 1001, L"1080p2398", {2002}},
            {video_format::x1080p2400, 1, 1920, 1080, 1920, 1080, 24000, 1000, L"1080p2400", {2000}},
            {video_format::x1080p2500, 1, 1920, 1080, 1920, 1080, 25000, 1000, L"1080p2500", {1920}},
            {video_format::x1080p2997,
             1,
             1920,
             1080,
             1920,
             1080,
             30000,
             1001,
             L"1080p2997",
             {1602, 1601, 1602, 1601, 1602}},
            {video_format::x1080p3000, 1, 1920, 1080, 1920, 1080, 30000, 1000, L"1080p3000", {1600}},
            {video_format::x1080p5000, 1, 1920, 1080, 1920, 1080, 50000, 1000, L"1080p5000", {960}},
            {video_format::x1080p5994, 1, 1920, 1080, 1920, 1080, 60000, 1001, L"1080p5994", {801, 800, 801, 801, 801}},
            {video_format::x1080p6000, 1, 1920, 1080, 1920, 1080, 60000, 1000, L"1080p6000", {800}},
            {video_format::x1556p2398, 1, 2048, 1556, 2048, 1556, 24000, 1001, L"1556p2398", {2002}},
            {video_format::x1556p2400, 1, 2048, 1556, 2048, 1556, 24000, 1000, L"1556p2400", {2000}},
            {video_format::x1556p2500, 1, 2048, 1556, 2048, 1556, 25000, 1000, L"1556p2500", {1920}},
            {video_format::x2160p2398, 1, 3840, 2160, 3840, 2160, 24000, 1001, L"2160p2398", {2002}},
            {video_format::x2160p2400, 1, 3840, 2160, 3840, 2160, 24000, 1000, L"2160p2400", {2000}},
            {video_format::x2160p2500, 1, 3840, 2160, 3840, 2160, 25000, 1000, L"2160p2500", {1920}},
            {video_format::x2160p2997,
             1,
             3840,
             2160,
             3840,
             2160,
             30000,
             1001,
             L"2160p2997",
             {1602, 1601, 1602, 1601, 1602}},
            {video_format::x2160p3000, 1, 3840, 2160, 3840, 2160, 30000, 1000, L"2160p3000", {1600}},
            {video_format::x2160p5000, 1, 3840, 2160, 3840, 2160, 50000, 1000, L"2160p5000", {960}},
            {video_format::x2160p5994, 1, 3840, 2160, 3840, 2160, 60000, 1001, L"2160p5994", {801, 800, 801, 801, 801}},
            {video_format::x2160p6000, 1, 3840, 2160, 3840, 2160, 60000, 1000, L"2160p6000", {800}},
            {video_format::x4kDCIp2398, 1, 4096, 2160, 4096, 2160, 24000, 1001, L"4kDCIp2398", {2002}},
            {video_format::x4kDCIp2400, 1, 4096, 2160, 4096, 2160, 24000, 1000, L"4kDCIp2400", {2000}},
            {video_format::x4kDCIp2500, 1, 4096, 2160, 4096, 2160, 25000, 1000, L"4kDCIp2500", {1920}},
            {video_format::x4kDCIp2997,
             1,
             4096,
             2160,
             4096,
             2160,
             30000,
             1001,
             L"4kDCIp2997",
             {1602, 1601, 1602, 1601, 1602}},
            {video_format::x4kDCIp3000, 1, 4096, 2160, 4096, 2160, 30000, 1000, L"4kDCIp3000", {1600}},
            {video_format::x4kDCIp5000, 1, 4096, 2160, 4096, 2160, 50000, 1000, L"4kDCIp5000", {960}},
            {video_format::x4kDCIp5994,
             1,
             4096,
             2160,
             4096,
             2160,
             60000,
             1001,
             L"4kDCIp5994",
             {801, 800, 801, 801, 801}},
            {video_format::x4kDCIp6000, 1, 4096, 2160, 4096, 2160, 60000, 1000, L"4kDCIp6000", {800}},
        };

        for (auto& f : default_formats)
            formats_.insert({(boost::to_lower_copy(f.name)), f});
    }

    video_format_desc find(const std::wstring& name) const
    {
        const std::wstring lower = boost::to_lower_copy(name);

        std::shared_lock<std::shared_mutex> lock(mutex_);

        const auto res = formats_.find(lower);
        if (res != formats_.end())
            return res->second;

        return invalid();
    }

    video_format_desc find_format(const video_format& id) const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        for (auto& f : formats_) {
            if (f.second.format == id)
                return f.second;
        }

        return invalid();
    }

    bool store(const video_format_desc& format)
    {
        const std::wstring lower = boost::to_lower_copy(format.name);

        std::unique_lock<std::shared_mutex> lock(mutex_);
        return formats_.insert({lower, format}).second;
    }

    std::size_t get_max_video_format_size() const
    {
        std::shared_lock<std::shared_mutex> lock(mutex_);
        size_t                              max = 0;
        for (auto& f : formats_) {
            if (f.second.size > max)
                max = f.second.size;
        }

        const size_t MaxBytesPerColor = 2;
        return max * MaxBytesPerColor;
    }
};

video_format_repository::video_format_repository()
    : impl_(new impl())
{
}
video_format_desc video_format_repository::invalid()
{
    return video_format_desc(video_format::invalid, 1, 0, 0, 0, 0, 1, 1, L"invalid", {1});
};
video_format_desc video_format_repository::find(const std::wstring& name) const { return impl_->find(name); }
video_format_desc video_format_repository::find_format(const video_format& format) const
{
    return impl_->find_format(format);
}
bool video_format_repository::store(const video_format_desc& format) { return impl_->store(format); }

std::size_t video_format_repository::get_max_video_format_size() const { return impl_->get_max_video_format_size(); }

video_format_desc::video_format_desc(const video_format     format,
                                     const int              field_count,
                                     const int              width,
                                     const int              height,
                                     const int              square_width,
                                     const int              square_height,
                                     const int              time_scale,
                                     const int              duration,
                                     const std::wstring     name,
                                     const std::vector<int> audio_cadence)
    : format(format)
    , width(width)
    , height(height)
    , square_width(square_width)
    , square_height(square_height)
    , field_count(field_count)
    , hz(static_cast<double>(time_scale) / static_cast<double>(duration))
    , fps(hz * field_count)
    , framerate(time_scale, duration)
    , time_scale(time_scale)
    , duration(duration)
    , size(width * height * 4)
    , name(std::move(name))
    , audio_sample_rate(48000)
    , audio_cadence(std::move(audio_cadence))
{
}

video_format_desc::video_format_desc()
    : format(video_format::invalid)
{
    *this = video_format_repository::invalid();
}

video_format_desc make_custom_video_format_desc(const std::wstring& id,
                                                int                 width,
                                                int                 height,
                                                int                 field_count,
                                                int                 time_scale,
                                                int                 duration,
                                                const std::wstring& cadence_str)
{
    if (id.empty())
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid video-mode id: " + id));

    if (width <= 0 || height <= 0)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid dimensions: " + std::to_wstring(width) + L"x" +
                                                        std::to_wstring(height)));

    if (field_count != 1 && field_count != 2)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid field-count: " + std::to_wstring(field_count)));

    if (time_scale <= 0 || duration <= 0)
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid framerate: " + std::to_wstring(time_scale) + L"/" +
                                                        std::to_wstring(duration)));

    std::vector<int> cadence;
    long long        cadence_sum = 0;

    std::list<std::wstring> cadence_parts;
    boost::split(cadence_parts, cadence_str, boost::is_any_of(L", "));

    for (auto& cad : cadence_parts) {
        if (cad.empty())
            continue;

        int c = 0;
        try {
            c = std::stoi(cad);
        } catch (...) {
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid cadence value in video-mode " + id + L": " + cad));
        }
        if (c <= 0)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid cadence value in video-mode " + id + L": " + cad));
        cadence.push_back(c);
        cadence_sum += c;
    }

    if (cadence.empty()) {
        // Attempt to calculate in the cadence for integer formats
        const int c = static_cast<int>(48000 / (static_cast<double>(time_scale) / duration) + 0.5);
        cadence.push_back(c);
        cadence_sum += c;
    }

    if (cadence_sum * time_scale != 48000LL * duration * static_cast<long long>(cadence.size())) {
        auto samples_per_second = static_cast<double>(cadence_sum * time_scale) / (duration * cadence.size());
        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Incorrect cadence in video-mode " + id + L". Got " +
                                                        std::to_wstring(samples_per_second) +
                                                        L" samples per second, expected 48000"));
    }

    return video_format_desc(
        video_format::custom, field_count, width, height, width, height, time_scale, duration, id, cadence);
}

bool same_video_mode(const video_format_desc& a, const video_format_desc& b)
{
    return a.format == b.format && a.width == b.width && a.height == b.height && a.field_count == b.field_count &&
           a.time_scale == b.time_scale && a.duration == b.duration && a.audio_cadence == b.audio_cadence;
}

bool operator==(const video_format_desc& lhs, const video_format_desc& rhs)
{
    if (lhs.format == video_format::custom || rhs.format == video_format::custom) {
        if (lhs.format != rhs.format) {
            // If one is custom, and the other isnt, then they dont match
            return false;
        }

        // TODO - expand on this
        if (lhs.width != rhs.width || lhs.height != rhs.height || lhs.framerate != rhs.framerate) {
            return false;
        }

        return true;
    } else {
        // If neither are custom, look just at the format
        return lhs.format == rhs.format;
    }
}

bool operator!=(const video_format_desc& lhs, const video_format_desc& rhs) { return !(lhs == rhs); }

std::wostream& operator<<(std::wostream& out, const video_format_desc& format_desc)
{
    out << format_desc.name.c_str();
    return out;
}

}} // namespace caspar::core
