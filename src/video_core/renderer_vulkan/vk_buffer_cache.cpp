// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <span>
#include <vector>

#include "common/scope_exit.h"
#include "video_core/buffer_cache/buffer_cache_base.h"
#include "video_core/nx2_perf.h"
#include "video_core/nx2_profile.h"
#include "video_core/renderer_vulkan/vk_buffer_cache.h"

#include "video_core/renderer_vulkan/index_restart.h"
#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/texture_cache/util.h"

namespace Vulkan {
namespace {
VkBufferCopy MakeBufferCopy(const VideoCommon::BufferCopy& copy) {
    return VkBufferCopy{
        .srcOffset = copy.src_offset,
        .dstOffset = copy.dst_offset,
        .size = copy.size,
    };
}

VkIndexType IndexTypeFromNumElements(const Device& device, u32 num_elements) {
    if (num_elements <= 0xff && device.IsExtIndexTypeUint8Supported()) {
        return VK_INDEX_TYPE_UINT8_EXT;
    }
    if (num_elements <= 0xffff) {
        return VK_INDEX_TYPE_UINT16;
    }
    return VK_INDEX_TYPE_UINT32;
}

u32 BytesPerIndex(VkIndexType index_type) {
    switch (index_type) {
    case VK_INDEX_TYPE_UINT8_EXT:
        return 1;
    case VK_INDEX_TYPE_UINT16:
        return 2;
    case VK_INDEX_TYPE_UINT32:
        return 4;
    default:
        ASSERT_MSG(false, "Invalid index type={}", index_type);
        return 1;
    }
}

vk::Buffer CreateBuffer(const Device& device, const MemoryAllocator& memory_allocator, u64 size,
                        VkDeviceSize sparse_alignment) {
    VkBufferUsageFlags flags =
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
    if (device.IsExtTransformFeedbackSupported()) {
        flags |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsExtConditionalRendering()) {
        flags |= VK_BUFFER_USAGE_CONDITIONAL_RENDERING_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        flags |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    const VkBufferCreateInfo buffer_ci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = size,
        .usage = flags,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (sparse_alignment > 1) {
        return memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal, sparse_alignment);
    }
    return memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
}
} // Anonymous namespace

Buffer::Buffer(BufferCacheRuntime& runtime, VideoCommon::NullBufferParams null_params)
    : VideoCommon::BufferBase(null_params), scheduler{&runtime.scheduler}, tracker{4096},
      pass_tracker{4096} {
    if (runtime.device.HasNullDescriptor()) {
        return;
    }
    device = &runtime.device;
    buffer = runtime.CreateNullBuffer();
    is_null = true;
    if (device->IsBufferDeviceAddressSupported()) {
        device_address = device->GetLogical().GetBufferDeviceAddress(*buffer);
    }
}

Buffer::Buffer(BufferCacheRuntime& runtime, DAddr cpu_addr_, u64 size_bytes_,
               bool sparse_compatible_)
    : VideoCommon::BufferBase(cpu_addr_, size_bytes_), device{&runtime.device},
      scheduler{&runtime.scheduler},
      buffer{CreateBuffer(*device, runtime.memory_allocator, SizeBytes(),
                          runtime.SparseAlignmentFor(sparse_compatible_))},
      tracker{SizeBytes()}, pass_tracker{SizeBytes()} {
    sparse_compatible = sparse_compatible_;
    if (runtime.device.HasDebuggingToolAttached()) {
        buffer.SetObjectNameEXT(fmt::format("Buffer {:#x}", CpuAddr()).c_str());
    }
    if (device->IsBufferDeviceAddressSupported()) {
        device_address = device->GetLogical().GetBufferDeviceAddress(*buffer);
    }
}

bool Buffer::IsRegionUsed(u64 offset, u64 size) const noexcept {
    if (scheduler->CurrentTick() != last_usage_tick) {
        return false;
    }
    return tracker.IsUsed(offset, size);
}

void Buffer::MarkUsage(u64 offset, u64 size) noexcept {
    const u64 current_tick = scheduler->CurrentTick();
    if (current_tick != last_usage_tick) {
        tracker.Reset();
        last_usage_tick = current_tick;
    }
    tracker.Track(offset, size);
    if (const u64 epoch = scheduler->PassEpoch(); epoch != pass_epoch) {
        pass_tracker.Reset();
        pass_epoch = epoch;
    }
    pass_tracker.Track(offset, size);
}

bool Buffer::IsRegionUsedInPass(u64 offset, u64 size) const noexcept {
    if (scheduler->PassEpoch() != pass_epoch) {
        return false;
    }
    return pass_tracker.IsUsed(offset, size);
}

VkBufferView Buffer::View(u32 offset, u32 size, VideoCore::Surface::PixelFormat format) {
    if (!device) {
        // Null buffer supported, return a null descriptor
        return VK_NULL_HANDLE;
    } else if (is_null) {
        // Null buffer not supported, adjust offset and size
        offset = 0;
        size = 0;
    }
    const auto it{std::ranges::find_if(views, [offset, size, format](const BufferView& view) {
        return offset == view.offset && size == view.size && format == view.format;
    })};
    if (it != views.end()) {
        return *it->handle;
    }
    views.push_back({
        .offset = offset,
        .size = size,
        .format = format,
        .handle = device->GetLogical().CreateBufferView({
            .sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .buffer = *buffer,
            .format = MaxwellToVK::SurfaceFormat(*device, FormatType::Buffer, false, format).format,
            .offset = offset,
            .range = size,
        }),
    });
    return *views.back().handle;
}

class QuadIndexBuffer {
public:
    QuadIndexBuffer(const Device& device_, MemoryAllocator& memory_allocator_,
                    Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                    VkDeviceSize max_bytes_, bool strip_)
        : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
          staging_pool{staging_pool_}, max_bytes{max_bytes_}, strip{strip_} {}

