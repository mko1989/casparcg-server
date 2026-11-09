#pragma once

#include "../util/ClientInfo.h"
#include "../util/lock_container.h"
#include <core/producer/stage.h>
#include <core/video_channel.h>
#include <common/memory.h>
#include <mutex>
#include <utility>
#include <vector>

namespace caspar { namespace protocol { namespace amcp {

class channel_context
{
  public:
    explicit channel_context() {}
    explicit channel_context(std::shared_ptr<core::video_channel> c,
                             std::shared_ptr<core::stage_base>    s,
                             const std::wstring&                  lifecycle_key)
        : raw_channel(std::move(c))
        , stage(std::move(s))
        , lock(std::make_shared<caspar::IO::lock_container>(lifecycle_key))
        , lifecycle_key_(lifecycle_key)
    {
    }
    const std::shared_ptr<core::video_channel>        raw_channel;
    const std::shared_ptr<core::stage_base>           stage;
    const std::shared_ptr<caspar::IO::lock_container> lock;
    const std::wstring                                lifecycle_key_;

    // HighAsCG WO-1020: a removed channel leaves an empty slot (default-constructed context), so channel numbers,
    // which are positions in the list, never shift.
    bool empty() const { return !raw_channel; }
};

// HighAsCG WO-1020: the channel list is copy-on-write so CHANNEL ADD / CHANNEL REMOVE can change it at runtime.
// Readers take a snapshot (an immutable vector; never modify it) and use it without further locking; it keeps
// every channel in it alive while held. Writers are serialised and publish a new vector.
class channel_list
{
  public:
    using snapshot_t = spl::shared_ptr<std::vector<channel_context>>;

    snapshot_t snapshot() const
    {
        std::lock_guard<std::mutex> lock(snapshot_mutex_);
        return snapshot_;
    }

    // f(std::vector<channel_context>& next) edits a copy; the copy is published only if f returns normally.
    template <typename F>
    auto modify(F&& f)
    {
        std::lock_guard<std::mutex> write_lock(write_mutex_);
        snapshot_t                  next(spl::make_shared<std::vector<channel_context>>(*snapshot()));
        auto                        result = f(*next);
        {
            std::lock_guard<std::mutex> lock(snapshot_mutex_);
            snapshot_ = std::move(next);
        }
        return result;
    }

  private:
    mutable std::mutex snapshot_mutex_;
    std::mutex         write_mutex_;
    snapshot_t         snapshot_;
};

struct command_context_simple
{
    const IO::ClientInfoPtr         client;
    const int                       channel_index;
    const int                       layer_id;
    const std::vector<std::wstring> parameters;
    // HighAsCG WO-1020: the channel this command was parsed against. If the slot holds a different channel when the
    // command runs (removed, or removed and re-added AT the same number), the command fails instead of landing on
    // the newcomer.
    std::weak_ptr<core::video_channel> target_channel;

    int layer_index(int default_ = 0) const { return layer_id == -1 ? default_ : layer_id; }

    command_context_simple(IO::ClientInfoPtr                client,
                           int                              channel_index,
                           int                              layer_id,
                           const std::vector<std::wstring>& parameters)
        : client(std::move(client))
        , channel_index(channel_index)
        , layer_id(layer_id)
        , parameters(parameters)
    {
    }
};

typedef std::function<std::future<std::wstring>(const command_context_simple&                        args,
                                                const spl::shared_ptr<std::vector<channel_context>>& channels)>
    amcp_command_func;

}}} // namespace caspar::protocol::amcp
