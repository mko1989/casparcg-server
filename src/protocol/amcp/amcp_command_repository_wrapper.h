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

#include "amcp_command_context.h"

#include <common/except.h>

namespace caspar { namespace protocol { namespace amcp {

typedef std::function<std::future<std::wstring>(command_context& args)> amcp_command_impl_func_future;
typedef std::function<std::wstring(command_context& args)>              amcp_command_impl_func;

class command_context_factory
{
  public:
    command_context_factory(std::shared_ptr<amcp_command_static_context> static_context)
        : static_context_(std::move(static_context))
    {
    }

    command_context create(const command_context_simple&                        simple_ctx,
                           const spl::shared_ptr<std::vector<channel_context>>& channels) const
    {
        const channel_context channel =
            simple_ctx.channel_index >= 0 ? checked_channel(simple_ctx, channels) : channel_context();
        auto ctx = command_context(
            static_context_, channels, simple_ctx.client, channel, simple_ctx.channel_index, simple_ctx.layer_id);
        ctx.parameters = std::move(simple_ctx.parameters);
        return ctx;
    }

  private:
    // HighAsCG WO-1020: the slot may have been emptied (CHANNEL REMOVE) or refilled (CHANNEL ADD ... AT) between
    // parsing and execution. Fail the command (403) rather than touch an empty slot or a different channel.
    static const channel_context& checked_channel(const command_context_simple&                        simple_ctx,
                                                  const spl::shared_ptr<std::vector<channel_context>>& channels)
    {
        const auto& slot = channels->at(simple_ctx.channel_index);
        const auto& want = simple_ctx.target_channel;
        const bool  had_target =
            want.owner_before(std::weak_ptr<core::video_channel>{}) || std::weak_ptr<core::video_channel>{}.owner_before(want);
        if (slot.empty() ||
            (had_target && (want.owner_before(slot.raw_channel) || slot.raw_channel.owner_before(want))))
            CASPAR_THROW_EXCEPTION(expected_user_error() << msg_info(L"Channel " +
                                                                     std::to_wstring(simple_ctx.channel_index + 1) +
                                                                     L" was removed or replaced"));
        return slot;
    }

    std::shared_ptr<amcp_command_static_context> static_context_;
};

class amcp_command_repository_wrapper
{
  public:
    amcp_command_repository_wrapper(std::shared_ptr<amcp_command_repository> repo,
                                    std::shared_ptr<command_context_factory> context_factory)
        : repo_(repo)
        , context_factory_(context_factory)
    {
    }

    void register_command(std::wstring                  category,
                          std::wstring                  name,
                          amcp_command_impl_func_future command,
                          int                           min_num_params);

    void register_command(std::wstring category, std::wstring name, amcp_command_impl_func command, int min_num_params);

    void register_channel_command(std::wstring                  category,
                                  std::wstring                  name,
                                  amcp_command_impl_func_future command,
                                  int                           min_num_params);

    void register_channel_command(std::wstring           category,
                                  std::wstring           name,
                                  amcp_command_impl_func command,
                                  int                    min_num_params);

  private:
    std::shared_ptr<amcp_command_repository> repo_;
    std::weak_ptr<command_context_factory>   context_factory_;
};

}}} // namespace caspar::protocol::amcp
