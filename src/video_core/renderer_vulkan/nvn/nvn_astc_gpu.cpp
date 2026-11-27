// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <limits>

#include "common/common_types.h"
#include "common/div_ceil.h"
#include "video_core/host_shaders/astc_bc_comp_spv.h"
#include "video_core/host_shaders/astc_decoder_comp_spv.h"
#include "video_core/renderer_vulkan/nvn/nvn_astc_gpu.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan::Nvn {
namespace {

/// The push constant block of astc_decoder.comp.
struct PushConstants {
    std::array<u32, 2> block_dims;
    u32 layer_stride;
    u32 block_size;
    u32 x_shift;
    u32 block_height;
    u32 block_height_mask;
    u32 native_tiling;
    std::array<u32, 2> decode_extent;
};
static_assert(sizeof(PushConstants) == 40);

constexpr u32 BINDING_INPUT = 0;
constexpr u32 BINDING_OUTPUT = 1;

/// ASTC blocks are 16 bytes; the stride of a row of blocks decides how many GOBs a tile row has.
constexpr u32 ASTC_BLOCK_BYTES = 16;

u32 GobColumns(const AstcGpuDecoder::Level& level) {
    const u32 blocks = level.source_columns != 0 ? level.source_columns
                                                  : Common::DivCeil(level.width, level.block_width);
    const u32 stride = blocks * ASTC_BLOCK_BYTES;
    return Common::DivCeil(stride, Tegra::Texture::GOB_SIZE_X);
}

/// The push constant block of upstream astc_bc.comp (Eglantine and ScarletNX).
struct AstcBcPushConstants {
    u32 width;
    u32 height;
    u32 blocks_x;
    u32 blocks_y;
    u32 srgb;
    u32 block_w;
    u32 block_h;
    u32 recip_w;
    u32 recip_h;
    u32 infill_ds;
    u32 infill_dt;
    u32 layer_stride;
    u32 block_size;
    u32 x_shift;
    u32 block_height;
    u32 block_height_mask;
    u32 native_tiling;
    u32 out_offset;
};
static_assert(sizeof(AstcBcPushConstants) == 72);

constexpr u32 FUSED_ASTC_BLOCK = 8;
constexpr u32 FUSED_GOB_HEIGHT = 3;
constexpr u32 FUSED_WORKGROUP_WIDTH = 64;
constexpr u32 FUSED_BLOCKS_X = AstcBc3::TILE_WIDTH / FUSED_ASTC_BLOCK;
constexpr u32 FUSED_BLOCKS_Y = AstcBc3::TILE_HEIGHT / FUSED_ASTC_BLOCK;
constexpr u32 FUSED_X_SHIFT = Tegra::Texture::GOB_SIZE_SHIFT + FUSED_GOB_HEIGHT;
constexpr u32 FUSED_BLOCK_SIZE =
    (FUSED_BLOCKS_X * ASTC_BLOCK_BYTES / Tegra::Texture::GOB_SIZE_X) << FUSED_X_SHIFT;
constexpr u32 FUSED_INFIL_STEP = (1024 + FUSED_ASTC_BLOCK / 2) / (FUSED_ASTC_BLOCK - 1);
static_assert(FUSED_BLOCKS_Y == (Tegra::Texture::GOB_SIZE_Y << FUSED_GOB_HEIGHT));
static_assert(FUSED_BLOCK_SIZE == AstcBc3::INPUT_TILE_BYTES);
static_assert(AstcBc3::OUTPUT_TILE_BYTES == AstcBc3::TILE_WIDTH * AstcBc3::TILE_HEIGHT);

} // Anonymous namespace

bool AstcGpuDecoder::Supported(const Device& device) {
    static const bool disabled = [] {
        const char* value = std::getenv("EDEN_NX2_NVN_NO_ASTC_GPU");
        return value != nullptr && *value != '\0' && std::strcmp(value, "0") != 0;
    }();
    if (disabled) {
        return false;
    }
    return device.IsFormatSupported(VIEW_FORMAT, VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT,
                                    FormatType::Optimal);
}

AstcGpuDecoder::AstcGpuDecoder(const Device& device_) : device{device_} {
    push_descriptors = device.IsKhrPushDescriptorSupported() && device.MaxPushDescriptors() >= 2;
    offset_alignment = (std::max)(device.GetStorageBufferAlignment(), u64{16});
    max_range = device.GetMaxStorageBufferRange();
    const vk::Device& logical = device.GetLogical();
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings{{
        {
            .binding = BINDING_INPUT,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
        {
            .binding = BINDING_OUTPUT,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
            .pImmutableSamplers = nullptr,
        },
    }};
    set_layout = logical.CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = ComputeSetFlags(push_descriptors),
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    const VkPushConstantRange push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .offset = 0,
        .size = sizeof(PushConstants),
    };
    pipeline_layout = logical.CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    const std::span<const u32> code = ASTC_DECODER_COMP_SPV;
    module = logical.CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(code.size_bytes()),
        .pCode = code.data(),
    });
    pipeline = logical.CreateComputePipeline(
        VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *module,
                .pName = "main",
                .pSpecializationInfo = nullptr,
            },
            .layout = *pipeline_layout,
            .basePipelineHandle = {},
            .basePipelineIndex = 0,
        },
        // The decoder is compiled on NvnAstcInit while the replay and present threads create their
        // own pipelines. VkPipelineCache is externally synchronized, so the shared static cache
        // cannot be used here without serializing all of them. This one small pipeline does not
        // justify that lock (or its contention with the game's pipeline preloading).
        VK_NULL_HANDLE);
}