    bool UpdateBuffer(u32 num_indices_) {
        while (!retired_buffers.empty() && scheduler.IsFree(retired_buffers.front().first)) {
            retired_buffers.erase(retired_buffers.begin());
        }
        if (num_indices_ <= num_indices) {
            return true;
        }

        const u64 growth = u64{num_indices} + num_indices / 2;
        const u32 capacity = static_cast<u32>((std::min)(u64{std::numeric_limits<u32>::max()},
                                                         (std::max)(u64{num_indices_}, growth)));
        if (capacity != num_indices_ && TryGrow(capacity)) {
            return true;
        }
        return TryGrow(num_indices_);
    }

    void BindBuffer() {
        scheduler.Record([buffer_ = *buffer, type = index_type](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindIndexBuffer(buffer_, 0, type);
        });
    }

private:
    bool TryGrow(u32 capacity) {
        const auto new_type = IndexTypeFromNumElements(device, capacity);
        const u32 num_quads = strip ? (capacity - 2) / 2 : capacity / 4;
        const u64 size = u64{num_quads} * 6 * BytesPerIndex(new_type);
        if (size > max_bytes) {
            return false;
        }
        try {
            Upload(capacity, new_type, num_quads, static_cast<size_t>(size));
            return true;
        } catch (const vk::Exception& exception) {
            if (exception.GetResult() != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                exception.GetResult() != VK_ERROR_OUT_OF_HOST_MEMORY) {
                throw;
            }
            return false;
        }
    }

    template <typename T>
    void WriteIndices(u8* out, u32 num_quads) const {
        const std::array<u32, 6> order = strip ? std::array<u32, 6>{0, 1, 3, 0, 3, 2}
                                               : std::array<u32, 6>{0, 1, 2, 0, 2, 3};
        const u32 step = strip ? 2 : 4;
        for (u32 quad = 0; quad < num_quads; ++quad) {
            std::array<T, 6> indices;
            for (size_t i = 0; i < indices.size(); ++i) {
                indices[i] = static_cast<T>(quad * step + order[i]);
            }
            std::memcpy(out, indices.data(), sizeof(indices));
            out += sizeof(indices);
        }
    }

    void Upload(u32 capacity, VkIndexType new_type, u32 num_quads, size_t size_bytes) {
        const VkBufferCreateInfo buffer_ci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .size = size_bytes,
            .usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
        };
        auto next = memory_allocator.CreateBuffer(buffer_ci, MemoryUsage::DeviceLocal);
        if (device.HasDebuggingToolAttached()) {
            next.SetObjectNameEXT("Quad LUT");
        }

        const bool host_visible = next.IsHostVisible();
        StagingBufferRef staging = [&] {
            if (host_visible) {
                return StagingBufferRef{};
            }
            return staging_pool.Request(size_bytes, MemoryUsage::Upload, true);
        }();

        SCOPE_EXIT {
            if (!host_visible) {
                staging_pool.FreeDeferred(staging);
            }
        };
        u8* staging_data = host_visible ? next.Mapped().data() : staging.mapped_span.data();
        switch (new_type) {
        case VK_INDEX_TYPE_UINT8_EXT:
            WriteIndices<u8>(staging_data, num_quads);
            break;
        case VK_INDEX_TYPE_UINT16:
            WriteIndices<u16>(staging_data, num_quads);
            break;
        default:
            WriteIndices<u32>(staging_data, num_quads);
            break;
        }

        if (!host_visible) {
            staging_pool.FlushDeferredUpload(staging);
            scheduler.RequestOutsideRenderPassOperationContext();
            scheduler.Record([src_buffer = staging.buffer, src_offset = staging.offset,
                              dst_buffer = *next, size_bytes](vk::CommandBuffer cmdbuf) {
                const VkBufferCopy copy{
                    .srcOffset = src_offset,
                    .dstOffset = 0,
                    .size = size_bytes,
                };
                const VkBufferMemoryBarrier write_barrier{
                    .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                    .pNext = nullptr,
                    .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                    .dstAccessMask = VK_ACCESS_INDEX_READ_BIT,
                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                    .buffer = dst_buffer,
                    .offset = 0,
                    .size = size_bytes,
                };
                cmdbuf.CopyBuffer(src_buffer, dst_buffer, copy);
                cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                       VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, write_barrier);
            });
        } else {
            next.Flush();
        }
        // Publish the replacement only after allocation and upload have succeeded.
        if (buffer) {
            retired_buffers.emplace_back(scheduler.CurrentTick(), std::move(buffer));
        }
        buffer = std::move(next);
        index_type = new_type;
        num_indices = capacity;
    }

    const Device& device;
    MemoryAllocator& memory_allocator;
    Scheduler& scheduler;
    StagingBufferPool& staging_pool;
    const VkDeviceSize max_bytes;
    const bool strip;
    vk::Buffer buffer{};
    std::vector<std::pair<u64, vk::Buffer>> retired_buffers;
    VkIndexType index_type{};
    u32 num_indices{};
};

