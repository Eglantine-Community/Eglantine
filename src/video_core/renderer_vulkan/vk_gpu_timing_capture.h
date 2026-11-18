// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "common/logging.h"
#include "video_core/gpu_timestamp_delta.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

// Opt-in, one-frame diagnostic. GPU timestamps add execution dependencies, so these
// samples locate expensive work; they are not steady-state FPS measurements.
class GpuTimingCapture {
public:
    static constexpr u32 Invalid = std::numeric_limits<u32>::max();

    GpuTimingCapture(const Device& device_, Scheduler& scheduler_)
        : device{device_}, scheduler{scheduler_} {
        if (const char* path = std::getenv("EDEN_NX2_GPU_TIMING_FILE")) {
            trigger = path;
        }
        if (trigger.empty()) {
            return;
        }
        const auto families = device.GetPhysical().GetQueueFamilyProperties();
        valid_bits = families.at(device.GetGraphicsFamily()).timestampValidBits;
        period = device.GetPhysical().GetProperties().limits.timestampPeriod;
        write_timestamp = reinterpret_cast<PFN_vkCmdWriteTimestamp>(
            device.GetDispatchLoader().vkGetDeviceProcAddr(*device.GetLogical(),
                                                          "vkCmdWriteTimestamp"));
    }

    u32 Begin(std::string_view kind, std::span<const u64> shaders, u64 key = 0) {
        if (!capturing || entries.size() == MaxQueries / 2) {
            return Invalid;
        }
        const u32 query = static_cast<u32>(entries.size()) * 2;
        entries.push_back({std::string{kind}, {shaders.begin(), shaders.end()}, key});
        Write(query);
        return query;
    }

    void End(u32 query) {
        if (query != Invalid) {
            Write(query + 1);
        }
    }

    void TickFrame() {
        if (trigger.empty()) {
            return;
        }
        if (capturing) {
            capturing = false;
            if (!entries.empty()) {
                scheduler.Finish();
                std::vector<u64> times(entries.size() * 2);
                const auto result = device.GetLogical().GetQueryResults(
                    *pool, 0, static_cast<u32>(times.size()), times.size() * sizeof(u64),
                    times.data(), sizeof(u64), VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
                if (result == VK_SUCCESS) {
                    Save(times);
                } else {
                    LOG_ERROR(Render_Vulkan, "GPU timing query results failed: {}", int(result));
                }
            }
        }
        std::error_code error;
        if (!std::filesystem::exists(trigger, error) ||
            !std::filesystem::remove(trigger, error)) {
            return;
        }
        if (valid_bits == 0 || write_timestamp == nullptr) {
            LOG_WARNING(Render_Vulkan, "GPU timing capture: timestamps unavailable");
            return;
        }
        if (!pool) {
            pool = device.GetLogical().CreateQueryPool({
                .sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .queryType = VK_QUERY_TYPE_TIMESTAMP,
                .queryCount = MaxQueries,
                .pipelineStatistics = 0,
            });
        }
        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([handle = *pool](vk::CommandBuffer cmdbuf) {
            cmdbuf.ResetQueryPool(handle, 0, MaxQueries);
        });
        entries.clear();
        capturing = true;
    }

private:
    struct Entry {
        std::string kind;
        std::vector<u64> shaders;
        u64 key;
    };

    void Write(u32 query) {
        scheduler.Record([function = write_timestamp, handle = *pool, query](vk::CommandBuffer cb) {
            function(*cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, handle, query);
        });
    }

    void Save(std::span<const u64> times) {
        nlohmann::json report{{"timestamp_period_ns", period},
                              {"timestamp_valid_bits", valid_bits},
                              {"intrusive_single_frame", true},
                              {"truncated", entries.size() == MaxQueries / 2}};
        report["entries"] = nlohmann::json::array();
        double total_ms = 0;
        for (size_t i = 0; i < entries.size(); ++i) {
            const auto& entry = entries[i];
            const double start_ms = static_cast<double>(VideoCommon::GpuTimestampDelta(
                                        times.front(), times[i * 2], valid_bits)) * period / 1e6;
            const double gap_ms = i == 0 ? 0.0 :
                static_cast<double>(VideoCommon::GpuTimestampDelta(
                    times[i * 2 - 1], times[i * 2], valid_bits)) * period / 1e6;
            const double ms = static_cast<double>(VideoCommon::GpuTimestampDelta(
                                  times[i * 2], times[i * 2 + 1], valid_bits)) * period / 1e6;
            total_ms += ms;
            report["entries"].push_back({{"kind", entry.kind}, {"shaders", entry.shaders},
                                          {"key", entry.key}, {"ms", ms},
                                          {"start_ms", start_ms}, {"gap_ms", gap_ms}});
        }
        report["measured_ms"] = total_ms;
        report["span_ms"] = static_cast<double>(VideoCommon::GpuTimestampDelta(
                                  times.front(), times.back(), valid_bits)) * period / 1e6;
        auto output = trigger;
        output += ".json";
        std::ofstream file{output};
        file << report.dump(2);
        LOG_INFO(Render_Vulkan, "GPU timing capture: {} intervals, {:.3f} ms; {}",
                 entries.size(), total_ms, output.string());
    }

    static constexpr u32 MaxQueries = 8192;
    const Device& device;
    Scheduler& scheduler;
    std::filesystem::path trigger;
    vk::QueryPool pool;
    PFN_vkCmdWriteTimestamp write_timestamp{};
    std::vector<Entry> entries;
    u32 valid_bits{};
    double period{};
    bool capturing{};
};

} // namespace Vulkan
