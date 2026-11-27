// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdint>
#include <limits>

#include "video_core/host_shaders/nvn_quad_indexed_comp_spv.h"
#include "video_core/renderer_vulkan/vk_indexed_quad.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

constexpr u32 WORKGROUP_SIZE = 64;

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

IndexedQuadGpu::IndexedQuadGpu(const Device& device_) : device{device_} {
    const auto& logical = device.GetLogical();
    std::array<VkDescriptorSetLayoutBinding, 3> bindings;
    for (u32 binding = 0; binding < bindings.size(); ++binding) {
        bindings[binding] = {binding, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                             VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    }
    set_layout = logical.CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    const VkPushConstantRange push_range{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Constants)};
    layout = logical.CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    const std::span<const u32> code = NVN_QUAD_INDEXED_COMP_SPV;
    const auto module = logical.CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size_bytes(),
        .pCode = code.data(),
    });
    pipeline = logical.CreateComputePipeline({
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *module,
            .pName = "main",
        },
        .layout = *layout,
        .basePipelineIndex = -1,
    }, VK_NULL_HANDLE);
}

void IndexedQuadGpu::Convert(vk::CommandBuffer cmd, VkDescriptorSet set,
                              const std::array<VkDescriptorBufferInfo, 3>& buffers, Constants push,
                              bool restart) const {
    std::array<VkWriteDescriptorSet, 3> writes;
    for (u32 binding = 0; binding < writes.size(); ++binding) {
        writes[binding] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = set,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &buffers[binding],
        };
    }
    const auto& logical = device.GetLogical();
    const auto& dld = device.GetDispatchLoader();
    dld.vkUpdateDescriptorSets(*logical, static_cast<u32>(writes.size()), writes.data(), 0, nullptr);

    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
    cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, std::span{&set, 1}, {});
    const u32 max_groups = (std::min)(device.GetMaxComputeWorkGroupCount()[0],
                                     std::numeric_limits<u32>::max() / WORKGROUP_SIZE);
    const auto dispatch = [&](u32 phase, u32 work) {
        push.phase = phase;
        cmd.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, push);
        const u32 groups = (work - 1) / WORKGROUP_SIZE + 1;
        cmd.Dispatch((std::min)(max_groups, groups), 1, 1);
    };
    if (restart) {
        const u32 chunks = (push.index_count - 1) / CHUNK_INDICES + 1;
        const auto order_chunks = BarrierFor(buffers[2], VK_ACCESS_SHADER_WRITE_BIT,
                                             VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        dispatch(1, chunks);
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, order_chunks);
        dispatch(2, 1);
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, order_chunks);
        dispatch(3, chunks);
    } else {
        dispatch(0, MaxQuads(push.index_count, push.flags != 0));
    }

    VkDescriptorBufferInfo command = buffers[2];
    command.range = COMMAND_SIZE;
    const std::array draw_barriers{
        BarrierFor(buffers[1], VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDEX_READ_BIT),
        BarrierFor(command, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_INDIRECT_COMMAND_READ_BIT),
    };
    cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                        VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
                        0, {}, draw_barriers, {});
}

} // namespace Vulkan
