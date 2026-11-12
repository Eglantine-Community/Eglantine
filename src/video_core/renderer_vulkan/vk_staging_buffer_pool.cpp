// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2022 yuzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

#include <fmt/ranges.h>

#include "common/alignment.h"
#include "common/assert.h"
#include "common/bit_util.h"
#include "common/common_types.h"
#include "common/literals.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {

using namespace Common::Literals;

// Maximum potential alignment of a Vulkan buffer
constexpr VkDeviceSize MAX_ALIGNMENT = 256;

// Stream buffer size in bytes
// *NIX drivers are more sensitive to increased buffers for streaming.
// Windows ones however, can intake bigger buffers and generally do not OOM.
// - GTX 960 on Windows will not OOM with 256mib
// - GT 1030 on ^NIX will OOM with 256mib
#if defined(__FreeBSD__)
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#else
// 128 MiB on Windows too: 256 MiB sat mostly unused in device memory on Switch 2 titles.
constexpr VkDeviceSize MAX_STREAM_BUFFER_SIZE = 128_MiB;
#endif

size_t GetStreamBufferSize(const Device& device) {
    if (!device.HasDebuggingToolAttached()) {
        if (device.IsIntegrated()) {
            // An integrated GPU's device-local host-visible heap can be no bigger than the stream
            // buffer (Radeon 780M: 256 MiB, 243 MiB budget), which then sat over that budget.
            // Leave a quarter of it free.
            VkDeviceSize heap_size = 0;
            ForEachDeviceLocalHostVisibleHeap(
                device, [&heap_size](size_t, VkMemoryHeap& heap) {
                    heap_size = (std::max)(heap_size, heap.size);
                });
            if (heap_size != 0 && heap_size <= MAX_STREAM_BUFFER_SIZE) {
                return Common::AlignUp(heap_size * 3 / 4, MAX_ALIGNMENT);
            }
        }
        return MAX_STREAM_BUFFER_SIZE;
    }

    VkDeviceSize size{0};
    bool has_device_local_host_visible_heap{};
    ForEachDeviceLocalHostVisibleHeap(device, [&size, &has_device_local_host_visible_heap](
                                                  size_t index, VkMemoryHeap& heap) {
        has_device_local_host_visible_heap = true;
        size = (std::max)(size, heap.size);
    });
    if (has_device_local_host_visible_heap) {
        // If rebar is not supported, cut the max heap size to 40%. This will allow 2 captures to be
        // loaded at the same time in RenderDoc. If rebar is supported, this shouldn't be an issue
        // as the heap will be much larger.
        if (size <= MAX_STREAM_BUFFER_SIZE) {
            size = size * 40 / 100;
        }
    } else {
        size = MAX_STREAM_BUFFER_SIZE;
    }
    return (std::min)(Common::AlignUp(size, MAX_ALIGNMENT), MAX_STREAM_BUFFER_SIZE);
}
} // Anonymous namespace

StagingBufferPool::StagingBufferPool(const Device& device_, MemoryAllocator& memory_allocator_,
                                     Scheduler& scheduler_)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      stream_buffer_size{GetStreamBufferSize(device)}, region_size{stream_buffer_size /
                                                                   StagingBufferPool::NUM_SYNCS} {
    const char* trace = std::getenv("EDEN_TRACE_STAGING");
    trace_staging = trace && trace[0] == '1';
    VkBufferCreateInfo stream_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = stream_buffer_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    stream_buffer = memory_allocator.CreateBuffer(stream_ci, MemoryUsage::Stream);
    if (device.HasDebuggingToolAttached()) {
        stream_buffer.SetObjectNameEXT("Stream Buffer");
    }
    if (device.IsBufferDeviceAddressSupported()) {
        stream_buffer_address = device.GetLogical().GetBufferDeviceAddress(*stream_buffer);
    }
    stream_pointer = stream_buffer.Mapped();
    ASSERT_MSG(!stream_pointer.empty(), "Stream buffer must be host visible!");
}

StagingBufferPool::~StagingBufferPool() = default;