BufferCacheRuntime::BufferCacheRuntime(const Device& device_, MemoryAllocator& memory_allocator_,
                                       Scheduler& scheduler_, StagingBufferPool& staging_pool_,
                                       GuestDescriptorQueue& guest_descriptor_queue_,
                                       ComputePassDescriptorQueue& compute_pass_descriptor_queue,
                                       DescriptorPool& descriptor_pool)
    : device{device_}, memory_allocator{memory_allocator_}, scheduler{scheduler_},
      master_semaphore{scheduler_.GetMasterSemaphore()}, staging_pool{staging_pool_},
      host_importer{device_, scheduler_.GetMasterSemaphore().HostImportWarmup()},
      guest_descriptor_queue{guest_descriptor_queue_}, compute_descriptor_queue{compute_pass_descriptor_queue},
      compute_descriptor_pool{descriptor_pool},
      quad_index_pass(device, scheduler, descriptor_pool, staging_pool,
                      compute_pass_descriptor_queue),
      multi_range_buffers(device_) {
    const VkDeviceSize quad_limit = StagingAllocationLimit(device);
    quad_array_index_buffer = std::make_shared<QuadIndexBuffer>(
        device, memory_allocator, scheduler, staging_pool, quad_limit, false);
    quad_strip_index_buffer = std::make_shared<QuadIndexBuffer>(
        device, memory_allocator, scheduler, staging_pool, quad_limit, true);
}

u32 BufferCacheRuntime::SetVertexRepackLayout(const Regs& regs, bool dynamic_input,
                                              bool uses_vertex_input) {
    const bool had_vertex_input = std::exchange(uses_vertex_buffers, uses_vertex_input);
    vertex_repack_failed = false;
    if (!uses_vertex_input) {
        return 0;
    }

    // The output belongs to the staging pool until the current submission completes.
    u32 dirty = std::exchange(retry_vertex_bindings, 0U);
    const u64 tick = CurrentTick();
    if (std::exchange(repack_tick, tick) != tick || !had_vertex_input) {
        dirty |= repacked_bindings;
    }
    u32 reconsider = repack_dynamic_input == dynamic_input ? 0U : ~0U;
    repack_dynamic_input = dynamic_input;
    if (std::memcmp(regs.vertex_attrib_format.data(), repack_attributes.data(),
                    sizeof(repack_attributes)) != 0) {
        for (size_t i = 0; i < repack_attributes.size(); ++i) {
            const auto& attr = regs.vertex_attrib_format[i];
            auto& old = repack_attributes[i];
            if (attr.hex != old.hex) {
                reconsider |= (1U << attr.buffer.Value()) | (1U << old.buffer.Value());
                old = attr;
            }
        }
    }
    for (size_t i = 0; i < repack_stream_keys.size(); ++i) {
        const auto& stream = regs.vertex_streams[i];
        // All supported component and packed-format alignments divide 16.
        const u32 key = stream.stride.Value() | ((stream.address_low & 15U) << 12) |
                        (u32{stream.IsEnabled()} << 16);
        if (std::exchange(repack_stream_keys[i], key) != key) {
            reconsider |= 1U << i;
        }
    }
    while (reconsider != 0) {
        const u32 binding = std::countr_zero(reconsider);
        const u32 bit = 1U << binding;
        reconsider &= reconsider - 1;
        auto result = BuildVertexRepackLayout(regs, device, binding, dynamic_input);
        const bool was_unsupported = (unsupported_repack_bindings & bit) != 0;
        if (!result.supported && !was_unsupported) {
            LOG_ERROR(Render_Vulkan, "Cannot represent vertex binding {} with the enabled Vulkan features", binding);
        }
        if (result.supported) {
            unsupported_repack_bindings &= ~bit;
        } else {
            unsupported_repack_bindings |= bit;
        }
        if (was_unsupported != !result.supported || vertex_repack_layouts[binding] != result.layout) {
            dirty |= bit;
            if (result.layout) {
                repacked_bindings |= bit;
            } else {
                repacked_bindings &= ~bit;
            }
            vertex_repack_layouts[binding] = std::move(result.layout);
        }
    }
    vertex_repack_failed = unsupported_repack_bindings != 0;
    return dirty;
}

StagingBufferRef BufferCacheRuntime::UploadStagingBuffer(size_t size) {
    return staging_pool.Request(size, MemoryUsage::Upload);
}

StagingBufferRef BufferCacheRuntime::DownloadStagingBuffer(size_t size, bool deferred) {
    return staging_pool.Request(size, MemoryUsage::Download, deferred);
}

VkFormat BufferCacheRuntime::TexelBufferFormat(VideoCore::Surface::PixelFormat format) const {
    return MaxwellToVK::SurfaceFormat(device, FormatType::Buffer, false, format).format;
}

void BufferCacheRuntime::PrepareDownloadStagingBuffer(const StagingBufferRef& ref) {
    ASSERT(ref.usage == MemoryUsage::Download);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([buffer = ref.buffer](vk::CommandBuffer cmdbuf) {
        const VkBufferMemoryBarrier barrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_HOST_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = buffer,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
                               barrier);
    });
}

void BufferCacheRuntime::InvalidateDownloadStagingBuffer(const StagingBufferRef& ref) {
    ASSERT(ref.usage == MemoryUsage::Download);
    ref.Invalidate();
}

void BufferCacheRuntime::FlushDownloadStagingBuffer(const StagingBufferRef& ref) {
    ASSERT(ref.usage == MemoryUsage::Download && ref.host_readable);
    ref.readback.Flush();
}

void BufferCacheRuntime::FreeDeferredStagingBuffer(StagingBufferRef& ref) {
    staging_pool.FreeDeferred(ref);
}

u64 BufferCacheRuntime::GetDeviceLocalMemory() const {
    return device.GetDeviceLocalMemory();
}

u64 BufferCacheRuntime::GetDeviceMemoryUsage() const {
    return device.GetDeviceMemoryUsage();
}

bool BufferCacheRuntime::CanReportMemoryUsage() const {
    return device.CanReportMemoryUsage();
}

u32 BufferCacheRuntime::GetUniformBufferAlignment() const {
    return static_cast<u32>(device.GetUniformBufferAlignment());
}

u32 BufferCacheRuntime::GetStorageBufferAlignment() const {
    return static_cast<u32>(device.GetStorageBufferAlignment());
}