AstcGpuDecoder::~AstcGpuDecoder() = default;

u64 AstcGpuDecoder::RequiredBytes(const Level& level) {
    const u32 rows = Common::DivCeil(level.height, level.block_height);
    const u32 tile_rows = Common::DivCeil(rows, Tegra::Texture::GOB_SIZE_Y << level.tile_height);
    const u32 x_shift = Tegra::Texture::GOB_SIZE_SHIFT + level.tile_height + level.tile_depth;
    return u64{level.layers - 1} * level.layer_stride + (u64{GobColumns(level)} << x_shift) *
                                                           tile_rows;
}

bool AstcGpuDecoder::DispatchLevel(vk::CommandBuffer cmd, VkBuffer input, u64 input_offset,
                                   u64 input_size, VkImageView view, const Level& level,
                                   const AllocateComputeSet& allocate_set) const {
    const u64 binding_offset = input_offset + level.offset;
    const u64 bytes = RequiredBytes(level);
    if (level.offset > input_size || bytes > input_size - level.offset || bytes > max_range ||
        binding_offset % offset_alignment != 0) {
        return false;
    }
    const VkDescriptorBufferInfo buffer_info{
        .buffer = input,
        .offset = binding_offset,
        .range = bytes,
    };
    const VkDescriptorImageInfo image_info{
        .sampler = VK_NULL_HANDLE,
        .imageView = view,
        .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
    std::array<VkWriteDescriptorSet, 2> writes{{
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = BINDING_INPUT,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pImageInfo = nullptr,
            .pBufferInfo = &buffer_info,
            .pTexelBufferView = nullptr,
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .pNext = nullptr,
            .dstSet = VK_NULL_HANDLE,
            .dstBinding = BINDING_OUTPUT,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .pImageInfo = &image_info,
            .pBufferInfo = nullptr,
            .pTexelBufferView = nullptr,
        },
    }};
    if (!BindComputeDescriptors(device.GetLogical(), cmd, *pipeline_layout, *set_layout,
                                push_descriptors, writes, allocate_set)) {
        return false;
    }
    // The shader's addressing is block_linear_unswizzle_2d's: a row of blocks is `stride`
    // bytes (rounded up to whole 64-byte GOB columns), a tile is 2^tile_height GOBs tall.
    const u32 x_shift = Tegra::Texture::GOB_SIZE_SHIFT + level.tile_height + level.tile_depth;
    const PushConstants constants{
        .block_dims = {level.block_width, level.block_height},
        .layer_stride = level.layer_stride,
        .block_size = GobColumns(level) << x_shift,
        .x_shift = x_shift,
        .block_height = level.tile_height,
        .block_height_mask = (1u << level.tile_height) - 1u,
        .native_tiling = 1,
        .decode_extent = {level.width, level.height},
    };
    cmd.PushConstants(*pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
    const u32 columns = Common::DivCeil(level.width, level.block_width);
    const u32 rows = Common::DivCeil(level.height, level.block_height);
    cmd.Dispatch(Common::DivCeil(columns, 8U), Common::DivCeil(rows, 8U), level.layers);
    return true;
}

bool AstcGpuDecoder::RecordWithView(vk::CommandBuffer cmd, VkBuffer input, u64 input_offset,
                                    u64 input_size, VkImageView view, const Level& level,
                                    const AllocateComputeSet& allocate_set) const {
    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
    return DispatchLevel(cmd, input, input_offset, input_size, view, level, allocate_set);
}

bool AstcGpuDecoder::Record(vk::CommandBuffer cmd, VkBuffer input, u64 input_offset, u64 input_size,
                            VkImage image, std::span<const Level> levels,
                            std::vector<vk::ImageView>& views,
                            const AllocateComputeSet& allocate_set) const {
    static constexpr VkImageViewUsageCreateInfo STORAGE_USAGE{
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .pNext = nullptr,
        .usage = VK_IMAGE_USAGE_STORAGE_BIT,
    };
    views.reserve(views.size() + levels.size());
    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
    for (const Level& level : levels) {
        views.push_back(device.GetLogical().CreateImageView({
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = &STORAGE_USAGE,
            .flags = 0,
            .image = image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY,
            .format = VIEW_FORMAT,
            .components{
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange{
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = level.level,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        }));
        if (!DispatchLevel(cmd, input, input_offset, input_size, *views.back(), level,
                           allocate_set)) {
            views.pop_back();
            return false;
        }
    }
    return true;
}

bool AstcBc3::Supported(const Device& device) {
    return device.GetMaxStorageBufferRange() >= OUTPUT_TILE_BYTES;
}

AstcBc3::AstcBc3(const Device& device_)
    : device{device_},
      push_descriptors{device.IsKhrPushDescriptorSupported() && device.MaxPushDescriptors() >= 2},
      offset_alignment{(std::max)(device.GetStorageBufferAlignment(), u64{16})},
      max_range{device.GetMaxStorageBufferRange()} {
    const vk::Device& logical = device.GetLogical();
    std::array<VkDescriptorSetLayoutBinding, 2> bindings{};
    for (u32 binding = 0; binding < bindings.size(); ++binding) {
        bindings[binding] = {
            .binding = binding,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        };
    }
    set_layout = logical.CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .flags = ComputeSetFlags(push_descriptors),
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    });
    const VkPushConstantRange push_range{
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .size = sizeof(AstcBcPushConstants),
    };
    pipeline_layout = logical.CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = set_layout.address(),
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_range,
    });
    const std::span<const u32> code = ASTC_BC_COMP_SPV;
    const vk::ShaderModule module = logical.CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = code.size_bytes(),
        .pCode = code.data(),
    });
    constexpr std::array<u32, 2> modes{1, 0}; // MODE_BC3, box fit
    constexpr std::array<VkSpecializationMapEntry, 2> entries{{
        {.constantID = 0, .offset = 0, .size = sizeof(u32)},
        {.constantID = 1, .offset = sizeof(u32), .size = sizeof(u32)},
    }};
    const VkSpecializationInfo specialization{
        .mapEntryCount = static_cast<u32>(entries.size()),
        .pMapEntries = entries.data(),
        .dataSize = sizeof(modes),
        .pData = modes.data(),
    };
    pipeline = logical.CreateComputePipeline(
        VkComputePipelineCreateInfo{
            .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
            .stage{
                .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                .module = *module,
                .pName = "main",
                .pSpecializationInfo = &specialization,
            },
            .layout = *pipeline_layout,
        },
        VK_NULL_HANDLE);
}

