// SPDX-FileCopyrightText: Copyright 2026 Dalia Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <vector>

#include "common/common_types.h"
#include "common/container/unordered_map.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

class Device;

struct SharedPipelineLayoutBundle {
    vk::DescriptorSetLayout descriptor_set_layout;
    vk::PipelineLayout pipeline_layout;
    vk::DescriptorUpdateTemplate descriptor_update_template;
    DescriptorBufferLayout descriptor_buffer_layout{};
    bool uses_push_descriptor{};
    bool uses_descriptor_buffer{};
};

struct GraphicsPipelineLibraryInput {
    std::array<u64, 6> unique_hashes{};
    std::shared_ptr<const SharedPipelineLayoutBundle> layout_bundle;
    VkRenderPass render_pass{VK_NULL_HANDLE};
    VkPipelineCreateFlags base_flags{};
    std::span<const VkPipelineShaderStageCreateInfo> shader_stages;
    const VkPipelineVertexInputStateCreateInfo* vertex_input_ci{};
    const VkPipelineVertexInputDivisorStateCreateInfoEXT* input_divisor_ci{};
    const VkPipelineInputAssemblyStateCreateInfo* input_assembly_ci{};
    const VkPipelineTessellationStateCreateInfo* tessellation_ci{};
    const VkPipelineViewportStateCreateInfo* viewport_ci{};
    const VkPipelineViewportSwizzleStateCreateInfoNV* swizzle_ci{};
    const VkPipelineViewportDepthClipControlCreateInfoEXT* ndc_info{};
    const VkPipelineRasterizationStateCreateInfo* rasterization_ci{};
    const VkPipelineRasterizationLineStateCreateInfoEXT* line_state_ci{};
    const VkPipelineRasterizationConservativeStateCreateInfoEXT* conservative_raster_ci{};
    const VkPipelineRasterizationProvokingVertexStateCreateInfoEXT* provoking_vertex_ci{};
    const VkPipelineMultisampleStateCreateInfo* multisample_ci{};
    const VkPipelineDepthStencilStateCreateInfo* depth_stencil_ci{};
    const VkPipelineColorBlendStateCreateInfo* color_blend_ci{};
    std::span<const VkDynamicState> dynamic_states;
};

struct GraphicsPipelineLibraryStats {
    size_t vertex_input_libraries{};
    size_t shader_libraries{};
    size_t fragment_output_libraries{};
    size_t layout_bundles{};
    size_t vertex_input_hits{};
    size_t shader_hits{};
    size_t fragment_output_hits{};
    size_t linked_pipelines{};
    size_t fallback_monolithic{};
};

class GraphicsPipelineLibraryCache {
public:
    explicit GraphicsPipelineLibraryCache(const Device& device, vk::PipelineCache& pipeline_cache);
    ~GraphicsPipelineLibraryCache();

    GraphicsPipelineLibraryCache(const GraphicsPipelineLibraryCache&) = delete;
    GraphicsPipelineLibraryCache& operator=(const GraphicsPipelineLibraryCache&) = delete;

    [[nodiscard]] bool IsSupported() const noexcept;
    [[nodiscard]] bool IsFastLinkingSupported() const noexcept;

    [[nodiscard]] std::shared_ptr<const SharedPipelineLayoutBundle> GetOrCreateLayout(
        const std::array<u64, 6>& unique_hashes, u64 layout_digest,
        const std::function<std::shared_ptr<SharedPipelineLayoutBundle>()>& creator);

    [[nodiscard]] vk::Pipeline BuildLinkedPipeline(const GraphicsPipelineLibraryInput& input);

    [[nodiscard]] bool PrewarmShaderStage(VkShaderStageFlagBits stage, VkShaderModule module,
                                          VkPipelineLayout layout, VkRenderPass compatible_rp,
                                          bool required_subgroup_size = false,
                                          u32 guest_warp_size = 32);

    [[nodiscard]] GraphicsPipelineLibraryStats Stats() const noexcept;

private:
    struct ByteKey {
        u64 hash{};
        std::vector<u8> bytes;

        bool operator==(const ByteKey& rhs) const noexcept {
            return hash == rhs.hash && bytes == rhs.bytes;
        }
    };

    struct ByteKeyHash {
        size_t operator()(const ByteKey& k) const noexcept {
            return static_cast<size_t>(k.hash);
        }
    };

    struct LayoutKey {
        std::array<u64, 6> unique_hashes{};
        u64 layout_digest{};

        bool operator==(const LayoutKey& rhs) const noexcept = default;
    };

    struct LayoutKeyHash {
        size_t operator()(const LayoutKey& k) const noexcept;
    };

    struct CachedLibrary {
        vk::Pipeline pipeline;
        // Keeps the VkPipelineLayout alive for shader libraries until shutdown.
        std::shared_ptr<const SharedPipelineLayoutBundle> layout_owner;
    };

    VkPipeline GetOrCreateVertexInput(const ByteKey& key,
                                      const GraphicsPipelineLibraryInput& input,
                                      std::span<const VkDynamicState> vi_dynamic,
                                      VkPipelineCreateFlags lib_flags, bool& out_hit);

    VkPipeline GetOrCreateShaderLibrary(const ByteKey& key,
                                        const GraphicsPipelineLibraryInput& input,
                                        std::span<const VkDynamicState> shader_dynamic,
                                        VkPipelineCreateFlags lib_flags, bool& out_hit);

    VkPipeline GetOrCreateFragmentOutput(const ByteKey& key,
                                         const GraphicsPipelineLibraryInput& input,
                                         std::span<const VkDynamicState> fo_dynamic,
                                         VkPipelineCreateFlags lib_flags, bool& out_hit);

    const Device& device;
    vk::PipelineCache& pipeline_cache;
    bool supported{};
    bool fast_linking{};

    mutable std::mutex mutex;
    Common::unordered_map<LayoutKey, std::shared_ptr<const SharedPipelineLayoutBundle>, LayoutKeyHash>
        layout_cache;
    Common::unordered_map<ByteKey, CachedLibrary, ByteKeyHash> vertex_input_cache;
    Common::unordered_map<ByteKey, CachedLibrary, ByteKeyHash> shader_cache;
    Common::unordered_map<ByteKey, CachedLibrary, ByteKeyHash> fragment_output_cache;

    std::atomic<size_t> vertex_input_hits{};
    std::atomic<size_t> shader_hits{};
    std::atomic<size_t> fragment_output_hits{};
    std::atomic<size_t> linked_pipelines{};
    std::atomic<size_t> fallback_monolithic{};
};

} // namespace Vulkan

