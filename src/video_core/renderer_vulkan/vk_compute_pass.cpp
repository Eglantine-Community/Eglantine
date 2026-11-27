// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2019 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <numeric>
#include <optional>
#include <utility>

#include <boost/container/small_vector.hpp>

#include "video_core/renderer_vulkan/vk_texture_cache.h"

#include "common/assert.h"
#include "common/common_types.h"
#include "common/logging.h"
#include "common/scope_exit.h"
#include "common/div_ceil.h"
#include "common/vector_math.h"
#include "video_core/host_shaders/astc_decoder_comp_spv.h"
#include "video_core/host_shaders/bc_encoder_comp_spv.h"
#include "video_core/host_shaders/queries_prefix_scan_sum_comp_spv.h"
#include "video_core/host_shaders/queries_prefix_scan_sum_nosubgroups_comp_spv.h"
#include "video_core/host_shaders/resolve_conditional_render_comp_spv.h"
#include "video_core/host_shaders/vulkan_quad_indexed_comp_spv.h"
#include "video_core/host_shaders/vulkan_uint8_comp_spv.h"
#include "video_core/host_shaders/vulkan_uint16_comp_spv.h"
#include "video_core/renderer_vulkan/index_restart.h"
#include "video_core/host_shaders/vulkan_vertex_repack_comp_spv.h"
#include "video_core/host_shaders/vulkan_plain_unswizzle_comp_spv.h"
#include "video_core/host_shaders/vulkan_msaa_depth_stencil_staging_comp_spv.h"
#include "video_core/host_shaders/block_linear_unswizzle_3d_bcn_comp_spv.h"
#include "video_core/renderer_vulkan/vk_compute_pass.h"
#include "video_core/renderer_vulkan/vertex_repack.h"
#include "video_core/surface.h"
#include "video_core/renderer_vulkan/vk_descriptor_pool.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_staging_buffer_pool.h"
#include "video_core/renderer_vulkan/vk_update_descriptor.h"
#include "video_core/texture_cache/accelerated_swizzle.h"
#include "video_core/texture_cache/types.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