u64 BufferCacheRuntime::GetMaxStorageBufferRange() const {
    return device.GetMaxStorageBufferRange();
}

void BufferCacheRuntime::TickFrame(Common::SlotVector<Buffer>& slot_buffers) noexcept {
    multi_range_buffers.TickFrame(scheduler);
    for (auto it = slot_buffers.begin(); it != slot_buffers.end(); it++) {
        if (scheduler.IsFree(it->LastUsageTick())) {
            it->ResetUsageTracking();
        }
    }
}

bool BufferCacheRuntime::IsFree(u64 tick) {
    return scheduler.IsFree(tick);
}

void BufferCacheRuntime::Wait(u64 tick) {
    scheduler.Wait(tick);
}

void BufferCacheRuntime::WaitSubmittedTick(u64 tick) {
    auto& master = scheduler.GetMasterSemaphore();
    if (master.IsFree(tick)) {
        return;
    }
    // EDEN_NX2_LIST_COPY_STATS=1 (diagnostic): how a front wait for a tick splits into waiting for
    // its submission and for the GPU after it, and how much GPU work was in flight.
    static const bool stats = [] {
        const char* value = std::getenv("EDEN_NX2_LIST_COPY_STATS");
        return value != nullptr && value[0] == '1';
    }();
    using Clock = std::chrono::steady_clock;
    const Clock::time_point start = stats ? Clock::now() : Clock::time_point{};
    const bool submitted_at_start = stats && scheduler.IsSubmitted(tick);
    const u64 in_flight = stats ? master.CurrentTick() - (std::min)(master.CurrentTick(),
                                                                     master.KnownGpuTick() + 1)
                                : 0;
    const u64 ahead = stats && tick > master.KnownGpuTick() ? tick - master.KnownGpuTick() : 0;
    Clock::time_point submitted = start;
    SCOPE_EXIT {
        if (stats) {
            const Clock::time_point done = Clock::now();
            static u64 waits{}, already{}, submit_ns{}, gpu_ns{}, flight{}, ticks_ahead{};
            ++waits;
            already += submitted_at_start;
            submit_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(submitted - start).count();
            gpu_ns += std::chrono::duration_cast<std::chrono::nanoseconds>(done - submitted).count();
            flight += in_flight;
            ticks_ahead += ahead;
            if ((waits % 2000) == 0) {
                LOG_INFO(Render_Vulkan,
                         "nx2 list copy waits: {} waits, {} already submitted; mean {:.3f} ms "
                         "until submitted, {:.3f} ms GPU after; {:.2f} submissions in flight, "
                         "{:.2f} ticks to the copy's",
                         waits, already, submit_ns / 1e6 / waits, gpu_ns / 1e6 / waits,
                         static_cast<double>(flight) / waits,
                         static_cast<double>(ticks_ahead) / waits);
            }
        }
    };
    // The copy's submission may be deferred (Scheduler::DeferFlush): the split's front asks the
    // back for it rather than waiting for the back's next submission. If the back has stopped,
    // nothing else uses the scheduler and Wait submits it.
    if (!scheduler.IsSubmitted(tick) && VideoCommon::GpuSplit::Get().Active() &&
        !VideoCommon::GpuSplit::OnBackThread() && !scheduler.WaitSubmitted(tick)) {
        submitted = stats ? Clock::now() : submitted;
        scheduler.Wait(tick);
        return;
    }
    submitted = stats ? Clock::now() : submitted;
    master.Wait(tick);
}

void BufferCacheRuntime::Finish() {
    scheduler.Finish();
}

namespace {
// EDEN_NX2_UPLOAD_HOIST=0: in-order buffer uploads end the render pass, as before.
bool UploadHoistEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_UPLOAD_HOIST");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}
} // namespace

bool BufferCacheRuntime::CanReorderUpload(Buffer& buffer,
                                          std::span<const VideoCommon::BufferCopy> copies) {
    if (Settings::values.disable_buffer_reorder) {
        return false;
    }
    const bool can_use_upload_cmdbuf =
        std::ranges::all_of(copies, [&](const VideoCommon::BufferCopy& copy) {
            return !buffer.IsRegionUsed(copy.dst_offset, copy.size);
        });
    // Used earlier in the submission: the upload must come after that use. If no draw of the
    // held render pass used it yet, it can run just before that pass instead of ending it
    // (CopyBuffer). Its destination then counts as used in the pass, so a second upload of the
    // same bytes, or one after a draw read them, keeps its place.
    hoist_next_upload = false;
    if (!can_use_upload_cmdbuf && UploadHoistEnabled() && scheduler.CanHoistUpload() &&
        std::ranges::none_of(copies, [&](const VideoCommon::BufferCopy& copy) {
            return buffer.IsRegionUsedInPass(copy.dst_offset, copy.size);
        })) {
        hoist_next_upload = true;
        for (const VideoCommon::BufferCopy& copy : copies) {
            buffer.MarkUsage(copy.dst_offset, copy.size);
        }
    }
    return can_use_upload_cmdbuf;
}