StagingBufferRef StagingBufferPool::Request(size_t size, MemoryUsage usage, bool deferred) {
    if (!deferred && usage == MemoryUsage::Upload && size <= region_size) {
        return GetStreamBuffer(size);
    }
    return GetStagingBuffer(size, usage, deferred);
}

void StagingBufferPool::FreeDeferred(StagingBufferRef& ref) {
    ReleaseDeferred(ref, scheduler.CurrentTick());
}

void StagingBufferPool::FreeCompletedDeferred(StagingBufferRef& ref) {
    // Unlike a just-recorded upload, a completed readback has no pending GPU use.
    ReleaseDeferred(ref, 0);
}

void StagingBufferPool::ReleaseDeferred(StagingBufferRef& ref, u64 last_use_tick) {
    auto& entries = GetCache(ref.usage)[ref.log2_level].entries;
    const auto is_this_one = [&ref](const StagingBuffer& entry) {
        return entry.index == ref.index;
    };
    auto it = std::find_if(entries.begin(), entries.end(), is_this_one);
    ASSERT(it != entries.end());
    ASSERT(it->deferred);
    it->tick = last_use_tick;
    it->deferred = false;
}

void StagingBufferPool::TickFrame() {
    constexpr size_t LARGE_STAGING_LOG2 = 20;
    current_delete_level = (current_delete_level + 1) % LARGE_STAGING_LOG2;

    ReleaseCache(MemoryUsage::DeviceLocal);
    ReleaseCache(MemoryUsage::Upload);
    ReleaseCache(MemoryUsage::Download);
}

StagingBufferRef StagingBufferPool::GetStreamBuffer(size_t size) {
    if (AreRegionsActive(Region(free_iterator) + 1,
                         (std::min)(Region(iterator + size) + 1, NUM_SYNCS))) {
        // Avoid waiting for the previous usages to be free
        return GetStagingBuffer(size, MemoryUsage::Upload);
    }
    const u64 current_tick = scheduler.CurrentTick();
    std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + Region(iterator),
              current_tick);
    used_iterator = iterator;
    free_iterator = (std::max)(free_iterator, iterator + size);

    if (iterator + size >= stream_buffer_size) {
        std::fill(sync_ticks.begin() + Region(used_iterator), sync_ticks.begin() + NUM_SYNCS,
                  current_tick);
        used_iterator = 0;
        iterator = 0;
        free_iterator = size;

        if (AreRegionsActive(0, Region(size) + 1)) {
            // Avoid waiting for the previous usages to be free
            return GetStagingBuffer(size, MemoryUsage::Upload);
        }
    }
    const size_t offset = iterator;
    iterator = Common::AlignUp(iterator + size, MAX_ALIGNMENT);
    return StagingBufferRef{
        .buffer = *stream_buffer,
        .device_address = stream_buffer_address,
        .offset = static_cast<VkDeviceSize>(offset),
        .mapped_span = stream_pointer.subspan(offset, size),
        .usage{},
        .log2_level{},
        .index{},
    };
}

bool StagingBufferPool::AreRegionsActive(size_t region_begin, size_t region_end) const {
    const u64 gpu_tick = scheduler.GetMasterSemaphore().KnownGpuTick();
    return std::any_of(sync_ticks.begin() + region_begin, sync_ticks.begin() + region_end,
                       [gpu_tick](u64 sync_tick) { return gpu_tick < sync_tick; });
};

StagingBufferRef StagingBufferPool::GetStagingBuffer(size_t size, MemoryUsage usage,
                                                     bool deferred) {
    if (const std::optional<StagingBufferRef> ref = TryGetReservedBuffer(size, usage, deferred)) {
        return *ref;
    }
    return CreateStagingBuffer(size, usage, deferred);
}