namespace {

constexpr u32 ASTC_BINDING_INPUT_BUFFER = 0;
constexpr u32 ASTC_BINDING_OUTPUT_IMAGE = 1;
constexpr size_t ASTC_NUM_BINDINGS = 2;

template <size_t size>
inline constexpr VkPushConstantRange COMPUTE_PUSH_CONSTANT_RANGE{
    .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    .offset = 0,
    .size = static_cast<u32>(size),
};

constexpr std::array<VkDescriptorSetLayoutBinding, 2> INPUT_OUTPUT_DESCRIPTOR_SET_BINDINGS{{
    {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
    {
        .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
}};

constexpr std::array<VkDescriptorSetLayoutBinding, 3> QUERIES_SCAN_DESCRIPTOR_SET_BINDINGS{{
    {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
    {
        .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
    {
        .binding = 2,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
}};

constexpr DescriptorBankInfo INPUT_OUTPUT_BANK_INFO{
    .uniform_buffers = 0,
    .storage_buffers = 2,
    .texture_buffers = 0,
    .image_buffers = 0,
    .textures = 0,
    .images = 0,
    .score = 2,
};

constexpr DescriptorBankInfo QUERIES_SCAN_BANK_INFO{
    .uniform_buffers = 0,
    .storage_buffers = 3,
    .texture_buffers = 0,
    .image_buffers = 0,
    .textures = 0,
    .images = 0,
    .score = 3,
};

constexpr std::array<VkDescriptorSetLayoutBinding, ASTC_NUM_BINDINGS> ASTC_DESCRIPTOR_SET_BINDINGS{{
    {
        .binding = ASTC_BINDING_INPUT_BUFFER,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
    {
        .binding = ASTC_BINDING_OUTPUT_IMAGE,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
}};

constexpr DescriptorBankInfo ASTC_BANK_INFO{
    .uniform_buffers = 0,
    .storage_buffers = 1,
    .texture_buffers = 0,
    .image_buffers = 0,
    .textures = 0,
    .images = 1,
    .score = 2,
};

constexpr VkDescriptorUpdateTemplateEntry INPUT_OUTPUT_DESCRIPTOR_UPDATE_TEMPLATE{
    .dstBinding = 0,
    .dstArrayElement = 0,
    .descriptorCount = 2,
    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .offset = 0,
    .stride = sizeof(DescriptorUpdateEntry),
};

constexpr VkDescriptorUpdateTemplateEntry QUERIES_SCAN_DESCRIPTOR_UPDATE_TEMPLATE{
    .dstBinding = 0,
    .dstArrayElement = 0,
    .descriptorCount = 3,
    .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
    .offset = 0,
    .stride = sizeof(DescriptorUpdateEntry),
};

constexpr std::array<VkDescriptorUpdateTemplateEntry, ASTC_NUM_BINDINGS>
    ASTC_PASS_DESCRIPTOR_UPDATE_TEMPLATE_ENTRY{{
        {
            .dstBinding = ASTC_BINDING_INPUT_BUFFER,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .offset = ASTC_BINDING_INPUT_BUFFER * sizeof(DescriptorUpdateEntry),
            .stride = sizeof(DescriptorUpdateEntry),
        },
        {
            .dstBinding = ASTC_BINDING_OUTPUT_IMAGE,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
            .offset = ASTC_BINDING_OUTPUT_IMAGE * sizeof(DescriptorUpdateEntry),
            .stride = sizeof(DescriptorUpdateEntry),
        },
    }};

struct AstcPushConstants {
    std::array<u32, 2> blocks_dims;
    u32 layer_stride;
    u32 block_size;
    u32 x_shift;
    u32 block_height;
    u32 block_height_mask;
    u32 native_tiling;
    std::array<u32, 2> decode_extent;
};

constexpr std::array<VkDescriptorSetLayoutBinding, 2> BC_ENCODER_DESCRIPTOR_SET_BINDINGS{{
    {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
    {
        .binding = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
}};

constexpr std::array<VkDescriptorUpdateTemplateEntry, 2> BC_ENCODER_DESCRIPTOR_UPDATE_TEMPLATE{{
    {
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
        .offset = 0,
        .stride = sizeof(DescriptorUpdateEntry),
    },
    {
        .dstBinding = 1,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .offset = sizeof(DescriptorUpdateEntry),
        .stride = sizeof(DescriptorUpdateEntry),
    },
}};

struct BcEncoderPushConstants {
    u32 blocks_x;
    u32 blocks_y;
    u32 width;
    u32 height;
    u32 bc3;
    u32 out_offset; // in 8-byte units
};

struct QueriesPrefixScanPushConstants {
    u32 min_accumulation_base;
    u32 max_accumulation_base;
    u32 accumulation_limit;
    u32 buffer_offset;
};

struct ConditionalRenderingResolvePushConstants {
    u32 compare_to_zero;
};
} // Anonymous namespace

ComputePass::ComputePass(const Device& device_, Scheduler& scheduler, DescriptorPool& descriptor_pool,
                         vk::Span<VkDescriptorSetLayoutBinding> bindings,
                         vk::Span<VkDescriptorUpdateTemplateEntry> templates,
                         const DescriptorBankInfo& bank_info,
                         vk::Span<VkPushConstantRange> push_constants, std::span<const u32> code,
                         std::optional<u32> optional_subgroup_size)
    : device{device_} {
    descriptor_set_layout = device.GetLogical().CreateDescriptorSetLayout({
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = bindings.size(),
        .pBindings = bindings.data(),
    });
    layout = device.GetLogical().CreatePipelineLayout({
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = descriptor_set_layout.address(),
        .pushConstantRangeCount = push_constants.size(),
        .pPushConstantRanges = push_constants.data(),
    });
    if (!templates.empty()) {
        descriptor_template = device.GetLogical().CreateDescriptorUpdateTemplate({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .descriptorUpdateEntryCount = templates.size(),
            .pDescriptorUpdateEntries = templates.data(),
            .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
            .descriptorSetLayout = *descriptor_set_layout,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_COMPUTE,
            .pipelineLayout = *layout,
            .set = 0,
        });
        descriptor_allocator = descriptor_pool.Allocator(device, scheduler, *descriptor_set_layout, bank_info);
    }
    if (code.empty()) {
        return;
    }
    module = device.GetLogical().CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(code.size_bytes()),
        .pCode = code.data(),
    });
    device.SaveShader(code);
    const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup_size_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .requiredSubgroupSize = optional_subgroup_size ? *optional_subgroup_size : 32U,
    };
    bool use_setup_size = device.IsExtSubgroupSizeControlSupported() && optional_subgroup_size;
    pipeline = device.GetLogical().CreateComputePipeline(VkComputePipelineCreateInfo{
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stage{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = use_setup_size ? &subgroup_size_ci : nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = *module,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        .layout = *layout,
        .basePipelineHandle = {},
        .basePipelineIndex = 0,
    }, device.StaticPipelineCache());
}

ComputePass::~ComputePass() = default;

namespace {

constexpr u32 REPACK_WORKGROUP_SIZE = 128;
constexpr u32 REPACK_PADDING = std::numeric_limits<u32>::max();
constexpr u32 REPACK_SOURCE_INDEX_MASK = 0x0fffffff;
constexpr u32 REPACK_SCATTER_ROUTE = 0xf0000000;
constexpr u64 REPACK_MAX_ADDRESSABLE_BYTES = u64{REPACK_PADDING} & ~u64{3};
constexpr size_t REPACK_BINDINGS = 4;

constexpr auto REPACK_DESCRIPTOR_BINDINGS = [] {
    std::array<VkDescriptorSetLayoutBinding, REPACK_BINDINGS> bindings{};
    for (u32 index = 0; index < bindings.size(); ++index) {
        bindings[index] = INPUT_OUTPUT_DESCRIPTOR_SET_BINDINGS[0];
        bindings[index].binding = index;
    }
    return bindings;
}();

constexpr DescriptorBankInfo REPACK_BANK_INFO = [] {
    auto info = INPUT_OUTPUT_BANK_INFO;
    info.storage_buffers = REPACK_BINDINGS;
    info.score = REPACK_BINDINGS;
    return info;
}();

constexpr auto REPACK_DESCRIPTOR_TEMPLATE = [] {
    auto entry = INPUT_OUTPUT_DESCRIPTOR_UPDATE_TEMPLATE;
    entry.descriptorCount = REPACK_BINDINGS;
    return entry;
}();

struct RepackPushConstants {
    u32 source_bias;
    u32 source_size;
    u32 source_stride;
    u32 destination_stride;
    u32 total_words;
    u32 source_full_bytes;
};
static_assert(sizeof(RepackPushConstants) == 6 * sizeof(u32));

struct RepackChunk {
    VkDeviceSize source_base;
    VkDeviceSize destination_offset;
    VkDeviceSize destination_size;
    u32 dispatch_groups;
    RepackPushConstants constants;
};

using RepackMap = boost::container::small_vector<u32, 128>;

RepackMap MakeRepackWordMap(std::span<const u32> byte_map) {
    const size_t word_count = byte_map.size() / sizeof(u32);
    RepackMap words(word_count, REPACK_PADDING);
    for (size_t index = 0; index < word_count; ++index) {
        const auto lanes = byte_map.subspan(index * sizeof(u32), sizeof(u32));
        const auto first = std::ranges::find_if(lanes, [](u32 value) {
            return value != REPACK_PADDING;
        });
        if (first == lanes.end()) {
            continue;
        }

        const u32 destination_lane = static_cast<u32>(first - lanes.begin());
        const u32 source_first = *first;
        if (source_first <= REPACK_SOURCE_INDEX_MASK) {
            auto end = first + 1;
            for (; end != lanes.end() && *end == source_first + (end - first); ++end) {
            }
            if (std::ranges::all_of(std::ranges::subrange(end, lanes.end()), [](u32 value) {
                    return value == REPACK_PADDING;
                })) {
                // The top nibble packs the lane and span length. 0xf cannot describe a span.
                const u32 count = static_cast<u32>(end - first);
                words[index] = source_first | ((count - 1) << 28) | (destination_lane << 30);
                continue;
            }
        }

        // Keep arbitrary byte routes in a side table after the one-word descriptors.
        const u32 scatter_index = static_cast<u32>((words.size() - word_count) / sizeof(u32));
        ASSERT(scatter_index < REPACK_SOURCE_INDEX_MASK);
        words[index] = REPACK_SCATTER_ROUTE | scatter_index;
        words.insert(words.end(), lanes.begin(), lanes.end());
    }
    return words;
}

} // namespace

VertexRepackPass::VertexRepackPass(const Device& device_, Scheduler& scheduler_,
                                 DescriptorPool& descriptor_pool, StagingBufferPool& staging_pool_,
                                 ComputePassDescriptorQueue& descriptor_queue_)
    : ComputePass(device_, scheduler_, descriptor_pool, REPACK_DESCRIPTOR_BINDINGS,
                  REPACK_DESCRIPTOR_TEMPLATE, REPACK_BANK_INFO,
                  COMPUTE_PUSH_CONSTANT_RANGE<sizeof(RepackPushConstants)>,
                  VULKAN_VERTEX_REPACK_COMP_SPV),
      scheduler{scheduler_}, staging_pool{staging_pool_}, descriptor_queue{descriptor_queue_},
      max_allocation_size{StagingAllocationLimit(device_)} {}

std::optional<RepackedVertexBuffer> VertexRepackPass::Assemble(
    VkBuffer source, VkDeviceSize offset, VkDeviceSize size, VkDeviceSize buffer_size,
    const VertexRepackLayout& vertex_layout) {
    const u32 source_stride = vertex_layout.source_stride;
    const u32 stride = vertex_layout.stride;
    const u64 max_range =
        (std::min)(device.GetMaxStorageBufferRange(), REPACK_MAX_ADDRESSABLE_BYTES) & ~u64{3};
    const u64 alignment = (std::max)(device.GetStorageBufferAlignment(), VkDeviceSize{4});
    if (!source || size == 0 || offset > buffer_size || size > buffer_size - offset ||
        stride == 0 || stride % sizeof(u32) != 0 || stride > device.GetMaxVertexInputBindingStride() ||
        stride > max_range || vertex_layout.copies.empty()) {
        return std::nullopt;
    }

    const u64 vertices = source_stride != 0 ? 1 + (size - 1) / source_stride : 1;
    if (vertices > max_allocation_size / stride) {
        return std::nullopt;
    }
    const u64 output_size = vertices * stride;
    RepackMap byte_map(stride, REPACK_PADDING);
    u64 source_end = 0;
    for (const auto& copy : vertex_layout.copies) {
        const u64 copy_end = u64{copy.source_offset} + copy.byte_count;
        if (copy.byte_count == 0 || copy_end > REPACK_PADDING ||
            u64{copy.destination_offset} + copy.byte_count > stride) {
            return std::nullopt;
        }
        source_end = (std::max)(source_end, copy_end);
        for (u32 byte = 0; byte < copy.byte_count; ++byte) {
            auto& entry = byte_map[copy.destination_offset + byte];
            const u32 source_byte = copy.source_offset + byte;
            if (entry != REPACK_PADDING && entry != source_byte) {
                return std::nullopt;
            }
            entry = source_byte;
        }
    }

    const auto word_map = MakeRepackWordMap({byte_map.data(), byte_map.size()});
    const u64 map_size = u64{word_map.size()} * sizeof(u32);
    if (map_size > max_range || map_size > max_allocation_size) {
        return std::nullopt;
    }

    const u64 dispatch_bytes = u64{device.GetMaxComputeWorkGroupCount()[0]} *
                               REPACK_WORKGROUP_SIZE * sizeof(u32);
    const u64 max_output_vertices = (std::min)(max_range, dispatch_bytes) / stride;
    // Every non-final chunk must end at an aligned storage-buffer descriptor offset.
    const u64 vertex_alignment = alignment / std::gcd(alignment, u64{stride});
    boost::container::small_vector<RepackChunk, 1> chunks;
    u64 tail_size = 0;
    for (u64 first = 0; first < vertices;) {
        const u64 relative = first * source_stride;
        const u64 source_offset = offset + relative;
        const u64 source_base = source_offset / alignment * alignment;
        const u64 bias = source_offset - source_base;
        const u64 readable = size - relative;
        if (bias >= max_range) {
            return std::nullopt;
        }
        const u64 source_capacity = max_range - bias;
        u64 count = (std::min)(vertices - first, max_output_vertices);
        if (readable > source_capacity) {
            if (source_end > source_capacity) {
                return std::nullopt;
            }
            if (source_stride != 0) {
                count = (std::min)(count, 1 + (source_capacity - source_end) / source_stride);
            }
        }
        if (count < vertices - first) {
            count -= count % vertex_alignment;
        }
        if (count == 0) {
            return std::nullopt;
        }

        const u64 source_size = (std::min)(readable, (count - 1) * source_stride + source_end);
        const u64 end = bias + source_size;
        const u64 rounded_end = (end + 3) & ~u64{3};
        const u64 source_full_bytes = (std::min)(rounded_end, (buffer_size - source_base) & ~u64{3});
        // Any chunk requiring a short word reads it from the same physical end of the buffer.
        if (end > source_full_bytes) {
            tail_size = (std::max)(tail_size, end - source_full_bytes);
        }
        const u64 destination_size = count * stride;
        const u32 words = static_cast<u32>(destination_size / sizeof(u32));
        chunks.push_back({
            .source_base = source_base,
            .destination_offset = first * stride,
            .destination_size = destination_size,
            .dispatch_groups = Common::DivCeil(words, REPACK_WORKGROUP_SIZE),
            .constants{
                .source_bias = static_cast<u32>(bias),
                .source_size = static_cast<u32>(source_size),
                .source_stride = source_stride,
                .destination_stride = stride,
                .total_words = words,
                .source_full_bytes = static_cast<u32>(source_full_bytes),
            },
        });
        first += count;
    }

    // Keep every scratch allocation reserved until recording the last command that can use it.
    // A later graphics bind belongs to this tick. A caller that starts a new command buffer must
    // re-establish its vertex state there.
    auto destination = staging_pool.Request(output_size, MemoryUsage::DeviceLocal, true);
    SCOPE_EXIT {
        staging_pool.FreeDeferred(destination);
    };
    auto map = staging_pool.Request(map_size, MemoryUsage::Upload, true);
    SCOPE_EXIT {
        staging_pool.FreeDeferred(map);
    };
    std::memcpy(map.mapped_span.data(), word_map.data(), map_size);
    staging_pool.FlushDeferredUpload(map);
    std::optional<StagingBufferRef> tail_buffer;
    if (tail_size != 0) {
        tail_buffer = staging_pool.Request(sizeof(u32), MemoryUsage::DeviceLocal, true);
    }
    SCOPE_EXIT {
        if (tail_buffer) {
            staging_pool.FreeDeferred(*tail_buffer);
        }
    };
    const auto& tail = tail_buffer ? *tail_buffer : map;
    ASSERT(map.offset % alignment == 0 && destination.offset % alignment == 0 &&
           tail.offset % alignment == 0);

    scheduler.RequestOutsideRenderPassOperationContext();
    if (tail_size != 0) {
        const VkBufferCopy region{
            .srcOffset = buffer_size & ~u64{3},
            .dstOffset = tail.offset,
            .size = tail_size,
        };
        scheduler.Record([source, tail, region](vk::CommandBuffer cmdbuf) {
            const VkMemoryBarrier read_barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            };
            cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, read_barrier);
            // Copy the 1–3 physically present bytes without a uint SSBO overread.
            cmdbuf.FillBuffer(tail.buffer, tail.offset, sizeof(u32), 0);
            const VkBufferMemoryBarrier fill_barrier{
                .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .buffer = tail.buffer,
                .offset = tail.offset,
                .size = sizeof(u32),
            };
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_TRANSFER_BIT, 0, fill_barrier);
            cmdbuf.CopyBuffer(source, tail.buffer, region);
        });
    }
    for (const auto& chunk : chunks) {
        descriptor_queue.Acquire(scheduler, REPACK_BINDINGS);
        const u32 source_full_bytes = chunk.constants.source_full_bytes;
        if (source_full_bytes != 0) {
            descriptor_queue.AddBuffer(source, chunk.source_base, source_full_bytes);
        } else {
            // A descriptor must still cover a valid word for the entirely partial source.
            descriptor_queue.AddBuffer(map.buffer, map.offset, sizeof(u32));
        }
        descriptor_queue.AddBuffer(destination.buffer, destination.offset + chunk.destination_offset,
                                   chunk.destination_size);
        descriptor_queue.AddBuffer(map.buffer, map.offset, map_size);
        descriptor_queue.AddBuffer(tail.buffer, tail.offset, sizeof(u32));
        const void* data = descriptor_queue.UpdateData();
        scheduler.Record([this, data, chunk](vk::CommandBuffer cmdbuf) {
            const VkMemoryBarrier read_barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            };
            cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, read_barrier);
            const auto set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, data);
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                                 sizeof(chunk.constants), &chunk.constants);
            cmdbuf.Dispatch(chunk.dispatch_groups, 1, 1);
        });
    }
    scheduler.Record([](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier write_barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, write_barrier);
    });
    scheduler.InvalidateDescriptorBuffer();
    return RepackedVertexBuffer{destination.buffer, destination.offset, output_size};
}