void BufferCacheRuntime::CopyBuffer(VkBuffer dst_buffer, VkBuffer src_buffer,
                                    std::span<const VideoCommon::BufferCopy> copies, bool barrier,
                                    bool can_reorder_upload) {
    if (dst_buffer == VK_NULL_HANDLE || src_buffer == VK_NULL_HANDLE) {
        return;
    }
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };

    // Measuring a popular game, this number never exceeds the specified size once data is warmed up
    boost::container::small_vector<VkBufferCopy, 8> vk_copies(copies.size());
    std::ranges::transform(copies, vk_copies.begin(), MakeBufferCopy);
    if (src_buffer == staging_pool.StreamBuf() && can_reorder_upload) {
        scheduler.RecordWithUploadBuffer([src_buffer, dst_buffer, vk_copies](
                                             vk::CommandBuffer, vk::CommandBuffer upload_cmdbuf) {
            upload_cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        });
        return;
    }
    if (std::exchange(hoist_next_upload, false) && src_buffer == staging_pool.StreamBuf() &&
        scheduler.CanHoistUpload()) {
        // Before the held render pass (CanReorderUpload); the scheduler puts the barriers
        // around all of the pass's hoisted uploads.
        scheduler.RecordBeforeRenderPass(
            Scheduler::PrePassStage::Upload,
            [src_buffer, dst_buffer, vk_copies](vk::CommandBuffer cmdbuf) {
                cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
            });
        return;
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    if (barrier && !vk_copies.empty() &&
        (src_buffer == staging_pool.StreamBuf() ||
         VideoCommon::Nx2GpuSide::On(VideoCommon::Nx2GpuSide::Opt::CopyBatch))) {
        // Independent in-order copies share barriers; resource lifetime still follows the tick.
        scheduler.RecordUpload(src_buffer, dst_buffer,
                               std::span<const VkBufferCopy>{vk_copies.data(), vk_copies.size()});
        return;
    }
    scheduler.Record([src_buffer, dst_buffer, vk_copies, barrier](vk::CommandBuffer cmdbuf) {
        if (barrier) {
            cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, READ_BARRIER);
        }
        cmdbuf.CopyBuffer(src_buffer, dst_buffer, vk_copies);
        if (barrier) {
            // Later transfers too: a reordered upload of the same bytes in a later submission
            // has no barrier of its own before it.
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, 0, WRITE_BARRIER);
        }
    });
}

void BufferCacheRuntime::PreCopyBarrier() {
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER);
    });
}

void BufferCacheRuntime::PostCopyBarrier() {
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        // Copies bracketed explicitly need the same ordering as CopyBuffer(barrier = true).
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                               vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, 0, WRITE_BARRIER);
    });
}

void BufferCacheRuntime::ClearBuffer(VkBuffer dest_buffer, u32 offset, size_t size, u32 value) {
    if (dest_buffer == VK_NULL_HANDLE) {
        return;
    }
    static constexpr VkMemoryBarrier READ_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    static constexpr VkMemoryBarrier WRITE_BARRIER{
        .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    };

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([dest_buffer, offset, size, value](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               0, READ_BARRIER);
        cmdbuf.FillBuffer(dest_buffer, offset, size, value);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, vk::PIPELINE_STAGE_GRAPHICS_COMPUTE,
                               0, WRITE_BARRIER);
    });
}

bool BufferCacheRuntime::BindMultiRangeStorageBuffer(u64 key, bool is_written) {
    if (multi_range_sources.empty() || multi_range_total == 0) {
        return false;
    }
    const MultiRangeRef ref = multi_range_buffers.Get(device, scheduler, memory_allocator, key,
                                                     multi_range_sources, multi_range_total);
    if (ref.handle == VK_NULL_HANDLE) {
        return false;
    }
    if (is_written && !ref.sparse) {
        return false;
    }
    if (ref.needs_gather) {
        PreCopyBarrier();
        VkDeviceSize dst_offset = 0;
        for (const MultiRangeSource& source : multi_range_sources) {
            const std::array<VideoCommon::BufferCopy, 1> copy{VideoCommon::BufferCopy{
                .src_offset = u64(source.offset),
                .dst_offset = u64(dst_offset),
                .size = size_t(source.size),
            }};
            CopyBuffer(ref.handle, source.handle, copy, false);
            dst_offset += source.size;
        }
        PostCopyBarrier();
        multi_range_buffers.MarkGathered(key);
    }
    guest_descriptor_queue.AddBuffer(ref.handle, ref.address, 0, ref.size);
    return true;
}

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format,
                                         u32 first_index, u32 num_indices, Buffer& buffer,
                                         u32 offset, u32 size, const Regs::PrimitiveRestart& restart,
                                         bool indirect, u32 removed_prefix) {
    BindIndexBuffer(topology, index_format, first_index, num_indices, buffer.Handle(), offset,
                    size, buffer.SizeBytes(), restart, indirect);
    // Quad expansion always emits an index buffer starting at index zero.
    if (!MaxwellToVK::UsesConvertedQuadIndices(topology)) {
        removed_index_prefix += removed_prefix;
    }
}

bool BufferCacheRuntime::CanGatherIndexBuffer(u64 size) {
    if (!index_gather_limit) {
        index_gather_limit = StagingAllocationLimit(device);
    }
    return size != 0 && size <= *index_gather_limit &&
           Common::AlignUp(size, sizeof(u32)) <= *index_gather_limit;
}