std::optional<StagingBufferRef> StagingBufferPool::TryGetReservedBuffer(size_t size,
                                                                        MemoryUsage usage,
                                                                        bool deferred) {
    StagingBuffers& cache_level = GetCache(usage)[Common::Log2Ceil(size)];

    const auto is_free = [this](const StagingBuffer& entry) {
        return !entry.deferred && scheduler.IsFree(entry.tick);
    };
    auto& entries = cache_level.entries;
    const auto hint_it = entries.begin() + cache_level.iterate_index;
    auto it = std::find_if(entries.begin() + cache_level.iterate_index, entries.end(), is_free);
    if (it == entries.end()) {
        it = std::find_if(entries.begin(), hint_it, is_free);
        if (it == hint_it) {
            return std::nullopt;
        }
    }
    cache_level.iterate_index = std::distance(entries.begin(), it) + 1;
    it->tick = deferred ? (std::numeric_limits<u64>::max)() : scheduler.CurrentTick();
    ASSERT(!it->deferred);
    it->deferred = deferred;
    return it->Ref();
}

StagingBufferRef StagingBufferPool::CreateStagingBuffer(size_t size, MemoryUsage usage, bool deferred) {
    auto const log2_size = Common::Log2Ceil<u32>(u32(size));
    TraceAllocation(size, size_t{1} << log2_size, usage, deferred);
    VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 1ULL << log2_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        buffer_ci.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    if (log2_size >= 20) {
        // Reclaim finished large buffers from other size buckets before adding another >= 1 MiB
        // allocation to Windows/WDDM commit.
        for (const auto trim_usage :
             {MemoryUsage::DeviceLocal, MemoryUsage::Upload, MemoryUsage::Download}) {
            auto& trim_cache = GetCache(trim_usage);
            for (size_t lvl = 20; lvl < NUM_LEVELS; ++lvl) {
                if (!trim_cache[lvl].entries.empty()) {
                    ReleaseLevel(trim_cache, lvl);
                }
            }
        }
    }
    vk::Buffer buffer = memory_allocator.CreateBuffer(buffer_ci, usage);
    if (device.HasDebuggingToolAttached()) {
        ++buffer_index;
        buffer.SetObjectNameEXT(fmt::format("Staging Buffer {}", buffer_index).c_str());
    }
    const std::span<u8> mapped_span = buffer.Mapped();
    const VkDeviceAddress buffer_address =
        device.IsBufferDeviceAddressSupported()
            ? device.GetLogical().GetBufferDeviceAddress(*buffer)
            : VkDeviceAddress{};
    StagingBuffer& entry = GetCache(usage)[log2_size].entries.emplace_back(StagingBuffer{
        .buffer = std::move(buffer),
        .device_address = buffer_address,
        .mapped_span = mapped_span,
        .usage = usage,
        .log2_level = log2_size,
        .index = unique_ids++,
        .tick = deferred ? (std::numeric_limits<u64>::max)() : scheduler.CurrentTick(),
        .deferred = deferred,
    });
    if (trace_staging) {
        trace_cached_bytes += size_t{1} << log2_size;
    }
    return entry.Ref();
}

void StagingBufferPool::TraceAllocation(size_t requested, size_t rounded, MemoryUsage usage,
                                        bool deferred) {
    // Log before allocating, even when the allocation fails or no frame ever completes.
    // Counters describe logical buffer sizes, not driver allocation/residency sizes.
    if (!trace_staging || trace_reports >= 256) {
        return;
    }
    const size_t projected = trace_cached_bytes + rounded;
    const bool growth = projected > trace_high_water && projected - trace_high_water >= 128_MiB;
    const bool large = rounded >= 64_MiB && trace_large_reports < 8;
    if (trace_reports != 0 && !growth && !large) {
        return;
    }
    ++trace_reports;
    trace_large_reports += large;
    trace_high_water = (std::max)(trace_high_water, projected);
    const u64 gpu_tick = scheduler.GetMasterSemaphore().KnownGpuTick();
    struct Totals {
        size_t bytes{};
        size_t deferred{};
        size_t inflight{};
        size_t count{};
    };
    const auto summarize = [gpu_tick](const StagingBuffersCache& cache) {
        Totals total;
        for (const auto& level : cache) {
            for (const auto& entry : level.entries) {
                const size_t bytes = size_t{1} << entry.log2_level;
                total.bytes += bytes;
                ++total.count;
                if (entry.deferred) {
                    total.deferred += bytes;
                } else if (entry.tick > gpu_tick) {
                    total.inflight += bytes;
                }
            }
        }
        return total;
    };
    const auto upload = summarize(upload_cache);
    const auto download = summarize(download_cache);
    const auto local = summarize(device_local_cache);
    LOG_WARNING(Render_Vulkan,
                "Staging allocation preflight #{}: requested={} rounded={} usage={} deferred={} "
                "delete_level={} current_tick={} gpu_tick={} stream_bytes={} "
                "upload(bytes/deferred/inflight/count)={}/{}/{}/{} "
                "download={}/{}/{}/{} device_local={}/{}/{}/{}",
                trace_reports, requested, rounded, static_cast<u32>(usage), deferred,
                current_delete_level, scheduler.CurrentTick(), gpu_tick, stream_buffer_size,
                upload.bytes, upload.deferred, upload.inflight, upload.count,
                download.bytes, download.deferred, download.inflight, download.count,
                local.bytes, local.deferred, local.inflight, local.count);
    LogMemoryStatistics(device);
}