PlainUnswizzlePass::PlainUnswizzlePass(const Device& device_, Scheduler& scheduler_,
                                     DescriptorPool& descriptor_pool,
                                     ComputePassDescriptorQueue& descriptor_queue_)
    : ComputePass(device_, scheduler_, descriptor_pool, INPUT_OUTPUT_DESCRIPTOR_SET_BINDINGS,
                  INPUT_OUTPUT_DESCRIPTOR_UPDATE_TEMPLATE, INPUT_OUTPUT_BANK_INFO,
                  COMPUTE_PUSH_CONSTANT_RANGE<sizeof(VideoCommon::PlainUnswizzleParameters)>,
                  VULKAN_PLAIN_UNSWIZZLE_COMP_SPV),
      scheduler{scheduler_}, descriptor_queue{descriptor_queue_} {}

void PlainUnswizzlePass::Dispatch(VkBuffer source, VkDeviceSize offset, VkBuffer destination,
                                 const VideoCommon::PlainUnswizzleParameters& parameters, u32 guard_bytes) {
    descriptor_queue.Acquire(scheduler, 2);
    descriptor_queue.AddBuffer(source, offset, u64{parameters.source_words} * 4);
    descriptor_queue.AddBuffer(destination, 0, u64{parameters.word_count} * 4 + guard_bytes);
    const void* data = descriptor_queue.UpdateData();
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, data, parameters](vk::CommandBuffer cmdbuf) {
        // The shared output may still be read by an earlier buffer-to-image copy.
        const VkMemoryBarrier before{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, before);
        const auto set = descriptor_allocator.Commit();
        device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, data);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(parameters), &parameters);
        cmdbuf.Dispatch(Common::DivCeil(parameters.word_count, 256U), 1, 1);
        const VkMemoryBarrier after{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                              VK_PIPELINE_STAGE_TRANSFER_BIT, 0, after);
    });
    scheduler.InvalidateDescriptorBuffer();
}

DepthStencilStagingPass::DepthStencilStagingPass(
    const Device& device_, Scheduler& scheduler_, DescriptorPool& descriptor_pool,
    ComputePassDescriptorQueue& descriptor_queue_)
    : ComputePass(device_, scheduler_, descriptor_pool, INPUT_OUTPUT_DESCRIPTOR_SET_BINDINGS,
                  INPUT_OUTPUT_DESCRIPTOR_UPDATE_TEMPLATE, INPUT_OUTPUT_BANK_INFO,
                  COMPUTE_PUSH_CONSTANT_RANGE<4 * sizeof(u32)>,
                  VULKAN_MSAA_DEPTH_STENCIL_STAGING_COMP_SPV),
      scheduler{scheduler_}, descriptor_queue{descriptor_queue_} {}

void DepthStencilStagingPass::CopyTile(
    VkBuffer staging, VkBuffer scratch, VkDeviceSize plane_offset, VkImage image,
    std::span<const VkBufferCopy> upload_regions, const std::array<VkBufferImageCopy, 2>& planes,
    u32 count, u32 guest_layout, bool host_float, bool download) {
    const VkDeviceSize packed_size = VkDeviceSize{count} * (guest_layout == 2 ? 8 : 4);
    const VkDeviceSize plane_size = VkDeviceSize{count} * 4 + Common::AlignUp(count, 4U);
    descriptor_queue.Acquire(scheduler, 2);
    descriptor_queue.AddBuffer(scratch, 0, packed_size);
    descriptor_queue.AddBuffer(scratch, plane_offset, plane_size);
    const void* data = descriptor_queue.UpdateData();
    const std::array<u32, 4> push{count, guest_layout, host_float, download};
    boost::container::small_vector<VkBufferCopy, 16> regions(upload_regions.begin(),
                                                            upload_regions.end());
    if (download) {
        for (auto& region : regions) {
            std::swap(region.srcOffset, region.dstOffset);
        }
    }
    scheduler.Record([this, data, push, staging, scratch, image, planes, download,
                      regions = std::move(regions)](vk::CommandBuffer cmdbuf) {
        const auto barrier = [&](VkPipelineStageFlags src_stage, VkAccessFlags src_access,
                                 VkPipelineStageFlags dst_stage, VkAccessFlags dst_access) {
            const VkMemoryBarrier memory{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .srcAccessMask = src_access,
                .dstAccessMask = dst_access,
            };
            cmdbuf.PipelineBarrier(src_stage, dst_stage, 0, memory);
        };
        constexpr VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT;
        constexpr VkPipelineStageFlags compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
        constexpr VkAccessFlags all_access = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        barrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, all_access, transfer,
                VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT);
        if (download) {
            const u32 stencil_bytes = push[0];
            if (stencil_bytes % 4 != 0) {
                // The shader reads uints, including the last partial word of the S8 plane.
                cmdbuf.FillBuffer(scratch, planes[1].bufferOffset + (stencil_bytes & ~3U), 4, 0);
                barrier(transfer, VK_ACCESS_TRANSFER_WRITE_BIT, transfer,
                        VK_ACCESS_TRANSFER_WRITE_BIT);
            }
            cmdbuf.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scratch, planes);
        } else {
            cmdbuf.CopyBuffer(staging, scratch, regions);
        }
        barrier(transfer, VK_ACCESS_TRANSFER_WRITE_BIT, compute,
                VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT);
        const auto set = descriptor_allocator.Commit();
        device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, data);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, push);
        cmdbuf.Dispatch(Common::DivCeil(push[0], 4U * LOCAL_SIZE), 1, 1);
        barrier(compute, VK_ACCESS_SHADER_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
        if (download) {
            cmdbuf.CopyBuffer(scratch, staging, regions);
            barrier(transfer, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_PIPELINE_STAGE_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_HOST_BIT,
                    all_access | VK_ACCESS_HOST_READ_BIT);
        } else {
            cmdbuf.CopyBufferToImage(scratch, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, planes);
        }
    });
    scheduler.InvalidateDescriptorBuffer();
}

