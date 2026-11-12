// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <vector>

#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

#include "video_core/host_shaders/fsr3_frame_interpolation_comp_spv.h"
#include "video_core/host_shaders/fsr3_optical_flow_comp_spv.h"

namespace Vulkan {

namespace {

VkImageMemoryBarrier MakeBarrier(VkImage image, VkAccessFlags src_access,
                                 VkAccessFlags dst_access, VkImageLayout old_layout,
                                 VkImageLayout new_layout) {
    return VkImageMemoryBarrier{
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = src_access,
        .dstAccessMask = dst_access,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1},
    };
}

void BarrierComputeWriteToRead(vk::CommandBuffer cmdbuf, VkImage image) {
    const auto barrier = MakeBarrier(image, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                                     VK_IMAGE_LAYOUT_GENERAL,
                                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
}

void CopyFrameToSlot(vk::CommandBuffer cmdbuf, VkImage source, VkImage destination,
                     VkImageLayout old_dst_layout, VkExtent2D extent) {
    const VkImageCopy region{
        .srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .srcOffset = {0, 0, 0},
        .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1},
        .dstOffset = {0, 0, 0},
        .extent = {extent.width, extent.height, 1},
    };

    const VkPipelineStageFlags dst_src_stage = old_dst_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                                   ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                                   : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkAccessFlags dst_src_access =
        old_dst_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;

    const std::array before{
        MakeBarrier(source,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT |
                        VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_TRANSFER_READ_BIT, VK_IMAGE_LAYOUT_GENERAL,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL),
        MakeBarrier(destination, dst_src_access, VK_ACCESS_TRANSFER_WRITE_BIT, old_dst_layout,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_TRANSFER_BIT | dst_src_stage,
                           VK_PIPELINE_STAGE_TRANSFER_BIT, 0, {}, {}, before);

    cmdbuf.CopyImage(source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destination,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, region);

    const std::array after{
        MakeBarrier(source, VK_ACCESS_TRANSFER_READ_BIT,
                    VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT |
                        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL),
        MakeBarrier(destination, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
    };
    cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                               VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                               VK_PIPELINE_STAGE_TRANSFER_BIT,
                           0, {}, {}, after);
}

void TransitionMotionForWrite(vk::CommandBuffer cmdbuf, VkImage motion_image,
                              VkImageLayout old_layout) {
    const VkPipelineStageFlags src_stage = old_layout == VK_IMAGE_LAYOUT_UNDEFINED
                                               ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT
                                               : VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkAccessFlags src_access =
        old_layout == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : VK_ACCESS_SHADER_READ_BIT;
    const auto barrier = MakeBarrier(motion_image, src_access, VK_ACCESS_SHADER_WRITE_BIT,
                                     old_layout, VK_IMAGE_LAYOUT_GENERAL);
    cmdbuf.PipelineBarrier(src_stage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, barrier);
}

VkDescriptorImageInfo MakeImageInfo(VkSampler sampler, VkImageView view, VkImageLayout layout) {
    return VkDescriptorImageInfo{
        .sampler = sampler,
        .imageView = view,
        .imageLayout = layout,
    };
}

VkWriteDescriptorSet MakeWriteSet(VkDescriptorSet set, u32 binding, VkDescriptorType type,
                                  const VkDescriptorImageInfo* info) {
    return VkWriteDescriptorSet{
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = set,
        .dstBinding = binding,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = type,
        .pImageInfo = info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
}

} // anonymous namespace

struct FlowPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float search_radius;
    float block_size;
};

struct InterpPushConstants {
    float inv_render_w;
    float inv_render_h;
    float render_w;
    float render_h;
    float blend_factor;
    float flow_scale;
};

Fsr3FrameGen::Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                           VkExtent2D extent, VkFormat format)
    : m_allocator{memory_allocator}, m_extent{extent},
      m_flow_extent{std::max(1u, extent.width / 2), std::max(1u, extent.height / 2)},
      m_format{format} {
    CreateResources(device);
    CreateShaders(device);
    CreatePipelines(device);
    m_initialized = true;
}

Fsr3FrameGen::~Fsr3FrameGen() = default;

void Fsr3FrameGen::CreateResources(const Device& device) {
    m_linear_sampler = CreateBilinearSampler(device);

    for (size_t i = 0; i < kFrameSlotCount; ++i) {
        m_frame_images[i] = CreateWrappedImage(m_allocator, m_extent, m_format);
        m_frame_views[i] = CreateWrappedImageView(device, m_frame_images[i], m_format);
        m_frame_layouts[i] = VK_IMAGE_LAYOUT_UNDEFINED;
    }

    // Half-resolution motion vector image
    m_motion_image = CreateWrappedImage(m_allocator, m_flow_extent, VK_FORMAT_R16G16_SFLOAT);
    m_motion_view = CreateWrappedImageView(device, m_motion_image, VK_FORMAT_R16G16_SFLOAT);
    m_motion_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    // 2 pre-populated flow sets + kInterpCacheSlots cached interpolation sets
    const std::array pool_sizes{
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                             4 + static_cast<u32>(kInterpCacheSlots) * 3},
        VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                             2 + static_cast<u32>(kInterpCacheSlots)},
    };
    m_desc_pool = device.GetLogical().CreateDescriptorPool(VkDescriptorPoolCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT,
        .maxSets = static_cast<u32>(kFrameSlotCount + kInterpCacheSlots),
        .poolSizeCount = static_cast<u32>(pool_sizes.size()),
        .pPoolSizes = pool_sizes.data(),
    });
}

