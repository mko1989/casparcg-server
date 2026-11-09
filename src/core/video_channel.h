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

#pragma once

#include "fwd.h"
#include "video_format.h"

#include "frame/pixel_format.h"

#include "monitor/monitor.h"

#include <common/memory.h>

#include <boost/signals2.hpp>

#include <atomic>
#include <functional>
#include <mutex>

namespace caspar { namespace core {

enum route_mode
{
    foreground,
    background,
    next, // background if any, otherwise foreground
};

struct route_id
{
    int        index;
    route_mode mode;

    bool const operator==(const route_id& o) { return index == o.index && mode == o.mode; }
};

struct route
{
    route()             = default;
    route(const route&) = delete;
    route(route&&)      = delete;

    route& operator=(const route&) = delete;
    route& operator=(route&&)      = delete;

    boost::signals2::signal<void(class draw_frame, class draw_frame)> signal;
    std::wstring                                                      name;

    // HighAsCG WO-1020: the source format is read by the consuming channel's thread every tick and refreshed by the
    // source channel when its mode changes (SET MODE), so it is behind a mutex (was a plain field, set once).
    video_format_desc format_desc() const
    {
        std::lock_guard<std::mutex> lock(format_mutex_);
        return format_desc_;
    }
    double fps() const
    {
        std::lock_guard<std::mutex> lock(format_mutex_);
        return format_desc_.fps;
    }
    void set_format_desc(const video_format_desc& format_desc)
    {
        std::lock_guard<std::mutex> lock(format_mutex_);
        format_desc_ = format_desc;
    }

    // HighAsCG WO-1020: set when the source channel is removed (CHANNEL REMOVE). Route producers then output nothing
    // (transparent, silent) instead of repeating their last frame and its audio forever.
    std::atomic<bool> source_removed{false};

  private:
    mutable std::mutex format_mutex_;
    video_format_desc  format_desc_;
};

class video_channel final
{
    video_channel(const video_channel&);
    video_channel& operator=(const video_channel&);

  public:
    explicit video_channel(int                                       index,
                           const video_format_desc&                  format_desc,
                           color_space                               default_color_space,
                           std::unique_ptr<image_mixer>              image_mixer,
                           std::function<void(core::monitor::state)> on_tick);
    ~video_channel();

    core::monitor::state state() const;

    const std::shared_ptr<core::stage>& stage() const;
    std::shared_ptr<core::stage>&       stage();
    const core::mixer&                  mixer() const;
    core::mixer&                        mixer();
    const core::output&                 output() const;
    core::output&                       output();

    spl::shared_ptr<core::frame_factory> frame_factory();

    int index() const;

    [[nodiscard]] channel_info get_consumer_channel_info() const;

    std::shared_ptr<core::route> route(int index = -1, route_mode mode = route_mode::foreground);

    // HighAsCG WO-1020: routes taken from this channel that some producer still holds (route:// on any channel,
    // including this one).
    int live_route_count() const;
    // HighAsCG WO-1020: CHANNEL REMOVE. Marks every live route (and any route asked for later) as source_removed.
    void close_routes();

  private:
    struct impl;
    spl::unique_ptr<impl> impl_;
};

}} // namespace caspar::core