void BufferCacheRuntime::BindGatheredIndexBuffer(
    PrimitiveTopology topology, IndexFormat index_format, u32 first_index, u32 num_indices,
    std::span<const IndexSource> sources, u32 size, const Regs::PrimitiveRestart& restart,
    bool indirect, u32 removed_prefix) {
    if (!CanGatherIndexBuffer(size)) {
        RejectIndexAssembly();
        return;
    }
    const VkDeviceSize padded_size = Common::AlignUp(VkDeviceSize{size}, sizeof(u32));

    boost::container::small_vector<std::pair<VkBuffer, VkBufferCopy>, 8> copies;
    VkDeviceSize copied = 0;
    for (const auto& source : sources) {
        if (!source.buffer || !source.buffer->Handle() || source.size == 0 ||
            source.size > size - copied || source.offset > source.buffer->SizeBytes() ||
            source.size > source.buffer->SizeBytes() - source.offset) {
            RejectIndexAssembly();
            return;
        }
        copies.emplace_back(source.buffer->Handle(),
                            VkBufferCopy{source.offset, copied, source.size});
        copied += source.size;
    }
    if (copied != size) {
        RejectIndexAssembly();
        return;
    }

    StagingBufferRef scratch;
    try {
        scratch = staging_pool.Request(padded_size, MemoryUsage::DeviceLocal, true);
    } catch (const vk::Exception& exception) {
        if (exception.GetResult() != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
            exception.GetResult() != VK_ERROR_OUT_OF_HOST_MEMORY) {
            throw;
        }
        RejectIndexAssembly();
        return;
    }
    SCOPE_EXIT {
        staging_pool.FreeDeferred(scratch);
    };
    PreCopyBarrier();
    scheduler.Record([scratch, size, padded_size, copies = std::move(copies)](vk::CommandBuffer cmdbuf) {
        if (size != padded_size) {
            const VkDeviceSize last_word = scratch.offset + padded_size - sizeof(u32);
            cmdbuf.FillBuffer(scratch.buffer, last_word, sizeof(u32), 0);
            const VkBufferMemoryBarrier ready{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = scratch.buffer,
                .offset = last_word,
                .size = sizeof(u32),
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   0, ready);
        }
        for (const auto& [source, copy] : copies) {
            const VkBufferCopy region{copy.srcOffset, scratch.offset + copy.dstOffset, copy.size};
            cmdbuf.CopyBuffer(source, scratch.buffer, region);
        }
    });
    PostCopyBarrier();
    const bool quads = MaxwellToVK::UsesConvertedQuadIndices(topology);
    BindIndexBuffer(topology, index_format, first_index, num_indices, scratch.buffer,
                    scratch.offset, size, scratch.offset + padded_size, restart, indirect);
    if (!quads) {
        removed_index_prefix += removed_prefix;
    }
}

void BufferCacheRuntime::SetDirectQuadDraw(std::optional<DirectQuadDraw> draw) {
    if (quad_assembly) {
        ReleaseQuadAssembly();
    }
    direct_quad_draw = draw;
}

void BufferCacheRuntime::ReleaseQuadAssembly() {
    // Keep both buffers pinned through recording of DrawIndexedIndirect, even if configuring the
    // graphics state advanced the scheduler tick between assembly and draw.
    for (auto& ref : *quad_assembly) {
        staging_pool.FreeDeferred(ref);
    }
    quad_assembly.reset();
}

void BufferCacheRuntime::BindIndexBuffer(PrimitiveTopology topology, IndexFormat index_format,
                                         u32 first_index, u32 num_indices, VkBuffer vk_buffer,
                                         VkDeviceSize vk_offset, u32 size, VkDeviceSize buffer_size,
                                         const Regs::PrimitiveRestart& restart, bool indirect) {
    VkIndexType vk_index_type = MaxwellToVK::IndexFormat(index_format);
    if (vk_offset > buffer_size || size > buffer_size - vk_offset) {
        RejectIndexAssembly();
        return;
    }
    const bool quads = MaxwellToVK::UsesConvertedQuadIndices(topology);
    if (quads && indirect) {
        // The expansion changes firstIndex and indexCount in the GPU-authored arguments.
        RejectIndexAssembly();
        return;
    }
    std::optional<u32> guest_marker;
    if (restart.enabled && !quads &&
        device.SupportsPrimitiveRestart(MaxwellToVK::PrimitiveTopology(device, topology))) {
        guest_marker = restart.index;
    }
    const auto plan = PlanIndexRestart(vk_index_type, guest_marker,
                                       device.IsExtIndexTypeUint8Supported(),
                                       device.SupportsCustomRestartIndex());
    if (quads || plan.host_type != vk_index_type) {
        std::optional<std::pair<VkBuffer, VkDeviceSize>> assembled;
        if (quads) {
            const u32 element_size = BytesPerIndex(vk_index_type);
            const VkDeviceSize first_byte = VkDeviceSize{first_index} * element_size;
            const bool strip = topology == PrimitiveTopology::QuadStrip;
            const bool restart_in_range = restart.enabled &&
                (element_size == 4 || restart.index < (1u << (element_size * 8)));
            vk_index_type = VK_INDEX_TYPE_UINT32;
            if (num_indices < 4) {
                assembled.emplace();
            } else if (first_byte <= size && restart_in_range && direct_quad_draw) {
                std::array<StagingBufferRef, 2> output{};
                try {
                    if (!quad_restart_pass) {
                        quad_restart_pass = std::make_unique<QuadRestartPass>(
                            device, scheduler, compute_descriptor_pool, staging_pool);
                    }
                    if (quad_restart_pass->Assemble(vk_buffer, vk_offset + first_byte,
                                                    size - first_byte, buffer_size, num_indices,
                                                    element_size, restart.index, strip,
                                                    *direct_quad_draw, output[0], output[1])) {
                        quad_assembly = output;
                        assembled = std::pair{output[0].buffer, output[0].offset};
                    }
                } catch (const vk::Exception& error) {
                    if (error.GetResult() != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
                        error.GetResult() != VK_ERROR_OUT_OF_HOST_MEMORY) {
                        throw;
                    }
                } catch (const std::bad_alloc&) {
                }
            } else if (first_byte <= size && !restart_in_range) {
                assembled = quad_index_pass.Assemble(index_format, num_indices, 0, vk_buffer,
                                                     vk_offset + first_byte, size - first_byte,
                                                     buffer_size, strip);
            }
        } else {
            const VkIndexType source_type = vk_index_type;
            const u32 element_size = BytesPerIndex(source_type);
            vk_index_type = plan.host_type;
            // Direct draws can rebase firstIndex. Round down to an aligned source so small
            // offsets still use the original storage buffer directly. Indirect arguments stay
            // GPU-owned, so their firstIndex is relative to the original binding.
            const VkDeviceSize prefix_bytes = VkDeviceSize{first_index} * element_size;
            if (!indirect && prefix_bytes <= size) {
                const VkDeviceSize alignment =
                    (std::max)(VkDeviceSize{4}, device.GetStorageBufferAlignment());
                const VkDeviceSize first_byte = vk_offset + prefix_bytes;
                const VkDeviceSize aligned_byte = first_byte - first_byte % alignment;
                if (aligned_byte > vk_offset) {
                    removed_index_prefix =
                        static_cast<u32>((aligned_byte - vk_offset) / element_size);
                    const u32 removed_bytes = removed_index_prefix * element_size;
                    vk_offset += removed_bytes;
                    size -= removed_bytes;
                }
            }
            const u64 required_indices = u64{DrawFirstIndex(first_index)} + num_indices;
            if (num_indices == 0) {
                assembled.emplace();
            } else if (required_indices <= (std::numeric_limits<u32>::max)()) {
                auto& pass =
                    source_type == VK_INDEX_TYPE_UINT16 ? widen_uint16_pass : widen_uint8_pass;
                if (!pass) {
                    pass = std::make_unique<IndexWidenPass>(device, scheduler, compute_descriptor_pool,
                                                           staging_pool, compute_descriptor_queue,
                                                           source_type);
                }
                assembled = pass->Assemble(static_cast<u32>(required_indices), vk_buffer, vk_offset,
                                           size, buffer_size, guest_marker.value_or(~0U));
            }
        }
        if (!assembled) {
            RejectIndexAssembly();
            return;
        }
        std::tie(vk_buffer, vk_offset) = *assembled;
    } else if (num_indices != 0 &&
               (!vk_buffer || vk_offset % BytesPerIndex(vk_index_type) != 0 ||
                (u64{first_index} + num_indices) * BytesPerIndex(vk_index_type) > size)) {
        RejectIndexAssembly();
        return;
    }
    if (vk_buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null index buffers. Replace it with our own null buffer.
        ReserveNullBuffer();
        vk_buffer = *null_buffer;
    }
    // The same index buffer is still bound in this command buffer (PR #161): skip the rebind.
    if (VideoCommon::Nx2Perf::On(VideoCommon::Nx2Perf::Opt::IndexRebind) && !VideoCommon::Nx2Perf::Verify()) {
        const u64 epoch = scheduler.RecordingEpoch();
        if (bound_index_valid && vk_buffer == bound_index_buffer &&
            vk_offset == bound_index_offset && vk_index_type == bound_index_type &&
            plan.native_marker == bound_restart_marker && epoch == bound_index_epoch) {
            return;
        }
        bound_index_valid = true;
        bound_index_buffer = vk_buffer;
        bound_index_offset = vk_offset;
        bound_index_type = vk_index_type;
        bound_restart_marker = plan.native_marker;
        bound_index_epoch = epoch;
    }
    scheduler.RecordState([vk_buffer, vk_offset, vk_index_type, marker = plan.native_marker](
                              vk::CommandBuffer cmdbuf) {
        cmdbuf.BindIndexBuffer(vk_buffer, vk_offset, vk_index_type);
        if (marker) {
            cmdbuf.SetPrimitiveRestartIndexEXT(*marker);
        }
    });
}