namespace {

// Match local_size_x in the index assembly shaders. Vulkan guarantees at least 128 invocations
// and a size of at least 128 in the X dimension.
constexpr u32 INDEX_ASSEMBLY_WORKGROUP_SIZE = 128;

u32 IndexAssemblyGroupCount(u32 work_items, u32 max_groups) {
    // The grid-stride shaders multiply the group count by local size in u32.
    constexpr u32 max_shader_groups = std::numeric_limits<u32>::max() / INDEX_ASSEMBLY_WORKGROUP_SIZE;
    const u32 limit = (std::min)(max_groups, max_shader_groups);
    return static_cast<u32>((std::min)(
        Common::DivCeil(u64{work_items}, u64{INDEX_ASSEMBLY_WORKGROUP_SIZE}), u64{limit}));
}

struct IndexAssemblyInput {
    VkBuffer buffer;
    VkDeviceSize offset;
    VkDeviceSize range;
    std::optional<StagingBufferRef> scratch;
    u32 bias{};
};

std::optional<IndexAssemblyInput> PrepareIndexAssemblyInput(
    const Device& device, Scheduler& scheduler, StagingBufferPool& staging_pool,
    VkDeviceSize max_allocation_size, VkBuffer buffer, VkDeviceSize offset,
    VkDeviceSize logical_size, VkDeviceSize buffer_size, VkDeviceSize bytes_read, bool word_access,
    bool allow_source_bias = false) {
    if (!buffer || offset > buffer_size || bytes_read > logical_size ||
        bytes_read > buffer_size - offset) {
        return std::nullopt;
    }
    const VkDeviceSize range = word_access ? (bytes_read + 3) & ~VkDeviceSize{3} : bytes_read;
    if (range > device.GetMaxStorageBufferRange()) {
        return std::nullopt;
    }
    const VkDeviceSize alignment = (std::max)(device.GetStorageBufferAlignment(),
                                              VkDeviceSize{word_access ? 4u : 1u});
    if (offset % alignment == 0 && range <= buffer_size - offset) {
        return IndexAssemblyInput{buffer, offset, range, std::nullopt};
    }
    if (allow_source_bias) {
        const VkDeviceSize bias = offset % alignment;
        const VkDeviceSize base = offset - bias;
        const VkDeviceSize direct_range = (bias + bytes_read + 3) & ~VkDeviceSize{3};
        if (bias + bytes_read <= std::numeric_limits<u32>::max() &&
            direct_range <= device.GetMaxStorageBufferRange() && direct_range <= buffer_size - base) {
            return IndexAssemblyInput{buffer, base, direct_range, std::nullopt, static_cast<u32>(bias)};
        }
    }
    if (range > max_allocation_size) {
        return std::nullopt;
    }

    auto scratch = staging_pool.Request(range, MemoryUsage::DeviceLocal, true);
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([buffer, offset, bytes_read, range, scratch](vk::CommandBuffer cmdbuf) {
        const VkMemoryBarrier source_ready{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, 0, source_ready);
        if (bytes_read != range) {
            const VkDeviceSize last_word = scratch.offset + range - sizeof(u32);
            cmdbuf.FillBuffer(scratch.buffer, last_word, sizeof(u32), 0);
            const VkBufferMemoryBarrier fill_before_copy{
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
                                   0, fill_before_copy);
        }
        const VkBufferCopy region{.srcOffset = offset, .dstOffset = scratch.offset,
                                  .size = bytes_read};
        cmdbuf.CopyBuffer(buffer, scratch.buffer, region);
    });
    return IndexAssemblyInput{scratch.buffer, scratch.offset, range, scratch};
}

constexpr VkMemoryBarrier INDEX_ASSEMBLY_SOURCE_READY{
    .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
    .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
    .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
};

} // namespace

IndexWidenPass::IndexWidenPass(const Device& device_, Scheduler& scheduler_,
                               DescriptorPool& descriptor_pool, StagingBufferPool& staging_buffer_pool_,
                               ComputePassDescriptorQueue& compute_pass_descriptor_queue_,
                               VkIndexType source)
    : ComputePass(device_, scheduler_, descriptor_pool, INPUT_OUTPUT_DESCRIPTOR_SET_BINDINGS,
                  INPUT_OUTPUT_DESCRIPTOR_UPDATE_TEMPLATE, INPUT_OUTPUT_BANK_INFO,
                  COMPUTE_PUSH_CONSTANT_RANGE<2 * sizeof(u32)>,
                  source == VK_INDEX_TYPE_UINT16 ? std::span<const u32>{VULKAN_UINT16_COMP_SPV}
                                                 : std::span<const u32>{VULKAN_UINT8_COMP_SPV}),
      scheduler{scheduler_}, staging_buffer_pool{staging_buffer_pool_},
      compute_pass_descriptor_queue{compute_pass_descriptor_queue_},
      max_allocation_size{StagingAllocationLimit(device_)}, source_type{source} {}

IndexWidenPass::~IndexWidenPass() = default;

std::optional<std::pair<VkBuffer, VkDeviceSize>> IndexWidenPass::Assemble(
    u32 num_indices, VkBuffer src_buffer, VkDeviceSize src_offset, VkDeviceSize logical_size,
    VkDeviceSize buffer_size, u32 restart_index) {
    if (num_indices == 0) {
        return std::pair<VkBuffer, VkDeviceSize>{};
    }
    const auto size = GetIndexWideningSize(source_type, num_indices);
    const u32 groups = IndexAssemblyGroupCount(size.work_items,
                                               device.GetMaxComputeWorkGroupCount()[0]);
    const VkDeviceSize staging_size = size.output_words * sizeof(u32);
    if (staging_size > (std::min)(max_allocation_size, device.GetMaxStorageBufferRange())) {
        return std::nullopt;
    }
    auto input = PrepareIndexAssemblyInput(device, scheduler, staging_buffer_pool,
                                           max_allocation_size, src_buffer, src_offset,
                                           logical_size, buffer_size, size.input_bytes, true);
    if (!input) {
        return std::nullopt;
    }
    SCOPE_EXIT {
        if (input->scratch) {
            staging_buffer_pool.FreeDeferred(*input->scratch);
        }
    };
    auto staging = staging_buffer_pool.Request(staging_size, MemoryUsage::DeviceLocal, true);
    SCOPE_EXIT {
        staging_buffer_pool.FreeDeferred(staging);
    };
    compute_pass_descriptor_queue.Acquire(scheduler, 2);
    compute_pass_descriptor_queue.AddBuffer(input->buffer, input->offset, input->range);
    compute_pass_descriptor_queue.AddBuffer(staging.buffer, staging.offset, staging_size);
    const void* const descriptor_data{compute_pass_descriptor_queue.UpdateData()};

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, descriptor_data, groups, num_indices, restart_index](vk::CommandBuffer cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INDEX_READ_BIT,
        };
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, INDEX_ASSEMBLY_SOURCE_READY);
        const VkDescriptorSet set = descriptor_allocator.Commit();
        device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        const std::array constants{num_indices, restart_index};
        cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(constants),
                             constants.data());
        cmdbuf.Dispatch(groups, 1, 1);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, WRITE_BARRIER);
    });
    scheduler.InvalidateDescriptorBuffer();
    return std::pair{staging.buffer, staging.offset};
}

QuadIndexedPass::QuadIndexedPass(const Device& device_, Scheduler& scheduler_,
                                 DescriptorPool& descriptor_pool_,
                                 StagingBufferPool& staging_buffer_pool_,
                                 ComputePassDescriptorQueue& compute_pass_descriptor_queue_)
    : ComputePass(device_, scheduler_, descriptor_pool_, INPUT_OUTPUT_DESCRIPTOR_SET_BINDINGS,
                  INPUT_OUTPUT_DESCRIPTOR_UPDATE_TEMPLATE, INPUT_OUTPUT_BANK_INFO,
                  COMPUTE_PUSH_CONSTANT_RANGE<sizeof(u32) * 3>, VULKAN_QUAD_INDEXED_COMP_SPV),
      scheduler{scheduler_}, staging_buffer_pool{staging_buffer_pool_},
      compute_pass_descriptor_queue{compute_pass_descriptor_queue_},
      max_allocation_size{StagingAllocationLimit(device_)} {}

