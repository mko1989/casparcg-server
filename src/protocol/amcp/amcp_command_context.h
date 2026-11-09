/*
 * Copyright (c) 2018 Norsk rikskringkasting AS
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
 * Author: Julian Waller, julian@superfly.tv
 */

#pragma once

#include "../util/ClientInfo.h"
#include "amcp_command_repository.h"
#include "amcp_shared.h"
#include <accelerator/accelerator.h>
#include <boost/property_tree/ptree.hpp>
#include <functional>
#include <future>
#include <utility>

namespace caspar::protocol::osc {
class client;
}

namespace caspar { namespace protocol { namespace amcp {

// HighAsCG WO-1020: runtime channel administration, implemented by the server (shell/server.cpp), which owns the
// accelerator, the OSC client and the channel list. Empty functions = not available (the commands answer 501).
struct channel_admin
{
    // Creates a channel; at = 0 appends after the highest number ever used, at = n fills the empty slot n (or
    // appends when n is one past the end). Returns the new 1-based channel number. Throws user_error.
    std::function<int(const core::video_format_desc& format, int color_depth, const std::wstring& color_space, int at)>
        add_channel;
    // Tears channel n down (1-based) and leaves its slot empty. Returns the AMCP reply line.
    std::function<std::wstring(int channel, bool force, const IO::ClientInfoPtr& client)> remove_channel;
    // The boot configuration with <channels> replaced by the live list (INFO CONFIG).
    std::function<boost::property_tree::wptree()> live_config;
};

struct amcp_command_static_context
{
    const core::video_format_repository                        format_repository;
    const spl::shared_ptr<core::cg_producer_registry>          cg_registry;
    const spl::shared_ptr<const core::frame_producer_registry> producer_registry;
    const spl::shared_ptr<const core::frame_consumer_registry> consumer_registry;
    const std::shared_ptr<amcp_command_repository>             parser;
    std::function<void(bool)>                                  shutdown_server_now;
    const std::string                                          proxy_host;
    const std::string                                          proxy_port;
    std::weak_ptr<accelerator::accelerator_device>             ogl_device;
    const spl::shared_ptr<osc::client>                         osc_client;
    channel_admin                                              channels_admin; // HighAsCG WO-1020, set by the server

    amcp_command_static_context(core::video_format_repository                               format_repository,
                                const spl::shared_ptr<core::cg_producer_registry>&          cg_registry,
                                const spl::shared_ptr<const core::frame_producer_registry>& producer_registry,
                                const spl::shared_ptr<const core::frame_consumer_registry>& consumer_registry,
                                std::shared_ptr<amcp_command_repository>                    parser,
                                std::function<void(bool)>                                   shutdown_server_now,
                                std::string                                                 proxy_host,
                                std::string                                                 proxy_port,
                                std::weak_ptr<accelerator::accelerator_device>              ogl_device,
                                const spl::shared_ptr<osc::client>&                         osc_client)
        : format_repository(std::move(format_repository))
        , cg_registry(cg_registry)
        , producer_registry(producer_registry)
        , consumer_registry(consumer_registry)
        , parser(std::move(parser))
        , shutdown_server_now(std::move(shutdown_server_now))
        , proxy_host(std::move(proxy_host))
        , proxy_port(std::move(proxy_port))
        , ogl_device(std::move(ogl_device))
        , osc_client(osc_client)
    {
    }
};

struct command_context
{
    std::shared_ptr<amcp_command_static_context>        static_context;
    const spl::shared_ptr<std::vector<channel_context>> channels;
    const IO::ClientInfoPtr                             client;
    const channel_context                               channel;
    const int                                           channel_index;
    const int                                           layer_id;
    std::vector<std::wstring>                           parameters;

    int layer_index(int default_ = 0) const { return layer_id == -1 ? default_ : layer_id; }

    command_context(std::shared_ptr<amcp_command_static_context>        static_context,
                    const spl::shared_ptr<std::vector<channel_context>> channels,
                    const IO::ClientInfoPtr&                            client,
                    channel_context                                     channel,
                    int                                                 channel_index,
                    int                                                 layer_id)
        : static_context(std::move(static_context))
        , channels(std::move(channels))
        , client(client)
        , channel(std::move(channel))
        , channel_index(channel_index)
        , layer_id(layer_id)
    {
    }
};

}}} // namespace caspar::protocol::amcp
