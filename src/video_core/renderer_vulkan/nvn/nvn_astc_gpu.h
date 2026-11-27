// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <span>
#include <vector>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/nvn/nvn_compute_descriptors.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
class Device;
}

namespace Vulkan::Nvn {

/// Decodes block-linear ASTC texture bytes straight from a storage buffer into the levels of an
/// RGBA8 image, on the GPU (the legacy path's `astc_decoder.comp`, the same shader the default
/// `accelerate_astc = Gpu` runs). The replay thread only records dispatches: nothing is decoded,
/// unswizzled or copied on the CPU, and the input is the guest's own bytes (an imported pool).
class AstcGpuDecoder {
public:
    /// One mip level of a storage (all its layers are one dispatch).
    struct Level {
        u32 level{};
        u32 layers{1};
        u32 width{};        ///< texels to decode, independent of the image view dimensions
        u32 height{};
        u64 offset{};       ///< bytes from the start of the input range to this level of layer 0
        u32 layer_stride{}; ///< bytes between layers
        u32 block_width{};  ///< texels per block
        u32 block_height{};
        u32 tile_height{};  ///< log2 of the GOBs per tile row (the level's block height)
        u32 tile_depth{};
        u32 source_columns{}; ///< padded block columns for a clipped region, or 0 for width
    };

    /// True when the device can run the decoder (storable RGBA8) and
    /// `EDEN_NX2_NVN_NO_ASTC_GPU` is not set.
    [[nodiscard]] static bool Supported(const Device& device);

    explicit AstcGpuDecoder(const Device& device);
    ~AstcGpuDecoder();

    /// Alignment of the input range offsets (minStorageBufferOffsetAlignment).
    [[nodiscard]] u64 OffsetAlignment() const {
        return offset_alignment;
    }

    /// Format of the storage image views the decoder writes through.
    // Matches the Rgba8 storage image declared by the compute shaders.
    static constexpr VkFormat VIEW_FORMAT = VK_FORMAT_R8G8B8A8_UNORM;
    [[nodiscard]] static u64 RequiredBytes(const Level& level);

    /// Records the dispatches into `cmd` (outside a render pass). `image` must be in GENERAL
    /// layout with STORAGE usage (extended usage for sRGB images). `input` is bound from
    /// `input_offset + level.offset`, `input_size` bytes in all are readable from `input_offset`.
    /// Appends the image views, which must outlive the command buffer even on partial failure.
    /// When push descriptors are unavailable, allocate_set supplies submission-owned sets.
    [[nodiscard]] bool Record(vk::CommandBuffer cmd, VkBuffer input, u64 input_offset, u64 input_size,
                              VkImage image, std::span<const Level> levels,
                              std::vector<vk::ImageView>& views,
                              const AllocateComputeSet& allocate_set = {}) const;

    /// Records a single level dispatch into `cmd` (outside a render pass) using an existing
    /// GENERAL storage `view` (2D array, `VIEW_FORMAT`), avoiding per-upload image view creation.
    [[nodiscard]] bool RecordWithView(vk::CommandBuffer cmd, VkBuffer input, u64 input_offset,
                                      u64 input_size, VkImageView view, const Level& level,
                                      const AllocateComputeSet& allocate_set) const;

private:
    bool DispatchLevel(vk::CommandBuffer cmd, VkBuffer input, u64 input_offset, u64 input_size,
                       VkImageView view, const Level& level,
                       const AllocateComputeSet& allocate_set) const;
    const Device& device;
    bool push_descriptors{};
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout pipeline_layout;
    vk::ShaderModule module;
    vk::Pipeline pipeline;
    u64 offset_alignment{16};
    u64 max_range{};
};

/// Fuses decoding and BC3 encoding for contiguous full NVN sparse ASTC 8x8 tiles.
class AstcBc3 {
public:
    static constexpr u32 TILE_WIDTH = 512;
    static constexpr u32 TILE_HEIGHT = 512;
    static constexpr u64 INPUT_TILE_BYTES = 0x10000;
    static constexpr u64 OUTPUT_TILE_BYTES = 0x40000;
    static constexpr u32 MAX_TILES = 32;

    [[nodiscard]] static bool Supported(const Device& device);
    explicit AstcBc3(const Device& device);

    [[nodiscard]] u64 OffsetAlignment() const {
        return offset_alignment;
    }

    /// Records outside a render pass. Input uses NVN block-linear GOBs. Output is tile-major,
    /// with tightly packed BC3 blocks in each tile. The two buffers need STORAGE usage and
    /// disjoint backing ranges large enough for the batch. The caller keeps them alive and
    /// synchronizes their use and reuse.
    [[nodiscard]] bool Record(vk::CommandBuffer cmd, VkBuffer input, u64 input_offset,
                              VkBuffer output, u64 output_offset, u32 tile_count,
                              const AllocateComputeSet& allocate_set = {}) const;

private:
    const Device& device;
    bool push_descriptors{};
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout pipeline_layout;
    vk::Pipeline pipeline;
    u64 offset_alignment{};
    u64 max_range{};
};

} // namespace Vulkan::Nvn