bool AstcBc3::Record(vk::CommandBuffer cmd, VkBuffer input, u64 input_offset, VkBuffer output,
                      u64 output_offset, u32 tile_count, const AllocateComputeSet& allocate_set) const {
    if (!input || !output || input == output || tile_count == 0 || tile_count > MAX_TILES) {
        return false;
    }
    const u64 input_bytes = tile_count * INPUT_TILE_BYTES;
    const u64 output_bytes = tile_count * OUTPUT_TILE_BYTES;
    const auto fits = [this](u64 offset, u64 bytes) {
        return offset % offset_alignment == 0 && bytes <= max_range &&
               offset <= (std::numeric_limits<u64>::max)() - bytes;
    };
    if (!fits(input_offset, input_bytes) || !fits(output_offset, output_bytes)) {
        return false;
    }
    const std::array<VkDescriptorBufferInfo, 2> ranges{{
        {.buffer = input, .offset = input_offset, .range = input_bytes},
        {.buffer = output, .offset = output_offset, .range = output_bytes},
    }};
    std::array<VkWriteDescriptorSet, 2> writes{};
    for (u32 binding = 0; binding < writes.size(); ++binding) {
        writes[binding] = {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstBinding = binding,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .pBufferInfo = &ranges[binding],
        };
    }
    if (!BindComputeDescriptors(device.GetLogical(), cmd, *pipeline_layout, *set_layout,
                                push_descriptors, writes, allocate_set)) {
        return false;
    }
    constexpr AstcBcPushConstants constants{
        .width = TILE_WIDTH,
        .height = TILE_HEIGHT,
        .blocks_x = FUSED_BLOCKS_X,
        .blocks_y = FUSED_BLOCKS_Y,
        // Match NVN's UNORM scratch rounding for void extents, including sRGB textures.
        .srgb = 0,
        .block_w = FUSED_ASTC_BLOCK,
        .block_h = FUSED_ASTC_BLOCK,
        .recip_w = (1u << 18) / FUSED_ASTC_BLOCK,
        .recip_h = (1u << 18) / FUSED_ASTC_BLOCK,
        .infill_ds = FUSED_INFIL_STEP,
        .infill_dt = FUSED_INFIL_STEP,
        .layer_stride = static_cast<u32>(INPUT_TILE_BYTES),
        .block_size = FUSED_BLOCK_SIZE,
        .x_shift = FUSED_X_SHIFT,
        .block_height = FUSED_GOB_HEIGHT,
        .block_height_mask = (1u << FUSED_GOB_HEIGHT) - 1,
        .native_tiling = 1,
        .out_offset = 0,
    };
    cmd.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
    cmd.PushConstants(*pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, constants);
    cmd.Dispatch(Common::DivCeil(FUSED_BLOCKS_X, FUSED_WORKGROUP_WIDTH), FUSED_BLOCKS_Y, tile_count);
    return true;
}

} // namespace Vulkan::Nvn
