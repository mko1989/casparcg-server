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

#include "oal_consumer.h"

#include <common/diagnostics/graph.h>
#include <common/env.h>
#include <common/except.h>
#include <common/executor.h>
#include <common/future.h>
#include <common/log.h>
#include <common/param.h>
#include <common/timer.h>
#include <common/utf.h>

#include <core/consumer/channel_info.h>
#include <core/consumer/frame_consumer.h>
#include <core/frame/frame.h>
#include <core/video_format.h>

#include <boost/algorithm/string/erase.hpp>
#include <boost/algorithm/string/predicate.hpp>
#include <boost/property_tree/ptree.hpp>

#include <tbb/concurrent_queue.h>

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4244)
#endif
extern "C" {
#define __STDC_CONSTANT_MACROS
#define __STDC_LIMIT_MACROS
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <AL/al.h>
#include <AL/alc.h>
#define AL_ALEXT_PROTOTYPES
#include <AL/alext.h>

namespace caspar { namespace oal {

using namespace std::chrono_literals;

// One OpenAL device + context per output device, shared by every consumer that resolves to it
// (HighAsCG WO-847, the device/context half of upstream #1714 that 2.5.1 ships and the Enhanced
// OAL consumer lacked). Before: each consumer opened its own device and made its context
// current PROCESS-WIDE with alcMakeContextCurrent, so with two <system-audio> consumers the
// second one's context received every AL call of the first (same source / buffer names), and a
// second consumer on an already open ALSA device failed with "Failed to initialize audio device".
// Now each thread that talks to OpenAL selects the consumer's context with alcSetThreadContext
// (ALC_EXT_thread_local_context), and consumers on the same device share it with one source each.
class device
{
    ALCdevice*  device_       = nullptr;
    ALCcontext* context_      = nullptr;
    bool        thread_local_ = false;
    std::string name_;

    explicit device(const ALCchar* name)
        : name_(name ? name : "")
    {
        device_ = alcOpenDevice(name);
        if (!device_) CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Failed to initialize audio device."));

        context_ = alcCreateContext(device_, nullptr);
        if (!context_) {
            alcCloseDevice(device_);
            CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Failed to create audio context."));
        }

        thread_local_ = alcIsExtensionPresent(device_, "ALC_EXT_thread_local_context") == ALC_TRUE;
        if (!thread_local_) {
            CASPAR_LOG(warning) << L"OpenAL: ALC_EXT_thread_local_context missing, using the process-wide "
                                   L"context (only one system-audio device can play)";
            if (!alcMakeContextCurrent(context_)) {
                alcDestroyContext(context_);
                alcCloseDevice(device_);
                CASPAR_THROW_EXCEPTION(invalid_operation() << msg_info("Failed to activate audio context."));
            }
        }
    }

    static std::mutex&                                  registry_mutex()
    {
        static std::mutex m;
        return m;
    }
    static std::map<std::string, std::weak_ptr<device>>& registry()
    {
        static std::map<std::string, std::weak_ptr<device>> r;
        return r;
    }

  public:
    device(const device&)            = delete;
    device& operator=(const device&) = delete;

    ~device()
    {
        if (thread_local_) {
            if (alcGetThreadContext() == context_)
                alcSetThreadContext(nullptr);
        } else if (alcGetCurrentContext() == context_) {
            alcMakeContextCurrent(nullptr);
        }
        if (context_) alcDestroyContext(context_);
        if (device_)  alcCloseDevice(device_);
    }

    // Resolve the configured name like before (space-insensitive match against the OpenAL list,
    // unknown name -> default device) and return the shared device for the resolved name.
    static std::shared_ptr<device> open(const std::wstring& requested)
    {
        std::lock_guard<std::mutex> lock(registry_mutex());

        const ALCchar* name = nullptr;
        if (!requested.empty()) {
            if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT")) {
                auto s = alcGetString(nullptr, alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT")
                    ? ALC_ALL_DEVICES_SPECIFIER
                    : ALC_DEVICE_SPECIFIER
                );
                name = iterate_and_find_device(s, requested);

                if (name)
                    CASPAR_LOG(debug) << L"Found specified OpenAL output device";
                else
                    CASPAR_LOG(warning) << L"Failed to find specified OpenAL output device. Using default device";
            } else {
                CASPAR_LOG(info) << L"Unable to enumerate OpenAL devices. Using default device";
            }
        }

        auto  key    = std::string(name ? name : "");
        auto& weak   = registry()[key];
        auto  shared = weak.lock();
        if (shared) {
            CASPAR_LOG(info) << L"OpenAL: sharing the already open device "
                             << (key.empty() ? std::wstring(L"(default)") : u16(key));
            return shared;
        }
        shared = std::shared_ptr<device>(new device(name));
        weak   = shared;
        return shared;
    }

    // Select this device's context for the calling thread. Every AL call of a consumer runs on its
    // initialize/teardown executor or on the channel's output thread; both call this first.
    void activate()
    {
        if (thread_local_) {
            if (alcGetThreadContext() != context_)
                alcSetThreadContext(context_);
        } else if (alcGetCurrentContext() != context_) {
            alcMakeContextCurrent(context_);
        }
    }

    void deactivate()
    {
        if (thread_local_ && alcGetThreadContext() == context_)
            alcSetThreadContext(nullptr);
    }

  private:
    static const ALCchar* iterate_and_find_device(const char* list, const std::wstring& device_name)
    {
        const ALCchar* result = nullptr;

        std::string short_device_name = u8(device_name);
        boost::algorithm::erase_all(short_device_name, " ");

        CASPAR_LOG(info) << L"------- OpenAL Device List -----";

        if (!list) {
            CASPAR_LOG(info) << L"No device names found";
        } else {
            const ALCchar* ptr = list;
            while (strlen(ptr) > 0) {
                CASPAR_LOG(info) << ptr;

                std::string tmpStr = ptr;
                boost::algorithm::erase_all(tmpStr, " ");
                if (boost::iequals(short_device_name, tmpStr))
                    result = ptr;

                ptr += strlen(ptr) + 1;
            }
        }

        CASPAR_LOG(info) << L"------ OpenAL Devices List done -----";
        return result;
    }
};

struct oal_consumer : public core::frame_consumer
{
    spl::shared_ptr<diagnostics::graph> graph_;
    caspar::timer                       perf_timer_;
    int                                 channel_index_ = -1;

    core::video_format_desc format_desc_;

    std::shared_ptr<device> device_;
    ALuint              source_ = 0;
    std::vector<ALuint> buffers_;
    std::queue<ALuint>  free_;

    std::atomic<bool> stop_{false};

    struct swr_deleter { void operator()(SwrContext* p) { swr_free(&p); } };
    std::unique_ptr<SwrContext, swr_deleter> swr_;

    // Executor used only for initialize() — send() runs entirely on the
    // output thread via the deferred future to avoid cross-thread deadlock.
    executor executor_{L"oal_consumer"};

  public:
    explicit oal_consumer(const std::wstring& device_name) : device_{device::open(device_name)}
    {
        graph_->set_color("tick-time", diagnostics::color(0.0f, 0.6f, 0.9f));
        graph_->set_color("dropped-frame", diagnostics::color(0.3f, 0.6f, 0.3f));
        graph_->set_color("late-frame", diagnostics::color(0.6f, 0.3f, 0.3f));
        diagnostics::register_graph(graph_);
    }

    ~oal_consumer() override
    {
        stop_ = true;

        executor_.invoke([=, this] {
            if (source_) {
                device_->activate();
                alSourceStop(source_);
                alDeleteSources(1, &source_);
                alDeleteBuffers(static_cast<ALsizei>(buffers_.size()), buffers_.data());
                device_->deactivate();
            }
        });
    }

    void initialize(const core::video_format_desc& format_desc,
                    const core::channel_info&      channel_info,
                    int                            port_index) override
    {
        format_desc_   = format_desc;
        channel_index_ = channel_info.index;
        graph_->set_text(print());

        executor_.invoke([=, this] {
            AVChannelLayout from, to;
            av_channel_layout_default(&from, format_desc_.audio_channels);
            av_channel_layout_default(&to, 2); // stereo

            SwrContext* raw = nullptr; // swr_alloc_set_opts2 reuses a non-null *ps (upstream 85fa10952)
            swr_alloc_set_opts2(&raw,
                &to,   AV_SAMPLE_FMT_S16, format_desc_.audio_sample_rate,
                &from, AV_SAMPLE_FMT_S32, format_desc_.audio_sample_rate,
                0, nullptr
            );
            swr_.reset(raw);
            swr_init(raw);

            device_->activate();
            buffers_.resize(4);
            alGenBuffers(static_cast<ALsizei>(buffers_.size()), buffers_.data());
            for (auto buf : buffers_) free_.push(buf);

            alGenSources(1, &source_);

            // pre-queue one frame of silence
            std::vector<int16_t> silence(format_desc_.audio_cadence[0] * 2, 0);
            auto buf = free_.front();
            free_.pop();
            alBufferData(buf, AL_FORMAT_STEREO16, silence.data(),
                static_cast<ALsizei>(silence.size() * sizeof(std::int16_t)),
                format_desc_.audio_sample_rate
            );
            alSourceQueueBuffers(source_, 1, &buf);
        });
    }

    std::future<bool> send(core::video_field field, core::const_frame frame) override
    {
        // Everything runs in the deferred future on output.cpp's thread when
        // .get() is called. This avoids cross-thread deadlock — no shared
        // mutex or condition variable needed between send() and the future.
        // The channel tick is gated to OpenAL's playback position, keeping
        // screen consumer and OAL frame-accurately synchronized with no
        // additional threads.
        return std::async(std::launch::deferred, [this, frame = std::move(frame)] {
            // This consumer's context on this channel's output thread for this frame only (WO-847);
            // released on return so a channel thread never exits holding it.
            device_->activate();
            struct release_context
            {
                device& d;
                ~release_context() { d.deactivate(); }
            } release{*device_};

            // Step 1: reclaim any buffers OpenAL has already finished playing
            {
                ALint done = 0;
                alGetSourcei(source_, AL_BUFFERS_PROCESSED, &done);
                while (done-- > 0) {
                    ALuint buf;
                    alSourceUnqueueBuffers(source_, 1, &buf);
                    free_.push(buf);
                }
            }

            // Step 2: resample and downmix to stereo int16
            auto&& audio_in    = frame.audio_data();
            auto   num_samples = static_cast<int>(audio_in.size() / format_desc_.audio_channels);
            std::vector<std::int16_t> audio_out(num_samples * 2);
            auto from = reinterpret_cast<const std::uint8_t*>(audio_in.data());
            auto to   = reinterpret_cast<std::uint8_t*>(audio_out.data());
            swr_convert(swr_.get(), &to, num_samples, &from, num_samples);

            // Step 3: wait for a free buffer if pool is exhausted
            while (free_.empty() && !stop_) {
                std::this_thread::sleep_for(100us);
                ALint done = 0;
                alGetSourcei(source_, AL_BUFFERS_PROCESSED, &done);
                while (done-- > 0) {
                    ALuint buf;
                    alSourceUnqueueBuffers(source_, 1, &buf);
                    free_.push(buf);
                }
            }
            if (stop_) return false;

            // Step 4: queue the audio buffer
            auto buf = free_.front();
            free_.pop();
            alBufferData(buf, AL_FORMAT_STEREO16, audio_out.data(),
                static_cast<ALsizei>(audio_out.size() * sizeof(std::int16_t)),
                format_desc_.audio_sample_rate
            );
            alSourceQueueBuffers(source_, 1, &buf);

            ALenum state = 0;
            alGetSourcei(source_, AL_SOURCE_STATE, &state);
            if (state != AL_PLAYING) alSourcePlay(source_);

            // Step 5: wait for OpenAL to finish playing one buffer.
            // On the very first frame nothing has played yet so we return
            // immediately to let playback start. On subsequent frames we
            // gate the channel tick to OpenAL's playback position, keeping
            // screen consumer and OAL frame-accurately synchronized.
            ALint queued = 0;
            alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
            if (queued <= 1) {
                // First frame — just start playing, don't wait
                graph_->set_value("tick-time", perf_timer_.elapsed() * format_desc_.fps * 0.5);
                perf_timer_.restart();
                return true;
            }

            while (!stop_) {
                ALint done = 0;
                alGetSourcei(source_, AL_BUFFERS_PROCESSED, &done);
                if (done > 0) {
                    graph_->set_value("tick-time", perf_timer_.elapsed() * format_desc_.fps * 0.5);
                    perf_timer_.restart();
                    return true;
                }
                std::this_thread::sleep_for(100us);
            }
            return false;
        });
    }

    std::wstring print() const override
    {
        return L"oal[" + std::to_wstring(channel_index_) + L"|" + format_desc_.name + L"]";
    }

    std::wstring name() const override { return L"system-audio"; }

    bool has_synchronization_clock() const override { return true; }

    int index() const override { return 500; }

    core::monitor::state state() const override
    {
        static const core::monitor::state empty;
        return empty;
    }
};

spl::shared_ptr<core::frame_consumer> create_consumer(const std::vector<std::wstring>&     params,
                                                      const core::video_format_repository& format_repository,
                                                      const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                                                      const core::channel_info& channel_info)
{
    if (params.empty() || !(boost::iequals(params.at(0), L"AUDIO") || boost::iequals(params.at(0), L"SYSTEM-AUDIO")))
        return core::frame_consumer::empty();

    return spl::make_shared<oal_consumer>(get_param(L"DEVICE_NAME", params, L""));
}

spl::shared_ptr<core::frame_consumer>
create_preconfigured_consumer(const boost::property_tree::wptree&                      ptree,
                              const core::video_format_repository&                     format_repository,
                              const std::vector<spl::shared_ptr<core::video_channel>>& channels,
                              const core::channel_info&                                channel_info)
{
    return spl::make_shared<oal_consumer>(ptree.get(L"device-name", L""));
}

}} // namespace caspar::oal
