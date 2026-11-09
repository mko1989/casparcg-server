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
#include "included_modules.h"

#include "server.h"

#include <accelerator/accelerator.h>

#include <common/bit_depth.h>
#include <common/env.h>
#include <common/except.h>
#include <common/memory.h>
#include <common/ptree.h>
#include <common/utf.h>

#include <core/consumer/output.h>
#include <core/diagnostics/call_context.h>
#include <core/diagnostics/osd_graph.h>
#include <core/frame/pixel_format.h>
#include <core/mixer/image/image_mixer.h>
#include <core/producer/cg_proxy.h>
#include <core/producer/color/color_producer.h>
#include <core/producer/frame_producer.h>
#include <core/video_channel.h>
#include <core/video_format.h>

#include <modules/image/consumer/image_consumer.h>

#include <protocol/amcp/AMCPCommandsImpl.h>
#include <protocol/amcp/AMCPProtocolStrategy.h>
#include <protocol/amcp/amcp_command_repository.h>
#include <protocol/amcp/amcp_shared.h>
#include <protocol/osc/client.h>
#include <protocol/util/AsyncEventServer.h>
#include <protocol/util/strategy_adapters.h>
#include <protocol/util/tokenize.h>

#include <boost/algorithm/string.hpp>
#include <boost/asio.hpp>
#include <boost/format.hpp>
#include <boost/property_tree/ptree.hpp>

#include <chrono>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>

namespace caspar {
using namespace core;
using namespace protocol;

std::shared_ptr<boost::asio::io_context> create_io_context_with_running_service()
{
    auto io_context = std::make_shared<boost::asio::io_context>();
    // To keep the io_context::run() running although no pending async
    // operations are posted.
    auto work      = std::make_shared<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(boost::asio::make_work_guard(*io_context));
    auto weak_work = std::weak_ptr<boost::asio::executor_work_guard<boost::asio::io_context::executor_type>>(work);
    auto thread    = std::make_shared<std::thread>([io_context, weak_work] {
        while (auto strong = weak_work.lock()) {
            try {
                io_context->run();
            } catch (...) {
                CASPAR_LOG_CURRENT_EXCEPTION();
            }
        }

        CASPAR_LOG(info) << "[asio] Global io_context uninitialized.";
    });

    return std::shared_ptr<boost::asio::io_context>(io_context.get(), [io_context, work, thread](void*) mutable {
        CASPAR_LOG(info) << "[asio] Shutting down global io_context.";
        work.reset();
        io_context->stop();
        if (thread->get_id() != std::this_thread::get_id())
            thread->join();
        else
            thread->detach();
    });
}

struct server::impl
{
    std::shared_ptr<boost::asio::io_context>               io_context_ = create_io_context_with_running_service();
    video_format_repository                                video_format_repository_;
    accelerator::accelerator                               accelerator_;
    std::shared_ptr<amcp::amcp_command_repository>         amcp_command_repo_;
    std::shared_ptr<amcp::amcp_command_repository_wrapper> amcp_command_repo_wrapper_;
    std::shared_ptr<amcp::command_context_factory>         amcp_context_factory_;
    std::vector<spl::shared_ptr<IO::AsyncEventServer>>     async_servers_;
    std::shared_ptr<IO::AsyncEventServer>                  primary_amcp_server_;
    std::shared_ptr<osc::client>                           osc_client_ = std::make_shared<osc::client>(io_context_);
    std::vector<std::shared_ptr<void>>                     predefined_osc_subscriptions_;
    // HighAsCG WO-1020: copy-on-write so CHANNEL ADD/REMOVE can change it while AMCP threads read snapshots.
    spl::shared_ptr<protocol::amcp::channel_list> channels_;
    // HighAsCG WO-1020: per slot, the <channel> element INFO CONFIG reports (boot XML, or one written for a runtime
    // CHANNEL ADD); empty = removed. Written inside channels_->modify(), read under slot_xml_mutex_.
    std::mutex                                                   slot_xml_mutex_;
    std::vector<std::optional<boost::property_tree::wptree>>     slot_xml_;
    spl::shared_ptr<core::cg_producer_registry>                   cg_registry_;
    spl::shared_ptr<core::frame_producer_registry>                producer_registry_;
    spl::shared_ptr<core::frame_consumer_registry>                consumer_registry_;
    std::function<void(bool)>                                     shutdown_server_now_;