QuadIndexedPass::~QuadIndexedPass() = default;

std::optional<std::pair<VkBuffer, VkDeviceSize>> QuadIndexedPass::Assemble(
    Tegra::Engines::Maxwell3D::Regs::IndexFormat index_format, u32 num_vertices, u32 base_vertex,
    VkBuffer src_buffer, VkDeviceSize src_offset, VkDeviceSize logical_size,
    VkDeviceSize buffer_size, bool is_strip) {
    const u32 index_shift = [index_format] {
        switch (index_format) {
        case Tegra::Engines::Maxwell3D::Regs::IndexFormat::UnsignedByte:
            return 0;
        case Tegra::Engines::Maxwell3D::Regs::IndexFormat::UnsignedShort:
            return 1;
        case Tegra::Engines::Maxwell3D::Regs::IndexFormat::UnsignedInt:
            return 2;
        }
        ASSERT(false);
        return 2;
    }();
    if (num_vertices < 4) {
        return std::pair<VkBuffer, VkDeviceSize>{};
    }
    const u32 num_primitives = is_strip ? (num_vertices - 2) / 2 : num_vertices / 4;
    const u32 groups =
        IndexAssemblyGroupCount(num_primitives, device.GetMaxComputeWorkGroupCount()[0]);
    const VkDeviceSize indices_read = is_strip ? VkDeviceSize{num_primitives} * 2 + 2
                                              : VkDeviceSize{num_primitives} * 4;
    const VkDeviceSize staging_size = VkDeviceSize{num_primitives} * 6 * sizeof(u32);
    if (staging_size > (std::min)(max_allocation_size, device.GetMaxStorageBufferRange())) {
        return std::nullopt;
    }
    auto input = PrepareIndexAssemblyInput(device, scheduler, staging_buffer_pool,
                                           max_allocation_size, src_buffer, src_offset,
                                           logical_size, buffer_size, indices_read << index_shift,
                                           true);
    if (!input) {
        return std::nullopt;
    }
    SCOPE_EXIT {
        if (input->scratch) {
            staging_buffer_pool.FreeDeferred(*input->scratch);
        }
    };
    auto staging = staging_buffer_pool.Request(staging_size, MemoryUsage::DeviceLocal, true);
    SCOPE_EXIT {
        staging_buffer_pool.FreeDeferred(staging);
    };
    compute_pass_descriptor_queue.Acquire(scheduler, 2);
    compute_pass_descriptor_queue.AddBuffer(input->buffer, input->offset, input->range);
    compute_pass_descriptor_queue.AddBuffer(staging.buffer, staging.offset, staging_size);
    const void* const descriptor_data{compute_pass_descriptor_queue.UpdateData()};

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, descriptor_data, groups, base_vertex, index_shift,
                      is_strip](vk::CommandBuffer cmdbuf) {
        static constexpr VkMemoryBarrier WRITE_BARRIER{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_INDEX_READ_BIT,
        };
        const std::array<u32, 3> push_constants{base_vertex, index_shift, is_strip ? 1u : 0u};
        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, INDEX_ASSEMBLY_SOURCE_READY);
        const VkDescriptorSet set = descriptor_allocator.Commit();
        device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push_constants),
                             &push_constants);
        cmdbuf.Dispatch(groups, 1, 1);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_VERTEX_INPUT_BIT, 0, WRITE_BARRIER);
    });
    scheduler.InvalidateDescriptorBuffer();
    return std::pair{staging.buffer, staging.offset};
}

QuadRestartPass::QuadRestartPass(const Device& device_, Scheduler& scheduler_,
                                 DescriptorPool& descriptor_pool, StagingBufferPool& staging_pool_)
    : device{device_}, scheduler{scheduler_}, staging_pool{staging_pool_},
      max_allocation_size{StagingAllocationLimit(device)}, pipeline{device},
      descriptor_allocator{descriptor_pool.Allocator(
          device, scheduler, pipeline.SetLayout(), DescriptorBankInfo{.storage_buffers = 3, .score = 3})} {}

bool QuadRestartPass::Assemble(VkBuffer source, VkDeviceSize offset, VkDeviceSize size,
                               VkDeviceSize buffer_size, u32 count, u32 width, u32 restart_index,
                               bool strip, const DirectDraw& draw, StagingBufferRef& indices,
                               StagingBufferRef& command) {
    constexpr u32 max_u32 = std::numeric_limits<u32>::max();
    if (count < 4 || u64{count} * width > max_u32 ||
        (draw.first_instance != 0 && !device.SupportsDrawIndirectFirstInstance())) {
        return false;
    }
    const u64 max_quads = IndexedQuadGpu::MaxQuads(count, strip);
    const u64 index_bytes = max_quads * 6 * sizeof(u32);
    const u64 chunks = (count - 1) / IndexedQuadGpu::CHUNK_INDICES + 1;
    const u64 command_bytes = IndexedQuadGpu::COMMAND_SIZE + chunks * IndexedQuadGpu::CHUNK_SIZE;
    const auto max_range = (std::min)(max_allocation_size, device.GetMaxStorageBufferRange());
    if (index_bytes / sizeof(u32) > max_u32 || index_bytes > max_range || command_bytes > max_range) {
        return false;
    }

    auto input = PrepareIndexAssemblyInput(device, scheduler, staging_pool, max_allocation_size,
                                           source, offset, size, buffer_size, u64{count} * width,
                                           true, true);
    if (!input) {
        return false;
    }
    SCOPE_EXIT {
        if (input->scratch) {
            staging_pool.FreeDeferred(*input->scratch);
        }
    };
    auto output_indices = staging_pool.Request(index_bytes, MemoryUsage::DeviceLocal, true);
    SCOPE_EXIT {
        if (output_indices.buffer) {
            staging_pool.FreeDeferred(output_indices);
        }
    };
    auto output_command = staging_pool.Request(command_bytes, MemoryUsage::DeviceLocal, true);
    SCOPE_EXIT {
        if (output_command.buffer) {
            staging_pool.FreeDeferred(output_command);
        }
    };
    const std::array<VkDescriptorBufferInfo, 3> buffers{{
        {input->buffer, input->offset, input->range},
        {output_indices.buffer, output_indices.offset, index_bytes},
        {output_command.buffer, output_command.offset, command_bytes},
    }};
    const IndexedQuadGpu::Constants push{
        .index_count = count,
        .index_width = width,
        .source_bias = input->bias,
        .restart_index = restart_index,
        .flags = strip ? 1u : 0u,
        .instance_count = draw.instances,
        .vertex_offset = draw.vertex_offset,
        .first_instance = draw.first_instance,
        .draw_index_limit = max_u32,
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, buffers, push](vk::CommandBuffer cmdbuf) {
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                               VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, INDEX_ASSEMBLY_SOURCE_READY);
        pipeline.Convert(cmdbuf, descriptor_allocator.Commit(), buffers, push, true);
    });
    scheduler.InvalidateDescriptorBuffer();
    indices = std::exchange(output_indices, {});
    command = std::exchange(output_command, {});
    return true;
}

ConditionalRenderingResolvePass::ConditionalRenderingResolvePass(
    const Device& device_, Scheduler& scheduler_, DescriptorPool& descriptor_pool_,
    ComputePassDescriptorQueue& compute_pass_descriptor_queue_)
    : ComputePass(device_, scheduler_, descriptor_pool_, INPUT_OUTPUT_DESCRIPTOR_SET_BINDINGS,
                  INPUT_OUTPUT_DESCRIPTOR_UPDATE_TEMPLATE, INPUT_OUTPUT_BANK_INFO,
                  COMPUTE_PUSH_CONSTANT_RANGE<sizeof(ConditionalRenderingResolvePushConstants)>,
                  RESOLVE_CONDITIONAL_RENDER_COMP_SPV),
      scheduler{scheduler_}, compute_pass_descriptor_queue{compute_pass_descriptor_queue_} {}

void ConditionalRenderingResolvePass::Resolve(VkBuffer dst_buffer, VkBuffer src_buffer,
                                              u32 src_offset, bool compare_to_zero) {
    if (!device.IsExtConditionalRendering()) {
        return;
    }
    const size_t compare_size = compare_to_zero ? 8 : 24;

    compute_pass_descriptor_queue.Acquire(scheduler, 2);
    compute_pass_descriptor_queue.AddBuffer(src_buffer, src_offset, compare_size);
    compute_pass_descriptor_queue.AddBuffer(dst_buffer, 0, sizeof(u32));
    const void* const descriptor_data{compute_pass_descriptor_queue.UpdateData()};

    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([this, descriptor_data, compare_to_zero](vk::CommandBuffer cmdbuf) {
        static constexpr VkMemoryBarrier read_barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
        };
        static constexpr VkMemoryBarrier write_barrier{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT,
        };
        const ConditionalRenderingResolvePushConstants uniforms{
            .compare_to_zero = compare_to_zero ? 1U : 0U,
        };
        const VkDescriptorSet set = descriptor_allocator.Commit();
        device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);

        cmdbuf.PipelineBarrier(vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER,
                       VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, read_barrier);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, uniforms);
        cmdbuf.Dispatch(1, 1, 1);
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               VK_PIPELINE_STAGE_CONDITIONAL_RENDERING_BIT_EXT, 0,
                               write_barrier);
    });
    scheduler.InvalidateDescriptorBuffer();
}

