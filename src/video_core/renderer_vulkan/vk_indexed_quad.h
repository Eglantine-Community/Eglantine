// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>

#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

/// One direct guest quad draw converted to a triangle list and one indirect command.
class IndexedQuadGpu {
public:
    // Commands and chunk metadata share an SSBO. The command starts at the base of the SSBO.
    static constexpr u32 CHUNK_INDICES = 256;
    static constexpr VkDeviceSize COMMAND_SIZE = sizeof(VkDrawIndexedIndirectCommand);
    static constexpr VkDeviceSize CHUNK_SIZE = 3 * sizeof(u32);

    static constexpr u32 MaxQuads(u32 count, bool strip) {
        return strip ? (count >= 4 ? count / 2 - 1 : 0) : count / 4;
    }

    struct Constants {
        u32 index_count;
        u32 index_width;
        u32 source_bias;
        u32 restart_index;
        u32 flags;
        u32 instance_count;
        s32 vertex_offset;
        u32 first_instance;
        u32 draw_index_limit;
        u32 phase{};
    };
    static_assert(sizeof(Constants) == 10 * sizeof(u32));

    IndexedQuadGpu(const Device& device);

    [[nodiscard]] VkDescriptorSetLayout SetLayout() const {
        return *set_layout;
    }

    /// All buffers, the descriptor set and pipeline outlive the recording submission.
    /// The caller makes input data visible to compute before invoking this method.
    void Convert(vk::CommandBuffer cmd, VkDescriptorSet set,
                 const std::array<VkDescriptorBufferInfo, 3>& buffers, Constants push,
                 bool restart) const;

private:
    const Device& device;
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
};

} // namespace Vulkan