    impl(const impl&)            = delete;
    impl& operator=(const impl&) = delete;

    explicit impl(std::function<void(bool)> shutdown_server_now)
        : video_format_repository_()
        , accelerator_(video_format_repository_)
        , producer_registry_(spl::make_shared<core::frame_producer_registry>())
        , consumer_registry_(spl::make_shared<core::frame_consumer_registry>())
        , shutdown_server_now_(std::move(shutdown_server_now))
    {
        caspar::core::diagnostics::osd::register_sink();
    }

    void start()
    {
        setup_video_modes(env::properties());
        CASPAR_LOG(info) << L"Initialized video modes.";

        auto xml_channels = setup_channels(env::properties());
        CASPAR_LOG(info) << L"Initialized channels.";

        setup_amcp_command_repo();
        CASPAR_LOG(info) << L"Initialized command repository.";

        module_dependencies dependencies(
            cg_registry_, producer_registry_, consumer_registry_, amcp_command_repo_wrapper_);
        initialize_modules(dependencies);
        CASPAR_LOG(info) << L"Initialized modules.";

        setup_channel_producers_and_consumers(xml_channels);
        CASPAR_LOG(info) << L"Initialized startup producers.";

        setup_controllers(env::properties());
        CASPAR_LOG(info) << L"Initialized controllers.";

        setup_osc(env::properties());
        CASPAR_LOG(info) << L"Initialized osc.";
    }

    ~impl()
    {
        std::weak_ptr<boost::asio::io_context> weak_io_context = io_context_;
        io_context_.reset();
        predefined_osc_subscriptions_.clear();
        osc_client_.reset();

        amcp_command_repo_wrapper_.reset();
        amcp_command_repo_.reset();
        amcp_context_factory_.reset();

        primary_amcp_server_.reset();
        async_servers_.clear();

        destroy_producers_synchronously();
        destroy_consumers_synchronously();
        channels_->modify([](std::vector<protocol::amcp::channel_context>& list) {
            list.clear();
            return 0;
        });

        while (weak_io_context.lock())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));