QueriesPrefixScanPass::QueriesPrefixScanPass(
    const Device& device_, Scheduler& scheduler_, DescriptorPool& descriptor_pool_,
    ComputePassDescriptorQueue& compute_pass_descriptor_queue_)
    : ComputePass(
          device_, scheduler_, descriptor_pool_, QUERIES_SCAN_DESCRIPTOR_SET_BINDINGS,
          QUERIES_SCAN_DESCRIPTOR_UPDATE_TEMPLATE, QUERIES_SCAN_BANK_INFO,
          COMPUTE_PUSH_CONSTANT_RANGE<sizeof(QueriesPrefixScanPushConstants)>,
          device_.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_BASIC_BIT) &&
                  device_.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_ARITHMETIC_BIT) &&
                  device_.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_SHUFFLE_BIT) &&
                  device_.IsSubgroupFeatureSupported(VK_SUBGROUP_FEATURE_SHUFFLE_RELATIVE_BIT)
              ? std::span<const u32>(QUERIES_PREFIX_SCAN_SUM_COMP_SPV)
              : std::span<const u32>(QUERIES_PREFIX_SCAN_SUM_NOSUBGROUPS_COMP_SPV)),
      scheduler{scheduler_}, compute_pass_descriptor_queue{compute_pass_descriptor_queue_} {}

void QueriesPrefixScanPass::Run(VkBuffer accumulation_buffer, VkBuffer dst_buffer,
                                VkBuffer src_buffer, size_t number_of_sums,
                                size_t min_accumulation_limit, size_t max_accumulation_limit) {
    constexpr VkAccessFlags BASE_DST_ACCESS = VK_ACCESS_SHADER_READ_BIT |
                                              VK_ACCESS_TRANSFER_READ_BIT |
                                              VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT |
                                              VK_ACCESS_INDIRECT_COMMAND_READ_BIT |
                                              VK_ACCESS_INDEX_READ_BIT |
                                              VK_ACCESS_UNIFORM_READ_BIT;
    const VkAccessFlags conditional_access =
        device.IsExtConditionalRendering() ? VK_ACCESS_CONDITIONAL_RENDERING_READ_BIT_EXT : 0;
    size_t current_runs = number_of_sums;
    size_t offset = 0;
    while (current_runs != 0) {
        static constexpr size_t DISPATCH_SIZE = 2048U;
        size_t runs_to_do = std::min<size_t>(current_runs, DISPATCH_SIZE);
        current_runs -= runs_to_do;
        compute_pass_descriptor_queue.Acquire(scheduler, 3);
        compute_pass_descriptor_queue.AddBuffer(src_buffer, 0, number_of_sums * sizeof(u64));
        compute_pass_descriptor_queue.AddBuffer(dst_buffer, 0, number_of_sums * sizeof(u64));
        compute_pass_descriptor_queue.AddBuffer(accumulation_buffer, 0, sizeof(u64));
        const void* const descriptor_data{compute_pass_descriptor_queue.UpdateData()};
        size_t used_offset = offset;
        offset += runs_to_do;

        scheduler.RequestOutsideRenderPassOperationContext();
        scheduler.Record([this, descriptor_data, min_accumulation_limit, max_accumulation_limit,
                          runs_to_do, used_offset, conditional_access](vk::CommandBuffer cmdbuf) {
            static constexpr VkMemoryBarrier read_barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            };
            const VkMemoryBarrier write_barrier{
                .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
                .dstAccessMask = BASE_DST_ACCESS | conditional_access,
            };
            const QueriesPrefixScanPushConstants uniforms{
                .min_accumulation_base = static_cast<u32>(min_accumulation_limit),
                .max_accumulation_base = static_cast<u32>(max_accumulation_limit),
                .accumulation_limit = static_cast<u32>(runs_to_do - 1),
                .buffer_offset = static_cast<u32>(used_offset),
            };
            const VkDescriptorSet set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);

            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT,
                                   VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, read_barrier);
            cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, uniforms);
            cmdbuf.Dispatch(1, 1, 1);
            cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                   vk::PIPELINE_STAGE_GRAPHICS_COMPUTE, 0, write_barrier);
        });
        scheduler.InvalidateDescriptorBuffer();
    }
}

ASTCDecoderPass::ASTCDecoderPass(const Device& device_, Scheduler& scheduler_,
                                 DescriptorPool& descriptor_pool_,
                                 StagingBufferPool& staging_buffer_pool_,
                                 ComputePassDescriptorQueue& compute_pass_descriptor_queue_,
                                 MemoryAllocator& memory_allocator_)
    : ComputePass(device_, scheduler_, descriptor_pool_, ASTC_DESCRIPTOR_SET_BINDINGS,
                  ASTC_PASS_DESCRIPTOR_UPDATE_TEMPLATE_ENTRY, ASTC_BANK_INFO,
                  COMPUTE_PUSH_CONSTANT_RANGE<sizeof(AstcPushConstants)>, ASTC_DECODER_COMP_SPV),
      scheduler{scheduler_}, staging_buffer_pool{staging_buffer_pool_},
      compute_pass_descriptor_queue{compute_pass_descriptor_queue_}, memory_allocator{
                                                                         memory_allocator_} {}

ASTCDecoderPass::~ASTCDecoderPass() = default;

void ASTCDecoderPass::Assemble(Image& image, const StagingBufferRef& map,
                               std::span<const VideoCommon::SwizzleParameters> swizzles) {
    const Target target{
        .image = image.Handle(),
        .aspect_mask = image.AspectMask(),
        .initialized = image.ExchangeInitialization(),
        .level_view = [&image](s32 host_level) { return image.StorageImageView(host_level); },
        .wait = true,
    };
    AssembleInto(image.info, image.host_level_offset, image.guest_size_bytes, target, map,
                 swizzles);
}

void ASTCDecoderPass::AssembleLayers(Image& image, const StagingBufferRef& map,
                                     std::span<const VideoCommon::SwizzleParameters> swizzles,
                                     std::span<const u32> layers) {
    const Target target{
        .image = image.Handle(),
        .aspect_mask = image.AspectMask(),
        .initialized = image.ExchangeInitialization(),
        .level_view = [&image](s32 host_level) { return image.StorageImageView(host_level); },
        .wait = true,
        .layers = layers,
        .layer_view = [&image](s32 host_level, u32 layer) {
            return image.StorageImageLayerView(host_level, layer);
        },
    };
    AssembleInto(image.info, image.host_level_offset, image.guest_size_bytes, target, map,
                 swizzles);
}

