// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;
class Scheduler;

/// AMD FidelityFX Super Resolution 3 (FSR 3) Frame Generation pipeline.
/// Uses compute-based optical flow and bilateral frame interpolation
/// to synthesize intermediate frames without requiring engine motion vectors.
///
/// Optimizations:
///  - Quarter-resolution optical flow (16├ù fewer compute invocations)
///  - Shared-memory tile caching in both shaders
///  - Early-out for static pixels (skip diamond search)
///  - Duplicate-frame detection (skip optical flow entirely)
///  - Precise pipeline barriers instead of full-pipeline TransitionImageLayout
///  - Descriptor-write caching (only update when image views change)
class Fsr3FrameGen {
public:
    explicit Fsr3FrameGen(const Device& device, MemoryAllocator& memory_allocator,
                          VkExtent2D extent, VkFormat format);
    ~Fsr3FrameGen();

    void Process(const Device& device, Scheduler& scheduler, VkImage current_frame,
                 [[maybe_unused]] VkImageView current_view, u64 frame_count);

    void Generate(const Device& device, Scheduler& scheduler, VkImage destination_image,
                  VkImageView destination_storage_view, float blend_factor, float flow_scale);

    [[nodiscard]] bool IsReady() const {
        return m_initialized && m_has_history && m_flow_valid;
    }

private:
    static constexpr size_t kFrameSlotCount = 2;
    static constexpr size_t kInterpCacheSlots = 16;

    struct InterpDescriptorSlot {
        u32 curr_index{};
        VkImageView dest_view{VK_NULL_HANDLE};
        bool populated{};
    };

    void CreateResources(const Device& device);
    void CreateShaders(const Device& device);
    void CreatePipelines(const Device& device);
    VkDescriptorSet GetOrCreateInterpDescriptorSet(const Device& device, Scheduler& scheduler,
                                                   u32 curr_index,
                                                   VkImageView destination_storage_view);

    MemoryAllocator& m_allocator;
    VkExtent2D m_extent;
    VkExtent2D m_flow_extent;   // half-res for optical flow
    VkFormat m_format;

    vk::Sampler m_linear_sampler;
    vk::ShaderModule m_flow_shader;
    vk::ShaderModule m_interp_shader;

    vk::DescriptorSetLayout m_flow_desc_layout;
    vk::PipelineLayout m_flow_pipe_layout;
    vk::Pipeline m_flow_pipeline;

    vk::DescriptorSetLayout m_interp_desc_layout;
    vk::PipelineLayout m_interp_pipe_layout;
    vk::Pipeline m_interp_pipeline;

    vk::DescriptorPool m_desc_pool;
    vk::DescriptorSets m_flow_desc_sets;
    vk::DescriptorSets m_interp_desc_sets;

    // Ping-pong frame copies (previous vs current) and motion vector image
    std::array<vk::Image, kFrameSlotCount> m_frame_images;
    std::array<vk::ImageView, kFrameSlotCount> m_frame_views;
    std::array<VkImageLayout, kFrameSlotCount> m_frame_layouts{
        VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_UNDEFINED,
    };

    vk::Image m_motion_image;
    vk::ImageView m_motion_view;
    VkImageLayout m_motion_layout{VK_IMAGE_LAYOUT_UNDEFINED};

    u32 m_curr_index{0};
    bool m_initialized{};
    bool m_has_history{};

    // Duplicate-frame detection: skip optical flow when the frame hasn't changed
    u64 m_last_frame_count{};
    bool m_flow_valid{};

    std::array<InterpDescriptorSlot, kInterpCacheSlots> m_interp_cache{};
    size_t m_interp_next_slot{0};
};

} // namespace Vulkan