void BufferCacheRuntime::BindQuadIndexBuffer(PrimitiveTopology topology, u32 count) {
    bound_index_valid = false;
    if (count < 4) {
        ReserveNullBuffer();
        scheduler.RecordState([this](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindIndexBuffer(*null_buffer, 0, VK_INDEX_TYPE_UINT32);
        });
        return;
    }

    auto& indices = topology == PrimitiveTopology::Quads ? *quad_array_index_buffer
                                                         : *quad_strip_index_buffer;
    if (!indices.UpdateBuffer(count)) {
        index_assembly_failed = true;
        return;
    }
    indices.BindBuffer();
}

void BufferCacheRuntime::BindVertexBuffer(u32 index, Buffer& buffer, u32 offset, u32 size,
                                          u32 stride) {
    VideoCommon::HostBindings<Buffer> bindings;
    bindings.min_index = index;
    bindings.max_index = index + 1;
    bindings.buffers.push_back(&buffer);
    bindings.offsets.push_back(offset);
    bindings.sizes.push_back(size);
    bindings.strides.push_back(stride);
    BindVertexBuffers(bindings);
}

void BufferCacheRuntime::BindVertexBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    const u32 device_max = device.GetMaxVertexInputBindings();
    const u32 min_binding = (std::min)(bindings.min_index, device_max);
    const u32 max_binding = (std::min)(bindings.max_index, device_max);
    const u32 binding_count = max_binding - min_binding;
    if (binding_count == 0) {
        return;
    }
    boost::container::static_vector<VkBuffer, VideoCommon::NUM_VERTEX_BUFFERS> buffer_handles(binding_count);
    for (u32 i = 0; i < binding_count; ++i) {
        const auto& buffer = *bindings.buffers[i];
        VkBuffer handle = buffer.Handle();
        const auto* layout = GetVertexRepackLayout(min_binding + i);
        if (layout) {
            bindings.strides[i] = layout->BindingStride();
        } else if (bindings.strides[i] > device.GetMaxVertexInputBindingStride()) {
            // Active attributes with an oversized stride already require a new layout.
            bindings.strides[i] = 0;
        }
        if (handle == VK_NULL_HANDLE || (layout && bindings.sizes[i] == 0)) {
            bindings.offsets[i] = 0;
            bindings.sizes[i] = VK_WHOLE_SIZE;
            if (device.HasNullDescriptor()) {
                handle = VK_NULL_HANDLE;
            } else {
                ReserveNullBuffer();
                handle = *null_buffer;
            }
        } else if (layout) {
            if (!vertex_repack_pass) {
                vertex_repack_pass = std::make_unique<VertexRepackPass>(
                    device, scheduler, compute_descriptor_pool, staging_pool, compute_descriptor_queue);
            }
            const auto repacked = vertex_repack_pass->Assemble(
                handle, bindings.offsets[i], bindings.sizes[i], buffer.SizeBytes(), *layout);
            if (!repacked) {
                vertex_repack_failed = true;
                // The caller cleared all dirty flags for this batch before asking us to bind it.
                for (u32 binding = min_binding; binding < max_binding; ++binding) {
                    retry_vertex_bindings |= 1U << binding;
                }
                return;
            }
            handle = repacked->buffer;
            bindings.offsets[i] = repacked->offset;
            bindings.sizes[i] = repacked->size;
        }
        buffer_handles[i] = handle;
    }
    const bool set_stride = vertex_stride_mode == VertexStrideMode::Binding;
    static constexpr bool small_command = true;
    if (small_command && binding_count == 1) {
        const VkBuffer handle = buffer_handles.front();
        const VkDeviceSize offset = bindings.offsets.front();
        if (device.IsExtExtendedDynamicStateSupported()) {
            const VkDeviceSize size = bindings.sizes.front();
            const VkDeviceSize stride = bindings.strides.front();
            scheduler.RecordState([min_binding, handle, offset, size, stride, set_stride](
                                      vk::CommandBuffer cmdbuf) {
                cmdbuf.BindVertexBuffers2EXT(min_binding, 1, &handle, &offset, &size,
                                             set_stride ? &stride : nullptr);
            });
        } else {
            scheduler.RecordState([min_binding, handle, offset](vk::CommandBuffer cmdbuf) {
                cmdbuf.BindVertexBuffers(min_binding, 1, &handle, &offset);
            });
        }
        return;
    }
    if (device.IsExtExtendedDynamicStateSupported()) {
        scheduler.RecordState([bindings_ = std::move(bindings),
                               buffer_handles_ = std::move(buffer_handles), binding_count,
                               set_stride](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffers2EXT(bindings_.min_index, binding_count, buffer_handles_.data(),
                                         bindings_.offsets.data(), bindings_.sizes.data(),
                                         set_stride ? bindings_.strides.data() : nullptr);
        });
    } else {
        scheduler.RecordState([bindings_ = std::move(bindings), buffer_handles_ = std::move(buffer_handles), binding_count](vk::CommandBuffer cmdbuf) {
            cmdbuf.BindVertexBuffers(bindings_.min_index, binding_count, buffer_handles_.data(), bindings_.offsets.data());
        });
    }
}