void ASTCDecoderPass::AssembleInto(const VideoCommon::ImageInfo& info, u32 skip,
                                   size_t guest_size_bytes, const Target& target,
                                   const StagingBufferRef& map,
                                   std::span<const VideoCommon::SwizzleParameters> swizzles) {
    using namespace VideoCommon::Accelerated;
    const std::array<u32, 2> block_dims{
        VideoCore::Surface::DefaultBlockWidth(info.format),
        VideoCore::Surface::DefaultBlockHeight(info.format),
    };
    scheduler.RequestOutsideRenderPassOperationContext();
    const VkPipeline vk_pipeline = *pipeline;
    const VkImageAspectFlags aspect_mask = target.aspect_mask;
    const VkImage vk_image = target.image;
    const bool is_initialized = target.initialized;
    const bool compute_only = target.compute_only_barrier;
    scheduler.Record([vk_pipeline, vk_image, aspect_mask, is_initialized,
                      compute_only](vk::CommandBuffer cmdbuf) {
        const VkImageMemoryBarrier image_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = static_cast<VkAccessFlags>(is_initialized ? VK_ACCESS_SHADER_WRITE_BIT
                                                                       : VK_ACCESS_NONE),
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = is_initialized ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vk_image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        const VkPipelineStageFlags src_stage =
            !is_initialized ? VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT)
            : compute_only  ? VkPipelineStageFlags(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                            : vk::PIPELINE_STAGE_GRAPHICS_COMPUTE_TRANSFER;
        cmdbuf.PipelineBarrier(src_stage, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, image_barrier);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, vk_pipeline);
    });
    // With trimmed host levels the input holds each layer's mip chain from host_level_offset on
    // (see TextureCache::UploadImageContents), so its layer stride is that tail's size.
    const u32 input_layer_stride =
        skip != 0 ? info.layer_stride -
                        static_cast<u32>(VideoCommon::FullUploadSwizzles(info)[skip].buffer_offset)
                  : info.layer_stride;
    const size_t input_size = skip != 0 ? size_t{input_layer_stride} * info.resources.layers
                                        : guest_size_bytes;
    // Only some layers: one dispatch per layer and level, into that layer's own view.
    for (size_t i = 0; i < target.layers.size(); ++i) {
        for (const VideoCommon::SwizzleParameters& swizzle : swizzles) {
            const size_t layer_offset = i * input_layer_stride;
            compute_pass_descriptor_queue.Acquire(scheduler, 2);
            compute_pass_descriptor_queue.AddBuffer(map.buffer,
                                                    map.offset + layer_offset + swizzle.buffer_offset,
                                                    input_layer_stride - swizzle.buffer_offset);
            compute_pass_descriptor_queue.AddImage(
                target.layer_view(swizzle.level - static_cast<s32>(skip), target.layers[i]));
            const void* const descriptor_data{compute_pass_descriptor_queue.UpdateData()};
            auto params = MakeBlockLinearSwizzle2DParams(swizzle, info);
            params.layer_stride = input_layer_stride;
            const u32 num_dispatches_x = Common::DivCeil(swizzle.num_tiles.width, 8U);
            const u32 num_dispatches_y = Common::DivCeil(swizzle.num_tiles.height, 8U);
            const std::array decode_extent{swizzle.num_tiles.width * block_dims[0],
                                           swizzle.num_tiles.height * block_dims[1]};
            scheduler.Record([this, num_dispatches_x, num_dispatches_y, block_dims, params,
                              decode_extent, descriptor_data](vk::CommandBuffer cmdbuf) {
                const AstcPushConstants uniforms{
                    .blocks_dims = block_dims,
                    .layer_stride = params.layer_stride,
                    .block_size = params.block_size,
                    .x_shift = params.x_shift,
                    .block_height = params.block_height,
                    .block_height_mask = params.block_height_mask,
                    .native_tiling = params.native_tiling,
                    .decode_extent = decode_extent,
                };
                const VkDescriptorSet set = descriptor_allocator.Commit();
                device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);
                cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
                cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, uniforms);
                cmdbuf.Dispatch(num_dispatches_x, num_dispatches_y, 1);
            });
        }
    }
    for (const VideoCommon::SwizzleParameters& swizzle : target.layers.empty() ? swizzles
                                                        : std::span<const VideoCommon::SwizzleParameters>{}) {
        const size_t input_offset = swizzle.buffer_offset + map.offset;
        const u32 num_dispatches_x = Common::DivCeil(swizzle.num_tiles.width, 8U);
        const u32 num_dispatches_y = Common::DivCeil(swizzle.num_tiles.height, 8U);
        const u32 num_dispatches_z = info.resources.layers;
        const std::array decode_extent{swizzle.num_tiles.width * block_dims[0],
                                       swizzle.num_tiles.height * block_dims[1]};

        compute_pass_descriptor_queue.Acquire(scheduler, 2);
        compute_pass_descriptor_queue.AddBuffer(map.buffer, input_offset,
                                                input_size - swizzle.buffer_offset);
        compute_pass_descriptor_queue.AddImage(
            target.level_view(swizzle.level - static_cast<s32>(skip)));
        const void* const descriptor_data{compute_pass_descriptor_queue.UpdateData()};

        // To unswizzle the ASTC data
        auto params = MakeBlockLinearSwizzle2DParams(swizzle, info);
        params.layer_stride = input_layer_stride;
        ASSERT(params.origin == (std::array<u32, 3>{0, 0, 0}));
        ASSERT(params.destination == (std::array<s32, 3>{0, 0, 0}));
        ASSERT(params.bytes_per_block_log2 == 4);
        scheduler.Record([this, num_dispatches_x, num_dispatches_y, num_dispatches_z, block_dims,
                          params, decode_extent, descriptor_data](vk::CommandBuffer cmdbuf) {
            const AstcPushConstants uniforms{
                .blocks_dims = block_dims,
                .layer_stride = params.layer_stride,
                .block_size = params.block_size,
                .x_shift = params.x_shift,
                .block_height = params.block_height,
                .block_height_mask = params.block_height_mask,
                .native_tiling = params.native_tiling,
                .decode_extent = decode_extent,
            };
            const VkDescriptorSet set = descriptor_allocator.Commit();
            device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);
            cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
            cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, uniforms);
            cmdbuf.Dispatch(num_dispatches_x, num_dispatches_y, num_dispatches_z);
        });
    }
    scheduler.Record([vk_image, aspect_mask, compute_only](vk::CommandBuffer cmdbuf) {
        const VkImageMemoryBarrier image_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = static_cast<VkAccessFlags>(
                compute_only ? VK_ACCESS_SHADER_READ_BIT
                             : (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT)),
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = vk_image,
            .subresourceRange{
                .aspectMask = aspect_mask,
                .baseMipLevel = 0,
                .levelCount = VK_REMAINING_MIP_LEVELS,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        const VkPipelineStageFlags dst_stage =
            compute_only ? VkPipelineStageFlags(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT)
                         : vk::PIPELINE_STAGE_GRAPHICS_COMPUTE;
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, dst_stage, 0, image_barrier);
    });
    scheduler.InvalidateDescriptorBuffer();
    // Each decode ends with a submit to the host GPU. Measured here
    // (count and time per 5 s); EDEN_NX2_ASTC_FLUSH=0 waits for the host GPU to go idle instead.
    static const bool flush_only = [] {
        const char* value = std::getenv("EDEN_NX2_ASTC_FLUSH");
        return value == nullptr || value[0] != '0';
    }();
    if (!target.wait) {
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    if (flush_only) {
        scheduler.Flush();
    } else {
        scheduler.Finish();
    }
    const auto end = std::chrono::steady_clock::now();
    static u32 decodes{};
    static std::chrono::nanoseconds waited{};
    static auto window_start = end;
    ++decodes;
    waited += end - start;
    if (end - window_start >= std::chrono::seconds{5}) {
        LOG_INFO(Render_Vulkan, "ASTC GPU decodes: {} in {} ms, {} ms waiting ({})", decodes,
                 std::chrono::duration_cast<std::chrono::milliseconds>(end - window_start).count(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(waited).count(),
                 flush_only ? "flush" : "finish");
        decodes = 0;
        waited = {};
        window_start = end;
    }
}

BcEncoderPass::BcEncoderPass(const Device& device_, Scheduler& scheduler_,
                             DescriptorPool& descriptor_pool_,
                             ComputePassDescriptorQueue& compute_pass_descriptor_queue_)
    : ComputePass(device_, scheduler_, descriptor_pool_, BC_ENCODER_DESCRIPTOR_SET_BINDINGS,
                  BC_ENCODER_DESCRIPTOR_UPDATE_TEMPLATE, ASTC_BANK_INFO,
                  COMPUTE_PUSH_CONSTANT_RANGE<sizeof(BcEncoderPushConstants)>, BC_ENCODER_COMP_SPV),
      scheduler{scheduler_}, compute_pass_descriptor_queue{compute_pass_descriptor_queue_} {}

BcEncoderPass::~BcEncoderPass() = default;

void BcEncoderPass::Encode(VkImageView source, u32 width, u32 height, u32 layers, bool bc3,
                           VkBuffer buffer, VkDeviceSize buffer_offset, VkDeviceSize buffer_size,
                           VkDeviceSize out_offset) {
    scheduler.RequestOutsideRenderPassOperationContext();
    compute_pass_descriptor_queue.Acquire(scheduler, 2);
    compute_pass_descriptor_queue.AddImage(source);
    compute_pass_descriptor_queue.AddBuffer(buffer, buffer_offset, buffer_size);
    const void* const descriptor_data{compute_pass_descriptor_queue.UpdateData()};
    const BcEncoderPushConstants uniforms{
        .blocks_x = Common::DivCeil(width, 4U),
        .blocks_y = Common::DivCeil(height, 4U),
        .width = width,
        .height = height,
        .bc3 = bc3 ? 1U : 0U,
        .out_offset = static_cast<u32>(out_offset / 8),
    };
    const VkPipeline vk_pipeline = *pipeline;
    scheduler.Record([this, vk_pipeline, uniforms, layers, descriptor_data](vk::CommandBuffer cmdbuf) {
        const VkDescriptorSet set = descriptor_allocator.Commit();
        device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, vk_pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, uniforms);
        cmdbuf.Dispatch(Common::DivCeil(uniforms.blocks_x, 8U), Common::DivCeil(uniforms.blocks_y, 8U),
                        layers);
    });
    scheduler.InvalidateDescriptorBuffer();
}

constexpr u32 BL3D_BINDING_INPUT_BUFFER  = 0;
constexpr u32 BL3D_BINDING_OUTPUT_BUFFER = 1;

struct alignas(16) BlockLinearUnswizzle3DPushConstants {
    u32 blocks_dim[3];           // Offset 0
    u32 bytes_per_block_log2;    // Offset 12

