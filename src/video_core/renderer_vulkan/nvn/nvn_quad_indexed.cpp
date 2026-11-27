// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <limits>
#include <new>

#include "video_core/renderer_vulkan/nvn/nvn_draw.h"
#include "video_core/renderer_vulkan/vk_indexed_quad.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan::Nvn {
namespace {

VkBufferMemoryBarrier BarrierFor(const VkDescriptorBufferInfo& buffer, VkAccessFlags src,
                                 VkAccessFlags dst) {
    return {
        .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
        .srcAccessMask = src,
        .dstAccessMask = dst,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .buffer = buffer.buffer,
        .offset = buffer.offset,
        .size = buffer.range,
    };
}

} // namespace

IndexedQuadGpu& DrawResources::QuadIndexedPipeline() {
    if (!quad_indexed) {
        quad_indexed = std::make_unique<IndexedQuadGpu>(impl.device);
    }
    return *quad_indexed;
}

bool DrawContext::AssembleGpuQuads(const BufferRef& source, const VC::DrawIndexedPayload& draw,
                                   u32 instances, BufferRef& output_indices,
                                   BufferRef& output_command) {
    output_indices = {};
    output_command = {};
    if (pass_open || !source.buffer || draw.index_type > 2 ||
        (draw.mode != 7 && draw.mode != 8) || draw.count < 4) {
        return false;
    }

    constexpr u32 max_u32 = std::numeric_limits<u32>::max();
    const bool strip = draw.mode == 8;
    const u32 width = 1u << draw.index_type;
    const u64 input_bytes = u64{draw.count} * width;
    const u64 max_indices = u64{IndexedQuadGpu::MaxQuads(draw.count, strip)} * 6;
    const u64 output_bytes = max_indices * sizeof(u32);
    const u64 max_range = impl.device.GetMaxStorageBufferRange();
    const u64 alignment = (std::max<u64>)(impl.device.GetStorageBufferAlignment(), sizeof(u32));
    const u64 source_base = source.offset - source.offset % alignment;
    const u64 source_bias = source.offset - source_base;
    const u64 input_padded = (input_bytes + 3) & ~u64{3};
    if (input_bytes > source.range || input_bytes > max_u32 || input_padded > max_range ||
        output_bytes > max_range || max_indices > max_u32 ||
        source.offset > std::numeric_limits<u64>::max() - input_bytes) {
        return false;
    }
    const u64 direct_range = (source_bias + input_bytes + 3) & ~u64{3};
    const bool needs_padding = direct_range > max_range || source_bias + input_bytes > max_u32 ||
                               direct_range - source_bias > source.range;
    const bool restart = fixed.primitive_restart &&
                         (width == 4 || fixed.restart_index < (1u << (width * 8u)));
    const u64 chunks = restart ? (draw.count - 1) / IndexedQuadGpu::CHUNK_INDICES + 1 : 0;
    const u64 scratch_bytes = IndexedQuadGpu::COMMAND_SIZE + chunks * IndexedQuadGpu::CHUNK_SIZE;
    if (scratch_bytes > max_range) {
        return false;
    }

    IndexedQuadGpu* pipeline;
    BufferRef input = source;
    if (!needs_padding) {
        input.offset = source_base;
        input.range = direct_range;
    }
    BufferRef indices;
    BufferRef scratch;
    VkDescriptorSet set{};
    try {
        pipeline = &resources.QuadIndexedPipeline();
        if (needs_padding) {
            input = AllocArena(input_padded, alignment, nullptr);
        }
        indices = AllocArena(output_bytes, alignment, nullptr);
        scratch = AllocArena(scratch_bytes, alignment, nullptr);
        set = AllocateSet(pipeline->SetLayout(), true);
    } catch (const vk::Exception& error) {
        if (error.GetResult() != VK_ERROR_OUT_OF_DEVICE_MEMORY &&
            error.GetResult() != VK_ERROR_OUT_OF_HOST_MEMORY) {
            throw;
        }
        return false;
    } catch (const std::bad_alloc&) {
        return false;
    }
    if (!input.buffer || !indices.buffer || !scratch.buffer || !set) {
        return false;
    }

    host->HostFlushBarrier();
    const vk::CommandBuffer cmd = host->HostCmd();
    // The recorder consumes pending general effects. Also order the exact source range here:
    // it may contain GPU-written bytes from an earlier operation or submission.
    const VkDescriptorBufferInfo source_read = needs_padding
        ? VkDescriptorBufferInfo{source.buffer, source.offset, input_bytes}
        : VkDescriptorBufferInfo{input.buffer, input.offset, input.range};
    const auto source_ready = BarrierFor(source_read, VK_ACCESS_MEMORY_WRITE_BIT,
        needs_padding ? VK_ACCESS_TRANSFER_READ_BIT : VK_ACCESS_SHADER_READ_BIT);
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                        needs_padding ? VK_PIPELINE_STAGE_TRANSFER_BIT
                                      : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        0, source_ready);
    if (needs_padding) {
        if (input_bytes != input_padded) {
            const VkDescriptorBufferInfo tail{
                input.buffer, input.offset + input_padded - sizeof(u32), sizeof(u32),
            };
            cmd.FillBuffer(tail.buffer, tail.offset, tail.range, 0);
            const auto fill_ready =
                BarrierFor(tail, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
            cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                0, fill_ready);
        }
        const VkBufferCopy copy{source.offset, input.offset, input_bytes};
        cmd.CopyBuffer(source.buffer, input.buffer, copy);
        const auto input_ready = BarrierFor({input.buffer, input.offset, input.range},
                                            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            0, input_ready);
    }
    const std::array<VkDescriptorBufferInfo, 3> buffers{{
        {input.buffer, input.offset, input.range},
        {indices.buffer, indices.offset, indices.range},
        {scratch.buffer, scratch.offset, scratch.range},
    }};
    pipeline->Convert(cmd, set, buffers, {
        .index_count = draw.count,
        .index_width = width,
        .source_bias = needs_padding ? 0 : static_cast<u32>(source_bias),
        .restart_index = fixed.restart_index,
        .flags = strip ? 1u : 0u,
        .instance_count = instances,
        .vertex_offset = draw.base_vertex,
        .first_instance = draw.base_instance,
        .draw_index_limit = CostSplitCount(max_u32),
    }, restart);
    host->HostMarkWritten();
    output_indices = indices;
    output_command = scratch;
    output_command.range = IndexedQuadGpu::COMMAND_SIZE;
    return true;
}

} // namespace Vulkan::Nvn