void BufferCacheRuntime::BindTransformFeedbackBuffer(u32 index, VkBuffer buffer, u32 offset,
                                                     u32 size) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Already logged in the rasterizer
        return;
    }
    if (buffer == VK_NULL_HANDLE) {
        // Vulkan doesn't support null transform feedback buffers.
        // Replace it with our own null buffer.
        ReserveNullBuffer();
        buffer = *null_buffer;
        offset = 0;
        size = 0;
    }
    scheduler.RecordState([index, buffer, offset, size](vk::CommandBuffer cmdbuf) {
        const VkDeviceSize vk_offset = offset;
        const VkDeviceSize vk_size = size;
        cmdbuf.BindTransformFeedbackBuffersEXT(index, 1, &buffer, &vk_offset, &vk_size);
    });
}

void BufferCacheRuntime::BindTransformFeedbackBuffers(VideoCommon::HostBindings<Buffer>& bindings) {
    if (!device.IsExtTransformFeedbackSupported()) {
        // Already logged in the rasterizer
        return;
    }
    boost::container::static_vector<VkBuffer, VideoCommon::NUM_VERTEX_BUFFERS> buffer_handles(bindings.buffers.size());
    for (u32 i = 0; i < bindings.buffers.size(); ++i) {
        auto handle = bindings.buffers[i]->Handle();
        if (handle == VK_NULL_HANDLE) {
            ReserveNullBuffer();
            handle = *null_buffer;
            bindings.offsets[i] = 0;
            bindings.sizes[i] = 0;
        }
        buffer_handles[i] = handle;
    }
    scheduler.RecordState([bindings_ = std::move(bindings), buffer_handles_ = std::move(buffer_handles)](vk::CommandBuffer cmdbuf) {
        cmdbuf.BindTransformFeedbackBuffersEXT(0, u32(buffer_handles_.size()), buffer_handles_.data(), bindings_.offsets.data(), bindings_.sizes.data());
    });
}

void BufferCacheRuntime::ReserveNullBuffer() {
    if (!null_buffer) {
        null_buffer = CreateNullBuffer();
    }
}

vk::Buffer BufferCacheRuntime::CreateNullBuffer() {
    VkBufferCreateInfo create_info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = 4,
        .usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    if (device.IsExtTransformFeedbackSupported()) {
        create_info.usage |= VK_BUFFER_USAGE_TRANSFORM_FEEDBACK_BUFFER_BIT_EXT;
    }
    if (device.IsBufferDeviceAddressSupported()) {
        create_info.usage |= VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    }
    vk::Buffer ret = memory_allocator.CreateBuffer(create_info, MemoryUsage::DeviceLocal);
    if (device.HasDebuggingToolAttached()) {
        ret.SetObjectNameEXT("Null buffer");
    }

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([buffer = *ret](vk::CommandBuffer cmdbuf) {
        cmdbuf.FillBuffer(buffer, 0, VK_WHOLE_SIZE, 0);
    });

    return ret;
}

} // namespace Vulkan
