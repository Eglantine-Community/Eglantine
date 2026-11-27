// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <functional>
#include <span>

#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan::Nvn {

using AllocateComputeSet = std::function<VkDescriptorSet(VkDescriptorSetLayout)>;

[[nodiscard]] inline VkDescriptorSetLayoutCreateFlags ComputeSetFlags(bool push_descriptors) {
    return push_descriptors
               ? VkDescriptorSetLayoutCreateFlags{VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR}
               : VkDescriptorSetLayoutCreateFlags{};
}

[[nodiscard]] inline bool BindComputeDescriptors(const vk::Device& logical, vk::CommandBuffer cmd,
                                                 VkPipelineLayout pipeline_layout,
                                                 VkDescriptorSetLayout set_layout,
                                                 bool push_descriptors,
                                                 std::span<VkWriteDescriptorSet> writes,
                                                 const AllocateComputeSet& allocate_set) {
    if (push_descriptors) {
        cmd.PushDescriptorSetKHR(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, writes);
        return true;
    }
    const VkDescriptorSet set = allocate_set ? allocate_set(set_layout) : VK_NULL_HANDLE;
    if (!set) {
        return false;
    }
    for (auto& write : writes) {
        write.dstSet = set;
    }
    logical.UpdateDescriptorSets(writes, {});
    cmd.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, pipeline_layout, 0, std::span{&set, 1},
                           {});
    return true;
}

} // namespace Vulkan::Nvn