    u32 origin[3];               // Offset 16
    u32 slice_size;              // Offset 28

    u32 block_size;              // Offset 32
    u32 x_shift;                 // Offset 36
    u32 block_height;            // Offset 40
    u32 block_height_mask;       // Offset 44

    u32 block_depth;             // Offset 48
    u32 block_depth_mask;        // Offset 52
    s32 _pad;                    // Offset 56

    s32 destination[3];          // Offset 60
    s32 _pad_end;                // Offset 72
};
static_assert(sizeof(BlockLinearUnswizzle3DPushConstants) <= 128);

BlockLinearUnswizzle3DPass::BlockLinearUnswizzle3DPass(
    const Device& device_, Scheduler& scheduler_,
    DescriptorPool& descriptor_pool_,
    StagingBufferPool& staging_buffer_pool_,
    ComputePassDescriptorQueue& compute_pass_descriptor_queue_)
    : ComputePass(device_, scheduler_, descriptor_pool_,
        std::array<VkDescriptorSetLayoutBinding, 2>{{
    {
        .binding = BL3D_BINDING_INPUT_BUFFER,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, // block-linear input
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
    {
        .binding = BL3D_BINDING_OUTPUT_BUFFER,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
        .pImmutableSamplers = nullptr,
    },
        }},
        std::array<VkDescriptorUpdateTemplateEntry, 2>{{
        {
            .dstBinding = BL3D_BINDING_INPUT_BUFFER,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .offset = BL3D_BINDING_INPUT_BUFFER * sizeof(DescriptorUpdateEntry),
            .stride = sizeof(DescriptorUpdateEntry),
        },
        {
            .dstBinding = BL3D_BINDING_OUTPUT_BUFFER,
            .dstArrayElement = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .offset = BL3D_BINDING_OUTPUT_BUFFER * sizeof(DescriptorUpdateEntry),
            .stride = sizeof(DescriptorUpdateEntry),
        }
        }},
        DescriptorBankInfo{
            .uniform_buffers = 0,
            .storage_buffers = 2,
            .texture_buffers = 0,
            .image_buffers = 0,
            .textures = 0,
            .images = 0,
            .score = 2,
        },
          COMPUTE_PUSH_CONSTANT_RANGE<sizeof(BlockLinearUnswizzle3DPushConstants)>,
          BLOCK_LINEAR_UNSWIZZLE_3D_BCN_COMP_SPV),
      scheduler{scheduler_},
      staging_buffer_pool{staging_buffer_pool_},
      compute_pass_descriptor_queue{compute_pass_descriptor_queue_} {}

BlockLinearUnswizzle3DPass::~BlockLinearUnswizzle3DPass() = default;

// God have mercy on my soul
void BlockLinearUnswizzle3DPass::Unswizzle(
    Image& image,
    const StagingBufferRef& swizzled,
    std::span<const VideoCommon::SwizzleParameters> swizzles,
    u32 z_start, u32 z_count)
{
    using namespace VideoCommon::Accelerated;

    const u32 MAX_BATCH_SLICES = (std::min)(z_count, image.info.size.depth);

    // Allocate or grow to cover this batch's slice count
    image.AllocateComputeUnswizzleBuffer(MAX_BATCH_SLICES);

    ASSERT(swizzles.size() == 1);
    const auto& sw = swizzles[0];
    const auto params = MakeBlockLinearSwizzle3DParams(sw, image.info);

    const u32 blocks_x = (image.info.size.width  + 3) / 4;
    const u32 blocks_y = (image.info.size.height + 3) / 4;

    scheduler.RequestOutsideRenderPassOperationContext();
    for (u32 z_offset = 0; z_offset < z_count; z_offset += MAX_BATCH_SLICES) {
        const u32 current_chunk_slices = (std::min)(MAX_BATCH_SLICES, z_count - z_offset);
        const u32 current_z_start = z_start + z_offset;

        UnswizzleChunk(image, swizzled, sw, params, blocks_x, blocks_y,
                       current_z_start, current_chunk_slices);
    }
}

void BlockLinearUnswizzle3DPass::UnswizzleChunk(
    Image& image,
    const StagingBufferRef& swizzled,
    const VideoCommon::SwizzleParameters& sw,
    const BlockLinearSwizzle3DParams& params,
    u32 blocks_x, u32 blocks_y,
    u32 z_start, u32 z_count)
{
    BlockLinearUnswizzle3DPushConstants pc{};
    pc.origin[0] = params.origin[0];
    pc.origin[1] = params.origin[1];
    pc.origin[2] = z_start; // Current chunk's Z start

    pc.destination[0] = params.destination[0];
    pc.destination[1] = params.destination[1];
    pc.destination[2] = 0; // Shader writes to start of output buffer

    pc.bytes_per_block_log2 = params.bytes_per_block_log2;
    pc.slice_size           = params.slice_size;
    pc.block_size           = params.block_size;
    pc.x_shift              = params.x_shift;
    pc.block_height         = params.block_height;
    pc.block_height_mask    = params.block_height_mask;
    pc.block_depth          = params.block_depth;
    pc.block_depth_mask     = params.block_depth_mask;

    pc.blocks_dim[0] = blocks_x;
    pc.blocks_dim[1] = blocks_y;
    pc.blocks_dim[2] = z_count; // Only process the count

    compute_pass_descriptor_queue.Acquire(scheduler, 3);
    compute_pass_descriptor_queue.AddBuffer(swizzled.buffer,
                                           sw.buffer_offset + swizzled.offset,
                                           image.guest_size_bytes - sw.buffer_offset);
    compute_pass_descriptor_queue.AddBuffer(*image.compute_unswizzle_buffer, 0,
                                           image.compute_unswizzle_buffer_size);

    const void* descriptor_data = compute_pass_descriptor_queue.UpdateData();
    const VkDescriptorSet set = descriptor_allocator.Commit();

    const u32 gx = Common::DivCeil(blocks_x, 8u);
    const u32 gy = Common::DivCeil(blocks_y, 8u);
    const u32 gz = Common::DivCeil(z_count, 4u);

    const u32 bytes_per_block = 1u << pc.bytes_per_block_log2;
    const VkDeviceSize output_slice_size =
        static_cast<VkDeviceSize>(blocks_x) * blocks_y * bytes_per_block;
    const VkDeviceSize barrier_size = output_slice_size * z_count;

    const bool is_first_chunk = (z_start == 0);

    const VkBuffer out_buffer = *image.compute_unswizzle_buffer;
    const VkImage dst_image = image.Handle();
    const VkImageAspectFlags aspect = image.AspectMask();
    const u32 image_width = image.info.size.width;
    const u32 image_height = image.info.size.height;

    scheduler.Record([this, set, descriptor_data, pc, gx, gy, gz, z_start, z_count,
                      barrier_size, is_first_chunk, out_buffer, dst_image, aspect,
                      image_width, image_height
                      ](vk::CommandBuffer cmdbuf) {

        if (dst_image == VK_NULL_HANDLE || out_buffer == VK_NULL_HANDLE) {
            return;
        }

        device.GetLogical().UpdateDescriptorSet(set, *descriptor_template, descriptor_data);
        cmdbuf.BindPipeline(VK_PIPELINE_BIND_POINT_COMPUTE, *pipeline);
        cmdbuf.BindDescriptorSets(VK_PIPELINE_BIND_POINT_COMPUTE, *layout, 0, set, {});
        cmdbuf.PushConstants(*layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
        cmdbuf.Dispatch(gx, gy, gz);

        // Single barrier for compute -> transfer (buffer ready, image transition)
        const VkBufferMemoryBarrier buffer_barrier{
            .sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = out_buffer,
            .offset = 0,
            .size = barrier_size,
        };

        // Image layout transition
        const VkImageMemoryBarrier pre_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = is_first_chunk ? VkAccessFlags{} :
                            static_cast<VkAccessFlags>(VK_ACCESS_TRANSFER_WRITE_BIT),
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = is_first_chunk ? VK_IMAGE_LAYOUT_UNDEFINED :
                        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst_image,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        };

        // Single barrier handles both buffer and image
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0,
            nullptr, buffer_barrier, pre_barrier
        );

        // Copy chunk to correct Z position in image
        const VkBufferImageCopy copy{
            .bufferOffset = 0, // Read from start of staging buffer
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource = {aspect, 0, 0, 1},
            .imageOffset = {0, 0, static_cast<s32>(z_start)}, // Write to correct Z
            .imageExtent = {image_width, image_height, z_count},
        };
        cmdbuf.CopyBufferToImage(out_buffer, dst_image,
                                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, copy);

        // Post-copy transition
        const VkImageMemoryBarrier post_barrier{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = dst_image,
            .subresourceRange = {aspect, 0, 1, 0, 1},
        };

        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
            0,
            nullptr, nullptr, post_barrier
        );
    });
    scheduler.InvalidateDescriptorBuffer();
}

} // namespace Vulkan
