// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "audio_core/sink/sink.h"
#include "audio_core/sink/sink_stream.h"

namespace Core {
class System;
} // namespace Core

namespace AudioCore::Sink {
class NullSinkStreamImpl final : public SinkStream {
public:
    explicit NullSinkStreamImpl(Core::System& system_, StreamType type_)
        : SinkStream{system_, type_} {}
    ~NullSinkStreamImpl() override = default;

    void Start(bool /*resume*/ = false) override {
        std::scoped_lock lk{pace_mutex};
        paused = false;
        next_deadline = std::chrono::steady_clock::now();
    }

    void Stop() override {
        {
            std::scoped_lock lk{pace_mutex};
            paused = true;
        }
        pace_cv.notify_all();
    }

    void AppendBuffer(SinkBuffer& buffer, std::span<s16>) override {
        if (type == StreamType::In) {
            return;
        }
        const u64 frames = buffer.frames != 0 ? buffer.frames : TargetSampleCount;
        AdvancePlayedSampleCount(frames);
        const auto duration =
            std::chrono::nanoseconds{(frames * 1'000'000'000ULL) / TargetSampleRate};
        const auto now = std::chrono::steady_clock::now();
        std::scoped_lock lk{pace_mutex};
        if (next_deadline < now) {
            next_deadline = now;
        }
        next_deadline += duration;
    }

    void WaitFreeSpace(std::stop_token stop_token) override {
        constexpr auto max_lead =
            std::chrono::nanoseconds{(u64{TargetSampleCount} * 3 * 1'000'000'000ULL) /
                                     TargetSampleRate};
        std::unique_lock lk{pace_mutex};
        const auto now = std::chrono::steady_clock::now();
        if (next_deadline > now + max_lead) {
            pace_cv.wait_until(lk, stop_token, next_deadline - max_lead,
                               [this] { return paused.load(std::memory_order_relaxed); });
        }
    }

    std::vector<s16> ReleaseBuffer(u64) override {
        return {};
    }

private:
    std::mutex pace_mutex;
    std::condition_variable_any pace_cv;
    std::chrono::steady_clock::time_point next_deadline{};
};

/**
 * A no-op sink for when no audio out is wanted.
 */
class NullSink final : public Sink {
public:
    explicit NullSink(std::string_view) {}
    ~NullSink() override = default;

    SinkStream* AcquireSinkStream(Core::System& system, u32, const std::string&,
                                  StreamType type) override {
        if (null_sink == nullptr) {
            null_sink = std::make_unique<NullSinkStreamImpl>(system, type);
            null_sink->SetDeviceVolume(device_volume);
        }
        return null_sink.get();
    }

    void CloseStream(SinkStream*) override {}
    void CloseStreams() override {}
    f32 GetDeviceVolume() const override {
        return device_volume;
    }
    void SetDeviceVolume(f32 volume) override {
        device_volume = volume;
        if (null_sink != nullptr) {
            null_sink->SetDeviceVolume(volume);
        }
    }
    void SetSystemVolume(f32 volume) override {}

private:
    SinkStreamPtr null_sink{};
};

} // namespace AudioCore::Sink