StagingBufferPool::StagingBuffersCache& StagingBufferPool::GetCache(MemoryUsage usage) {
    switch (usage) {
    case MemoryUsage::DeviceLocal:
        return device_local_cache;
    case MemoryUsage::Upload:
        return upload_cache;
    case MemoryUsage::Download:
        return download_cache;
    default:
        ASSERT_MSG(false, "Invalid memory usage={}", usage);
        return upload_cache;
    }
}

void StagingBufferPool::ReleaseCache(MemoryUsage usage) {
    constexpr size_t LARGE_STAGING_LOG2 = 20; // >= 1 MiB
    auto& cache = GetCache(usage);
    if (current_delete_level < LARGE_STAGING_LOG2 && !cache[current_delete_level].entries.empty()) {
        ReleaseLevel(cache, current_delete_level);
    }
    for (size_t lvl = LARGE_STAGING_LOG2; lvl < NUM_LEVELS; ++lvl) {
        if (!cache[lvl].entries.empty()) {
            ReleaseLevel(cache, lvl);
        }
    }
}

void StagingBufferPool::ReleaseLevel(StagingBuffersCache& cache, size_t log2) {
    constexpr size_t LARGE_STAGING_LOG2 = 20; // >= 1 MiB
    constexpr size_t HUGE_STAGING_LOG2 = 24;  // >= 16 MiB
    constexpr size_t deletions_per_tick = 16;
    auto& staging = cache[log2];
    auto& entries = staging.entries;
    const size_t old_size = entries.size();
    if (old_size == 0) {
        return;
    }

    const bool is_large = log2 >= LARGE_STAGING_LOG2;
    const size_t begin_offset = is_large ? 0 : staging.delete_index;
    const size_t end_offset =
        is_large ? old_size : (std::min)(begin_offset + deletions_per_tick, old_size);
    bool kept_warm = log2 >= HUGE_STAGING_LOG2;
    const auto begin = entries.begin() + begin_offset;
    const auto end = entries.begin() + end_offset;
    entries.erase(std::remove_if(begin, end,
                                 [this, is_large, &kept_warm](const StagingBuffer& entry) {
                                     if (entry.deferred || !scheduler.IsFree(entry.tick)) {
                                         return false;
                                     }
                                     // Keep at most one warm 1..8 MiB buffer to avoid per-frame
                                     // reallocation churn; release >= 16 MiB buffers immediately.
                                     if (is_large && !kept_warm) {
                                         kept_warm = true;
                                         return false;
                                     }
                                     if (trace_staging) {
                                         trace_cached_bytes -= size_t{1} << entry.log2_level;
                                     }
                                     return true;
                                 }),
                  end);

    const size_t new_size = entries.size();
    if (is_large) {
        staging.delete_index = 0;
    } else {
        staging.delete_index += deletions_per_tick;
        if (staging.delete_index >= new_size) {
            staging.delete_index = 0;
        }
    }
    if (staging.iterate_index >= new_size) {
        staging.iterate_index = 0;
    }
}

} // namespace Vulkan