void Fsr3FrameGen::CreateShaders(const Device& device) {
    m_flow_shader = BuildShader(device, FSR3_OPTICAL_FLOW_COMP_SPV);
    m_interp_shader = BuildShader(device, FSR3_FRAME_INTERPOLATION_COMP_SPV);
}

void Fsr3FrameGen::CreatePipelines(const Device& device) {
    const std::array flow_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    m_flow_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(flow_bindings.size()),
        .pBindings = flow_bindings.data(),
    });

    const VkPushConstantRange flow_push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(FlowPushConstants),
    };

    m_flow_pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = m_flow_desc_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &flow_push_range,
    });

    m_flow_pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *m_flow_shader,
            .pName = "main",
        },
        .layout = *m_flow_pipe_layout,
    });

    const std::array interp_bindings{
        VkDescriptorSetLayoutBinding{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
        VkDescriptorSetLayoutBinding{3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                                     VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
    };

    m_interp_desc_layout = device.GetLogical().CreateDescriptorSetLayout(VkDescriptorSetLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = static_cast<u32>(interp_bindings.size()),
        .pBindings = interp_bindings.data(),
    });

    const VkPushConstantRange interp_push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(InterpPushConstants),
    };

    m_interp_pipe_layout = device.GetLogical().CreatePipelineLayout(VkPipelineLayoutCreateInfo{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = m_interp_desc_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &interp_push_range,
    });

    m_interp_pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *m_interp_shader,
            .pName = "main",
        },
        .layout = *m_interp_pipe_layout,
    });

    // Pre-populate the 2 ping-pong optical flow descriptor sets so Process() never mutates active sets.
    const std::array<VkDescriptorSetLayout, kFrameSlotCount> flow_layouts{
        *m_flow_desc_layout,
        *m_flow_desc_layout,
    };
    m_flow_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, flow_layouts);

    for (u32 curr_idx = 0; curr_idx < kFrameSlotCount; ++curr_idx) {
        const u32 prev_idx = curr_idx ^ 1U;
        const std::array<VkDescriptorImageInfo, 3> infos{
            MakeImageInfo(*m_linear_sampler, *m_frame_views[curr_idx],
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            MakeImageInfo(*m_linear_sampler, *m_frame_views[prev_idx],
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
            MakeImageInfo(VK_NULL_HANDLE, *m_motion_view, VK_IMAGE_LAYOUT_GENERAL),
        };
        const std::array<VkWriteDescriptorSet, 3> writes{
            MakeWriteSet(m_flow_desc_sets[curr_idx], 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         &infos[0]),
            MakeWriteSet(m_flow_desc_sets[curr_idx], 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                         &infos[1]),
            MakeWriteSet(m_flow_desc_sets[curr_idx], 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
                         &infos[2]),
        };
        device.GetLogical().UpdateDescriptorSets(writes, {});
    }

    std::vector<VkDescriptorSetLayout> interp_layouts(kInterpCacheSlots, *m_interp_desc_layout);
    m_interp_desc_sets = CreateWrappedDescriptorSets(m_desc_pool, interp_layouts);
}

VkDescriptorSet Fsr3FrameGen::GetOrCreateInterpDescriptorSet(const Device& device, Scheduler& scheduler,
                                                             u32 curr_index,
                                                             VkImageView destination_storage_view) {
    for (size_t i = 0; i < kInterpCacheSlots; ++i) {
        const auto& entry = m_interp_cache[i];
        if (entry.populated && entry.curr_index == curr_index &&
            entry.dest_view == destination_storage_view) {
            return m_interp_desc_sets[i];
        }
    }

    const size_t slot = m_interp_next_slot;
    m_interp_next_slot = (m_interp_next_slot + 1) % kInterpCacheSlots;
    if (m_interp_cache[slot].populated) {
        scheduler.Finish();
    }

    const u32 prev_index = curr_index ^ 1U;
    const VkDescriptorSet set = m_interp_desc_sets[slot];
    const std::array<VkDescriptorImageInfo, 4> infos{
        MakeImageInfo(*m_linear_sampler, *m_frame_views[curr_index],
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        MakeImageInfo(*m_linear_sampler, *m_frame_views[prev_index],
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        MakeImageInfo(*m_linear_sampler, *m_motion_view,
                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL),
        MakeImageInfo(VK_NULL_HANDLE, destination_storage_view, VK_IMAGE_LAYOUT_GENERAL),
    };
    const std::array<VkWriteDescriptorSet, 4> writes{
        MakeWriteSet(set, 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &infos[0]),
        MakeWriteSet(set, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &infos[1]),
        MakeWriteSet(set, 2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &infos[2]),
        MakeWriteSet(set, 3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &infos[3]),
    };
    device.GetLogical().UpdateDescriptorSets(writes, {});
    m_interp_cache[slot] = InterpDescriptorSlot{
        .curr_index = curr_index,
        .dest_view = destination_storage_view,
        .populated = true,
    };
    return set;
}

void Fsr3FrameGen::Process([[maybe_unused]] const Device& device, Scheduler& scheduler,
                           VkImage current_frame, [[maybe_unused]] VkImageView current_view,
                           u64 frame_count) {
    if (!m_initialized) {
        return;
    }

    if (m_has_history && frame_count == m_last_frame_count && m_flow_valid) {
        return;
    }
    m_last_frame_count = frame_count;

    // Advance ping-pong index so the previous slot keeps frame N-1 intact until Generate() finishes.
    m_curr_index ^= 1U;
    const u32 curr_idx = m_curr_index;
    const VkImage dst_image = *m_frame_images[curr_idx];
    const VkImageLayout old_dst_layout = m_frame_layouts[curr_idx];
    m_frame_layouts[curr_idx] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (!m_has_history) {
        scheduler.Record(
            [current_frame, dst_image, old_dst_layout, extent = m_extent](vk::CommandBuffer cmdbuf) {
                CopyFrameToSlot(cmdbuf, current_frame, dst_image, old_dst_layout, extent);
            });
        m_has_history = true;
        m_flow_valid = false;
        return;
    }

    const u32 flow_w = m_flow_extent.width;
    const u32 flow_h = m_flow_extent.height;
    const FlowPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(flow_w),
        .inv_render_h = 1.0f / static_cast<float>(flow_h),
        .render_w = static_cast<float>(flow_w),
        .render_h = static_cast<float>(flow_h),
        .search_radius = 2.0f,
        .block_size = 2.0f,
    };

    const VkImageLayout old_motion_layout = m_motion_layout;
    m_motion_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    const VkImage motion_image = *m_motion_image;
    const VkPipeline flow_pipeline = *m_flow_pipeline;
    const VkPipelineLayout flow_pipe_layout = *m_flow_pipe_layout;
    const VkDescriptorSet flow_set = m_flow_desc_sets[curr_idx];

    scheduler.Record([constants, current_frame, dst_image, old_dst_layout, extent = m_extent,
                      motion_image, old_motion_layout, flow_pipeline, flow_pipe_layout, flow_set,
                      flow_w, flow_h](vk::CommandBuffer cmdbuf) {
        CopyFrameToSlot(cmdbuf, current_frame, dst_image, old_dst_layout, extent);
        TransitionMotionForWrite(cmdbuf, motion_image, old_motion_layout);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, flow_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, flow_pipe_layout, 0, flow_set,
                                  {});
        cmdbuf.PushConstants(flow_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((flow_w + 15) / 16, (flow_h + 15) / 16, 1);

        BarrierComputeWriteToRead(cmdbuf, motion_image);
    });

    m_flow_valid = true;
}

void Fsr3FrameGen::Generate(const Device& device, Scheduler& scheduler, VkImage destination_image,
                            VkImageView destination_storage_view, float blend_factor,
                            float flow_scale) {
    if (!m_initialized || !m_has_history || !m_flow_valid ||
        destination_image == VK_NULL_HANDLE || destination_storage_view == VK_NULL_HANDLE) {
        return;
    }

    const VkDescriptorSet interp_set = GetOrCreateInterpDescriptorSet(
        device, scheduler, m_curr_index, destination_storage_view);

    const u32 out_w = m_extent.width;
    const u32 out_h = m_extent.height;
    const float flow_to_full =
        static_cast<float>(out_w) / static_cast<float>(std::max(1u, m_flow_extent.width));
    const InterpPushConstants constants{
        .inv_render_w = 1.0f / static_cast<float>(out_w),
        .inv_render_h = 1.0f / static_cast<float>(out_h),
        .render_w = static_cast<float>(out_w),
        .render_h = static_cast<float>(out_h),
        .blend_factor = blend_factor,
        .flow_scale = flow_scale * flow_to_full,
    };

    const VkPipeline interp_pipeline = *m_interp_pipeline;
    const VkPipelineLayout interp_pipe_layout = *m_interp_pipe_layout;

    scheduler.Record([constants, destination_image, interp_pipeline, interp_pipe_layout, interp_set,
                      out_w, out_h](vk::CommandBuffer cmdbuf) {
        const auto pre_barrier = MakeBarrier(destination_image, 0, VK_ACCESS_SHADER_WRITE_BIT,
                                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, {}, {}, pre_barrier);

        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, interp_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, interp_pipe_layout, 0,
                                  interp_set, {});
        cmdbuf.PushConstants(interp_pipe_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
        cmdbuf.Dispatch((out_w + 15) / 16, (out_h + 15) / 16, 1);

        const auto post_barrier =
            MakeBarrier(destination_image, VK_ACCESS_SHADER_WRITE_BIT,
                        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
                        VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT |
                                   VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                               0, {}, {}, post_barrier);
    });
}

} // namespace Vulkan