        uninitialize_modules();
        core::diagnostics::osd::shutdown();
    }

    void setup_video_modes(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;

        auto videomodes_config = pt.get_child_optional(L"configuration.video-modes");
        if (videomodes_config) {
            for (auto& xml_channel :
                 pt | witerate_children(L"configuration.video-modes") | welement_context_iteration) {
                ptree_verify_element_name(xml_channel, L"video-mode");

                // HighAsCG WO-1020: the checks live in core so AMCP VIDEO-MODE ADD applies exactly the same ones.
                const std::wstring id         = xml_channel.second.get(L"id", L"");
                const auto         new_format = core::make_custom_video_format_desc(
                    id,
                    xml_channel.second.get<int>(L"width", 0),
                    xml_channel.second.get<int>(L"height", 0),
                    xml_channel.second.get<int>(L"field-count", 1),
                    xml_channel.second.get<int>(L"time-scale", 60000),
                    xml_channel.second.get<int>(L"duration", 1000),
                    xml_channel.second.get(L"cadence", L""));

                const auto existing = video_format_repository_.find(id);
                if (existing.format != video_format::invalid)
                    CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Video-mode already exists: " + id));

                video_format_repository_.store(new_format);
            }
        }
    }

    // HighAsCG WO-1020: one channel, as setup_channels made them at startup; also used by CHANNEL ADD at runtime.
    protocol::amcp::channel_context make_channel(int                             channel_id,
                                                 const core::video_format_desc&  format_desc,
                                                 int                             color_depth,
                                                 const std::wstring&             color_space_name)
    {
        if (color_depth != 8 && color_depth != 16)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid color-depth: " + std::to_wstring(color_depth)));

        auto color_space_str = boost::to_lower_copy(color_space_name);
        if (color_space_str != L"bt709" && color_space_str != L"bt2020")
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid color-space, must be bt709 or bt2020"));

        if (format_desc.format == video_format::invalid)
            CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid video-mode: " + format_desc.name));

        auto weak_client = std::weak_ptr<osc::client>(osc_client_);
        auto depth       = color_depth == 16 ? common::bit_depth::bit16 : common::bit_depth::bit8;
        auto default_color_space =
            color_space_str == L"bt2020" ? core::color_space::bt2020 : core::color_space::bt709;
        auto channel =
            spl::make_shared<video_channel>(channel_id,
                                            format_desc,
                                            default_color_space,
                                            accelerator_.create_image_mixer(channel_id, depth),
                                            [channel_id, weak_client](core::monitor::state channel_state) {
                                                monitor::state state;
                                                state[""]["channel"][channel_id] = channel_state;
                                                auto client                      = weak_client.lock();
                                                if (client) {
                                                    client->send(std::move(state));
                                                }
                                            });

        const std::wstring lifecycle_key = L"lock" + std::to_wstring(channel_id);
        return protocol::amcp::channel_context(channel, channel->stage(), lifecycle_key);
    }

    std::vector<boost::property_tree::wptree> setup_channels(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;

        std::vector<wptree> xml_channels;

        channels_->modify([&](std::vector<protocol::amcp::channel_context>& list) {
            for (auto& xml_channel : pt | witerate_children(L"configuration.channels") | welement_context_iteration) {
                xml_channels.push_back(xml_channel.second);
                ptree_verify_element_name(xml_channel, L"channel");

                auto format_desc_str = xml_channel.second.get(L"video-mode", L"PAL");
                auto format_desc     = video_format_repository_.find(format_desc_str);
                auto color_depth     = xml_channel.second.get<unsigned char>(L"color-depth", 8);
                if (format_desc.format == video_format::invalid)
                    CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid video-mode: " + format_desc_str));

                list.emplace_back(make_channel(static_cast<int>(list.size() + 1),
                                               format_desc,
                                               color_depth,
                                               xml_channel.second.get(L"color-space", L"bt709")));
            }

            std::lock_guard<std::mutex> lock(slot_xml_mutex_);
            slot_xml_.assign(xml_channels.begin(), xml_channels.end());
            return 0;
        });

        return xml_channels;
    }

    // HighAsCG WO-1020: CHANNEL ADD. Runs on an AMCP thread; the list's write lock serialises it with REMOVE.
    int add_channel(const core::video_format_desc& format_desc,
                    int                            color_depth,
                    const std::wstring&            color_space,
                    int                            at)
    {
        return channels_->modify([&](std::vector<protocol::amcp::channel_context>& list) -> int {
            size_t slot = list.size(); // append: numbers are never reused unless asked for with AT
            if (at > 0) {
                slot = static_cast<size_t>(at - 1);
                if (slot > list.size())
                    CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"CHANNEL ADD AT " + std::to_wstring(at) +
                                                                    L": not an empty slot or the next number"));
                if (slot < list.size() && !list[slot].empty())
                    CASPAR_THROW_EXCEPTION(user_error()
                                           << msg_info(L"CHANNEL ADD AT " + std::to_wstring(at) + L": channel exists"));
            }

            const int  channel_id = static_cast<int>(slot + 1);
            auto       context    = make_channel(channel_id, format_desc, color_depth, color_space);
            if (slot == list.size()) {
                list.push_back(context);
            } else { // channel_context is not assignable: rebuild the copy with the slot filled
                std::vector<protocol::amcp::channel_context> rebuilt;
                rebuilt.reserve(list.size());
                for (size_t n = 0; n < list.size(); ++n)
                    rebuilt.push_back(n == slot ? context : list[n]);
                list.swap(rebuilt);
            }

            boost::property_tree::wptree xml;
            xml.put(L"<xmlattr>.runtime", L"true");
            xml.put(L"video-mode", format_desc.name);
            xml.put(L"color-depth", color_depth);
            xml.put(L"color-space", boost::to_lower_copy(color_space));
            xml.put_child(L"consumers", boost::property_tree::wptree());
            {
                std::lock_guard<std::mutex> lock(slot_xml_mutex_);
                slot_xml_.resize(std::max(slot_xml_.size(), list.size()));
                slot_xml_[slot] = std::move(xml);
            }

            CASPAR_LOG(info) << L"CHANNEL ADD: channel " << channel_id << L" " << format_desc.name << L" "
                             << color_depth << L"-bit " << color_space;
            return channel_id;
        });
    }

    // HighAsCG WO-1020: CHANNEL REMOVE. See WO-1020 for the hazard analysis behind the order below.
    std::wstring remove_channel(int channel_id, bool force, const IO::ClientInfoPtr& client)
    {
        // A route producer is destroyed on the producer destroyer thread a moment after its layer is stopped, so a
        // REMOVE sent right after the STOP would still see the route. Give that up to 500 ms before deciding.
        if (!force) {
            const auto snapshot = channels_->snapshot();
            if (channel_id >= 1 && channel_id <= static_cast<int>(snapshot->size()) &&
                !snapshot->at(channel_id - 1).empty()) {
                const auto& candidate = snapshot->at(channel_id - 1).raw_channel;
                for (int n = 0; n < 50 && candidate->live_route_count() > 0; ++n)
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }

        std::shared_ptr<video_channel>             channel;
        std::shared_ptr<IO::lock_container>        lock;
        const std::wstring refusal = channels_->modify([&](std::vector<protocol::amcp::channel_context>& list) {
            if (channel_id < 1 || channel_id > static_cast<int>(list.size()) || list[channel_id - 1].empty())
                return std::wstring(L"401 CHANNEL REMOVE ERROR\r\n");

            const auto& slot = list[channel_id - 1];
            if (slot.lock && !slot.lock->check_access(client))
                return std::wstring(L"503 CHANNEL REMOVE FAILED\r\n");

            const int routes = slot.raw_channel->live_route_count();
            if (routes > 0 && !force) {
                CASPAR_LOG(warning) << L"CHANNEL REMOVE " << channel_id << L" refused: " << routes
                                    << L" live route(s) from it. STOP them, or use FORCE.";
                return L"403 CHANNEL REMOVE FAILED (" + std::to_wstring(routes) +
                       L" live routes from it; STOP them or add FORCE)\r\n";
            }

            channel = slot.raw_channel;
            lock    = slot.lock;

            // 1. Empty the slot first: new commands can no longer reach the channel, and commands already parsed
            //    against it fail when they run (command_context_factory checks the slot).
            std::vector<protocol::amcp::channel_context> rebuilt;
            rebuilt.reserve(list.size());
            for (size_t n = 0; n < list.size(); ++n) {
                if (static_cast<int>(n) == channel_id - 1)
                    rebuilt.emplace_back();
                else
                    rebuilt.push_back(list[n]);
            }
            list.swap(rebuilt);

            std::lock_guard<std::mutex> xml_lock(slot_xml_mutex_);
            if (static_cast<size_t>(channel_id - 1) < slot_xml_.size())
                slot_xml_[channel_id - 1].reset();
            return std::wstring();
        });
        if (!refusal.empty())
            return refusal;

        // 2. Routes taken from it output nothing from now on (only reachable with FORCE, or a route raced in).
        channel->close_routes();

        // 3. Release LOCKs: the lifecycle objects bound to client connections refer to this container.
        if (lock)
            lock->clear_locks();

        // 4. Outputs first, while the channel still ticks: screen windows close, DeckLink stops, streams end. Each
        //    consumer is destroyed on its own thread (destroy_consumer_proxy), as with AMCP REMOVE.
        const auto consumers = channel->output().clear();

        // 5. Layers: producers go to the producer destroyer thread (HTML/CEF, FFmpeg, DeckLink input), as with CLEAR.
        auto cleared = channel->stage()->clear();
        if (cleared.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            CASPAR_LOG(warning) << L"CHANNEL REMOVE " << channel_id << L": stage clear still pending after 5 s";

        // 6. The video_channel itself (its thread, mixer, empty stage and output) is destroyed when the last
        //    snapshot holding it is dropped. That is normally this command's own snapshot, released on this AMCP
        //    queue thread right after the reply; a command running on another queue can hold it a little longer.
        //    No producer or consumer keeps a strong reference to a channel (WO-1020 investigation), so this is
        //    never the channel's own thread. The destructor only has to join a thread that has no work left.
        channel.reset();

        CASPAR_LOG(info) << L"CHANNEL REMOVE: channel " << channel_id << L" removed (" << consumers
                         << L" consumers" << (force ? L", FORCE" : L"") << L")";
        return L"202 CHANNEL REMOVE OK\r\n";
    }

    // HighAsCG WO-1020: INFO CONFIG with <channels> as they are now. Removed slots are <channel removed="true"/>,
    // so the numbering of the list still matches the live channel numbers.
    boost::property_tree::wptree live_config()
    {
        auto pt = env::properties();

        boost::property_tree::wptree channels;
        {
            std::lock_guard<std::mutex> lock(slot_xml_mutex_);
            for (auto& xml : slot_xml_) {
                if (xml) {
                    channels.add_child(L"channel", *xml);
                } else {
                    boost::property_tree::wptree removed;
                    removed.put(L"<xmlattr>.removed", L"true");
                    channels.add_child(L"channel", removed);
                }
            }
        }
        pt.put_child(L"configuration.channels", channels);
        return pt;
    }

    void setup_osc(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;
        using namespace boost::asio::ip;

        auto default_port                 = pt.get<unsigned short>(L"configuration.osc.default-port", 6250);
        auto disable_send_to_amcp_clients = pt.get(L"configuration.osc.disable-send-to-amcp-clients", false);
        auto predefined_clients           = pt.get_child_optional(L"configuration.osc.predefined-clients");

        if (predefined_clients) {
            for (auto& predefined_client :
                 pt | witerate_children(L"configuration.osc.predefined-clients") | welement_context_iteration) {
                ptree_verify_element_name(predefined_client, L"predefined-client");

                const auto address = ptree_get<std::wstring>(predefined_client.second, L"address");
                const auto port    = ptree_get<unsigned short>(predefined_client.second, L"port");

                boost::system::error_code ec;
                auto                      ipaddr = make_address_v4(u8(address), ec);
                if (!ec)
                    predefined_osc_subscriptions_.push_back(
                        osc_client_->get_subscription_token(udp::endpoint(ipaddr, port)));
                else
                    CASPAR_LOG(warning) << "Invalid OSC client. Must be valid ipv4 address: " << address;
            }
        }

        if (!disable_send_to_amcp_clients && primary_amcp_server_)
            primary_amcp_server_->add_client_lifecycle_object_factory(
                [=, this](const std::string& ipv4_address) -> std::pair<std::wstring, std::shared_ptr<void>> {
                    using namespace boost::asio::ip;

                    return std::make_pair(std::wstring(L"osc_subscribe"),
                                          osc_client_->get_subscription_token(
                                              udp::endpoint(make_address_v4(ipv4_address), default_port)));
                });
    }

    void setup_channel_producers_and_consumers(const std::vector<boost::property_tree::wptree>& xml_channels)
    {
        auto console_client = spl::make_shared<IO::ConsoleClientInfo>();

        const auto channels = channels_->snapshot();

        std::vector<spl::shared_ptr<core::video_channel>> channels_vec;
        for (auto& cc : *channels) {
            channels_vec.emplace_back(cc.raw_channel);
        }

        for (auto& channel : *channels) {
            core::diagnostics::scoped_call_context save;
            core::diagnostics::call_context::for_thread().video_channel = channel.raw_channel->index();

            auto xml_channel = xml_channels.at(channel.raw_channel->index() - 1);

            // Consumers
            if (xml_channel.get_child_optional(L"consumers")) {
                for (auto& xml_consumer : xml_channel | witerate_children(L"consumers") | welement_context_iteration) {
                    auto name = xml_consumer.first;

                    try {
                        if (name != L"<xmlcomment>")
                            channel.raw_channel->output().add(
                                consumer_registry_->create_consumer(name,
                                                                    xml_consumer.second,
                                                                    video_format_repository_,
                                                                    channels_vec,
                                                                    channel.raw_channel->get_consumer_channel_info()));
                    } catch (...) {
                        CASPAR_LOG_CURRENT_EXCEPTION();
                    }
                }
            }

            // Producers
            if (xml_channel.get_child_optional(L"producers")) {
                for (auto& xml_producer : xml_channel | witerate_children(L"producers") | welement_context_iteration) {
                    ptree_verify_element_name(xml_producer, L"producer");

                    const std::wstring command = xml_producer.second.get_value(L"");
                    const auto         attrs   = xml_producer.second.get_child(L"<xmlattr>");
                    const int          id      = attrs.get(L"id", -1);

                    try {
                        std::list<std::wstring> tokens{
                            L"PLAY", (boost::wformat(L"%i-%i") % channel.raw_channel->index() % id).str()};
                        IO::tokenize(command, tokens);
                        auto cmd = amcp_command_repo_->parse_command(console_client, tokens, L"");

                        if (cmd) {
                            std::wstring res = cmd->Execute(channels_->snapshot()).get();
                            console_client->send(std::move(res), false);
                        }
                    } catch (const user_error&) {
                        CASPAR_LOG(error) << "Failed to parse command: " << command;
                    } catch (...) {
                        CASPAR_LOG_CURRENT_EXCEPTION();
                    }
                }
            }
        }
    }

    void setup_amcp_command_repo()
    {
        amcp_command_repo_ = std::make_shared<amcp::amcp_command_repository>(channels_);

        auto ogl_device = accelerator_.get_device();
        auto ctx        = std::make_shared<amcp::amcp_command_static_context>(
            video_format_repository_,
            cg_registry_,
            producer_registry_,
            consumer_registry_,
            amcp_command_repo_,
            shutdown_server_now_,
            u8(caspar::env::properties().get(L"configuration.amcp.media-server.host", L"127.0.0.1")),
            u8(caspar::env::properties().get(L"configuration.amcp.media-server.port", L"8000")),
            ogl_device,
            spl::make_shared_ptr(osc_client_));

        // HighAsCG WO-1020
        ctx->channels_admin.add_channel = [this](const core::video_format_desc& format_desc,
                                                 int                            color_depth,
                                                 const std::wstring&            color_space,
                                                 int at) { return add_channel(format_desc, color_depth, color_space, at); };
        ctx->channels_admin.remove_channel = [this](int channel_id, bool force, const IO::ClientInfoPtr& client) {
            return remove_channel(channel_id, force, client);
        };
        ctx->channels_admin.live_config = [this] { return live_config(); };

        amcp_context_factory_ = std::make_shared<amcp::command_context_factory>(ctx);

        amcp_command_repo_wrapper_ =
            std::make_shared<amcp::amcp_command_repository_wrapper>(amcp_command_repo_, amcp_context_factory_);

        amcp::register_commands(amcp_command_repo_wrapper_);
    }

    void setup_controllers(const boost::property_tree::wptree& pt)
    {
        using boost::property_tree::wptree;
        for (auto& xml_controller : pt | witerate_children(L"configuration.controllers") | welement_context_iteration) {
            auto name     = xml_controller.first;
            auto protocol = ptree_get<std::wstring>(xml_controller.second, L"protocol");

            if (name == L"tcp") {
                auto port = ptree_get<unsigned int>(xml_controller.second, L"port");

                try {
                    auto asyncbootstrapper = spl::make_shared<IO::AsyncEventServer>(
                        io_context_,
                        create_protocol(protocol, L"TCP Port " + std::to_wstring(port)),
                        static_cast<short>(port));
                    async_servers_.push_back(asyncbootstrapper);

                    if (!primary_amcp_server_ && boost::iequals(protocol, L"AMCP"))
                        primary_amcp_server_ = asyncbootstrapper;
                } catch (...) {
                    CASPAR_LOG(fatal) << L"Failed to setup " << protocol << L" controller on port "
                                      << boost::lexical_cast<std::wstring>(port) << L". It is likely already in use";
                    throw;
                    // CASPAR_LOG_CURRENT_EXCEPTION();
                }
            } else
                CASPAR_LOG(warning) << "Invalid controller: " << name;
        }
    }

    IO::protocol_strategy_factory<char>::ptr create_protocol(const std::wstring& name,
                                                             const std::wstring& port_description) const
    {
        using namespace IO;

        if (boost::iequals(name, L"AMCP"))
            return amcp::create_char_amcp_strategy_factory(port_description, spl::make_shared_ptr(amcp_command_repo_));

        CASPAR_THROW_EXCEPTION(user_error() << msg_info(L"Invalid protocol: " + name));
    }
};

server::server(std::function<void(bool)> shutdown_server_now)
    : impl_(new impl(std::move(shutdown_server_now)))
{
}
void                                                     server::start() { impl_->start(); }
spl::shared_ptr<protocol::amcp::amcp_command_repository> server::get_amcp_command_repository() const
{
    return spl::make_shared_ptr(impl_->amcp_command_repo_);
}

} // namespace caspar
