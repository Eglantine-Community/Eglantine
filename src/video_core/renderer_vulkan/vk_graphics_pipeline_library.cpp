// SPDX-FileCopyrightText: Copyright 2026 Dalia Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include "common/cityhash.h"
#include "common/logging.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline_library.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {

namespace {

template <typename T>
void AppendPod(std::vector<u8>& dst, const T& value) {
    static_assert(std::is_trivially_copyable_v<T>);
    const size_t offset = dst.size();
    dst.resize(offset + sizeof(T));
    std::memcpy(dst.data() + offset, &value, sizeof(T));
}

template <typename T>
void AppendSpan(std::vector<u8>& dst, std::span<const T> values) {
    static_assert(std::is_trivially_copyable_v<T>);
    const u32 count = static_cast<u32>(values.size());
    AppendPod(dst, count);
    if (count > 0) {
        const size_t offset = dst.size();
        const size_t byte_size = sizeof(T) * count;
        dst.resize(offset + byte_size);
        std::memcpy(dst.data() + offset, values.data(), byte_size);
    }
}

void FinalizeKeyHash(std::vector<u8>& bytes, u64& out_hash) {
    out_hash = Common::CityHash64(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

bool IsVertexInputDynamicState(VkDynamicState state) noexcept {
    switch (state) {
    case VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY:
    case VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE:
    case VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE:
    case VK_DYNAMIC_STATE_VERTEX_INPUT_EXT:
        return true;
    default:
        return false;
    }
}

bool IsFragmentOutputDynamicState(VkDynamicState state) noexcept {
    switch (state) {
    case VK_DYNAMIC_STATE_BLEND_CONSTANTS:
    case VK_DYNAMIC_STATE_LOGIC_OP_EXT:
    case VK_DYNAMIC_STATE_COLOR_WRITE_ENABLE_EXT:
    case VK_DYNAMIC_STATE_LOGIC_OP_ENABLE_EXT:
    case VK_DYNAMIC_STATE_COLOR_BLEND_ENABLE_EXT:
    case VK_DYNAMIC_STATE_COLOR_BLEND_EQUATION_EXT:
    case VK_DYNAMIC_STATE_COLOR_WRITE_MASK_EXT:
    case VK_DYNAMIC_STATE_ALPHA_TO_COVERAGE_ENABLE_EXT:
    case VK_DYNAMIC_STATE_ALPHA_TO_ONE_ENABLE_EXT:
        return true;
    default:
        return false;
    }
}

template <typename T>
T ZeroPNextCopy(const T& src) noexcept {
    T copy = src;
    copy.pNext = nullptr;
    return copy;
}

} // Anonymous namespace

size_t GraphicsPipelineLibraryCache::LayoutKeyHash::operator()(const LayoutKey& k) const noexcept {
    std::array<u64, 7> words{};
    std::ranges::copy(k.unique_hashes, words.begin());
    words[6] = k.layout_digest;
    return static_cast<size_t>(
        Common::CityHash64(reinterpret_cast<const char*>(words.data()), sizeof(words)));
}

GraphicsPipelineLibraryCache::GraphicsPipelineLibraryCache(const Device& device_,
                                                           vk::PipelineCache& pipeline_cache_)
    : device{device_}, pipeline_cache{pipeline_cache_},
      supported{device.IsExtGraphicsPipelineLibrarySupported()},
      fast_linking{device.IsExtGraphicsPipelineLibraryFastLinkingSupported()} {
    if (supported) {
        LOG_INFO(Render_Vulkan,
                 "VK_EXT_graphics_pipeline_library enabled (fast_linking={}, hw_fast_linking={})",
                 fast_linking, device.IsExtGraphicsPipelineLibraryFastLinkingSupported());
    }
}

GraphicsPipelineLibraryCache::~GraphicsPipelineLibraryCache() = default;

bool GraphicsPipelineLibraryCache::IsSupported() const noexcept {
    return supported;
}

bool GraphicsPipelineLibraryCache::IsFastLinkingSupported() const noexcept {
    return fast_linking;
}

std::shared_ptr<const SharedPipelineLayoutBundle> GraphicsPipelineLibraryCache::GetOrCreateLayout(
    const std::array<u64, 6>& unique_hashes, u64 layout_digest,
    const std::function<std::shared_ptr<SharedPipelineLayoutBundle>()>& creator) {
    const LayoutKey key{
        .unique_hashes = unique_hashes,
        .layout_digest = layout_digest,
    };
    {
        std::scoped_lock lk{mutex};
        if (const auto it = layout_cache.find(key); it != layout_cache.end()) {
            return it->second;
        }
    }
    auto created = creator();
    if (!created) {
        return nullptr;
    }
    std::scoped_lock lk{mutex};
    const auto [it, inserted] = layout_cache.emplace(key, std::move(created));
    return it->second;
}

vk::Pipeline GraphicsPipelineLibraryCache::BuildLinkedPipeline(
    const GraphicsPipelineLibraryInput& input) {
    if (!supported || !input.layout_bundle || !input.layout_bundle->pipeline_layout) {
        return {};
    }
    if (!input.vertex_input_ci || !input.input_assembly_ci || !input.tessellation_ci ||
        !input.viewport_ci || !input.rasterization_ci || !input.multisample_ci ||
        !input.depth_stencil_ci || !input.color_blend_ci || input.shader_stages.empty()) {
        return {};
    }

    std::vector<VkDynamicState> vi_dynamic;
    std::vector<VkDynamicState> shader_dynamic;
    std::vector<VkDynamicState> fo_dynamic;
    vi_dynamic.reserve(4);
    shader_dynamic.reserve(input.dynamic_states.size());
    fo_dynamic.reserve(10);
    for (const VkDynamicState state : input.dynamic_states) {
        if (IsVertexInputDynamicState(state)) {
            vi_dynamic.push_back(state);
        } else if (IsFragmentOutputDynamicState(state)) {
            fo_dynamic.push_back(state);
        } else {
            shader_dynamic.push_back(state);
        }
    }

    VkPipelineCreateFlags link_flags = input.base_flags;
    VkPipelineCreateFlags lib_flags = input.base_flags | VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    if (!fast_linking) {
        lib_flags |= VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
        link_flags |= VK_PIPELINE_CREATE_LINK_TIME_OPTIMIZATION_BIT_EXT;
    }

    // 1. Vertex Input Interface key
    ByteKey vi_key;
    AppendPod(vi_key.bytes, lib_flags);
    AppendSpan(vi_key.bytes, std::span<const VkDynamicState>{vi_dynamic});
    AppendPod(vi_key.bytes, input.vertex_input_ci->flags);
    AppendSpan(vi_key.bytes,
               std::span{input.vertex_input_ci->pVertexBindingDescriptions,
                         input.vertex_input_ci->vertexBindingDescriptionCount});
    AppendSpan(vi_key.bytes,
               std::span{input.vertex_input_ci->pVertexAttributeDescriptions,
                         input.vertex_input_ci->vertexAttributeDescriptionCount});
    if (input.input_divisor_ci) {
        AppendPod(vi_key.bytes, u8{1});
        AppendSpan(vi_key.bytes,
                   std::span{input.input_divisor_ci->pVertexBindingDivisors,
                             input.input_divisor_ci->vertexBindingDivisorCount});
    } else {
        AppendPod(vi_key.bytes, u8{0});
    }
    AppendPod(vi_key.bytes, ZeroPNextCopy(*input.input_assembly_ci));
    FinalizeKeyHash(vi_key.bytes, vi_key.hash);

    // 2. Shader (Pre-Rasterization + Fragment Shader) key
    ByteKey shader_key;
    AppendPod(shader_key.bytes, lib_flags);
    AppendPod(shader_key.bytes, input.unique_hashes);
    const u64 layout_ptr =
        reinterpret_cast<u64>(static_cast<VkPipelineLayout>(*input.layout_bundle->pipeline_layout));
    const u64 rp_ptr = reinterpret_cast<u64>(input.render_pass);
    AppendPod(shader_key.bytes, layout_ptr);
    AppendPod(shader_key.bytes, rp_ptr);
    AppendSpan(shader_key.bytes, std::span<const VkDynamicState>{shader_dynamic});
    AppendPod(shader_key.bytes, ZeroPNextCopy(*input.tessellation_ci));
    {
        auto vp = ZeroPNextCopy(*input.viewport_ci);
        vp.pViewports = nullptr;
        vp.pScissors = nullptr;
        AppendPod(shader_key.bytes, vp);
        if (vp.viewportCount > 0 && input.viewport_ci->pViewports) {
            AppendSpan(shader_key.bytes,
                       std::span{input.viewport_ci->pViewports, vp.viewportCount});
        }
        if (vp.scissorCount > 0 && input.viewport_ci->pScissors) {
            AppendSpan(shader_key.bytes,
                       std::span{input.viewport_ci->pScissors, vp.scissorCount});
        }
    }
    if (input.swizzle_ci) {
        AppendPod(shader_key.bytes, u8{1});
        AppendSpan(shader_key.bytes,
                   std::span{input.swizzle_ci->pViewportSwizzles, input.swizzle_ci->viewportCount});
    } else {
        AppendPod(shader_key.bytes, u8{0});
    }
    if (input.ndc_info) {
        AppendPod(shader_key.bytes, u8{1});
        AppendPod(shader_key.bytes, input.ndc_info->negativeOneToOne);
    } else {
        AppendPod(shader_key.bytes, u8{0});
    }
    AppendPod(shader_key.bytes, ZeroPNextCopy(*input.rasterization_ci));
    if (input.line_state_ci) {
        AppendPod(shader_key.bytes, u8{1});
        AppendPod(shader_key.bytes, ZeroPNextCopy(*input.line_state_ci));
    } else {
        AppendPod(shader_key.bytes, u8{0});
    }
    if (input.conservative_raster_ci) {
        AppendPod(shader_key.bytes, u8{1});
        AppendPod(shader_key.bytes, ZeroPNextCopy(*input.conservative_raster_ci));
    } else {
        AppendPod(shader_key.bytes, u8{0});
    }
    if (input.provoking_vertex_ci) {
        AppendPod(shader_key.bytes, u8{1});
        AppendPod(shader_key.bytes, input.provoking_vertex_ci->provokingVertexMode);
    } else {
        AppendPod(shader_key.bytes, u8{0});
    }
    AppendPod(shader_key.bytes, input.multisample_ci->rasterizationSamples);
    AppendPod(shader_key.bytes, input.multisample_ci->sampleShadingEnable);
    AppendPod(shader_key.bytes, ZeroPNextCopy(*input.depth_stencil_ci));
    FinalizeKeyHash(shader_key.bytes, shader_key.hash);

    // 3. Fragment Output Interface key
    ByteKey fo_key;
    AppendPod(fo_key.bytes, lib_flags);
    AppendPod(fo_key.bytes, rp_ptr);
    AppendSpan(fo_key.bytes, std::span<const VkDynamicState>{fo_dynamic});
    {
        auto ms = ZeroPNextCopy(*input.multisample_ci);
        ms.pSampleMask = nullptr;
        AppendPod(fo_key.bytes, ms);
    }
    {
        auto cb = ZeroPNextCopy(*input.color_blend_ci);
        cb.pAttachments = nullptr;
        AppendPod(fo_key.bytes, cb);
        if (input.color_blend_ci->attachmentCount > 0 && input.color_blend_ci->pAttachments) {
            AppendSpan(fo_key.bytes,
                       std::span{input.color_blend_ci->pAttachments,
                                 input.color_blend_ci->attachmentCount});
        }
    }
    FinalizeKeyHash(fo_key.bytes, fo_key.hash);

    bool vi_hit = false;
    bool shader_hit = false;
    bool fo_hit = false;
    const VkPipeline vi_lib = GetOrCreateVertexInput(vi_key, input, vi_dynamic, lib_flags, vi_hit);
    const VkPipeline shader_lib =
        GetOrCreateShaderLibrary(shader_key, input, shader_dynamic, lib_flags, shader_hit);
    const VkPipeline fo_lib =
        GetOrCreateFragmentOutput(fo_key, input, fo_dynamic, lib_flags, fo_hit);

    if (!vi_lib || !shader_lib || !fo_lib) {
        fallback_monolithic.fetch_add(1, std::memory_order_relaxed);
        return {};
    }

    if (vi_hit) {
        vertex_input_hits.fetch_add(1, std::memory_order_relaxed);
    }
    if (shader_hit) {
        shader_hits.fetch_add(1, std::memory_order_relaxed);
    }
    if (fo_hit) {
        fragment_output_hits.fetch_add(1, std::memory_order_relaxed);
    }

    const std::array<VkPipeline, 3> libs{vi_lib, shader_lib, fo_lib};
    const VkPipelineLibraryCreateInfoKHR lib_link_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LIBRARY_CREATE_INFO_KHR,
        .pNext = nullptr,
        .libraryCount = static_cast<u32>(libs.size()),
        .pLibraries = libs.data(),
    };
    const VkGraphicsPipelineCreateInfo link_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &lib_link_ci,
        .flags = link_flags,
        .stageCount = 0,
        .pStages = nullptr,
        .pVertexInputState = nullptr,
        .pInputAssemblyState = nullptr,
        .pTessellationState = nullptr,
        .pViewportState = nullptr,
        .pRasterizationState = nullptr,
        .pMultisampleState = nullptr,
        .pDepthStencilState = nullptr,
        .pColorBlendState = nullptr,
        .pDynamicState = nullptr,
        .layout = *input.layout_bundle->pipeline_layout,
        .renderPass = VK_NULL_HANDLE,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };

    vk::Pipeline linked = device.GetLogical().TryCreateGraphicsPipeline(link_ci, *pipeline_cache);
    if (!linked) {
        fallback_monolithic.fetch_add(1, std::memory_order_relaxed);
        return {};
    }
    linked_pipelines.fetch_add(1, std::memory_order_relaxed);
    return linked;
}

VkPipeline GraphicsPipelineLibraryCache::GetOrCreateVertexInput(
    const ByteKey& key, const GraphicsPipelineLibraryInput& input,
    std::span<const VkDynamicState> vi_dynamic, VkPipelineCreateFlags lib_flags, bool& out_hit) {
    {
        std::scoped_lock lk{mutex};
        if (const auto it = vertex_input_cache.find(key); it != vertex_input_cache.end()) {
            out_hit = true;
            return *it->second.pipeline;
        }
    }
    out_hit = false;

    VkPipelineVertexInputDivisorStateCreateInfoEXT divisor_copy{};
    VkPipelineVertexInputStateCreateInfo vi_copy = ZeroPNextCopy(*input.vertex_input_ci);
    if (input.input_divisor_ci) {
        divisor_copy = ZeroPNextCopy(*input.input_divisor_ci);
        vi_copy.pNext = &divisor_copy;
    }
    const VkPipelineInputAssemblyStateCreateInfo ia_copy = ZeroPNextCopy(*input.input_assembly_ci);
    const VkPipelineDynamicStateCreateInfo dyn_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<u32>(vi_dynamic.size()),
        .pDynamicStates = vi_dynamic.empty() ? nullptr : vi_dynamic.data(),
    };
    const VkGraphicsPipelineLibraryCreateInfoEXT gpl_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_VERTEX_INPUT_INTERFACE_BIT_EXT,
    };
    const VkGraphicsPipelineCreateInfo pipe_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &gpl_ci,
        .flags = lib_flags,
        .stageCount = 0,
        .pStages = nullptr,
        .pVertexInputState = &vi_copy,
        .pInputAssemblyState = &ia_copy,
        .pTessellationState = nullptr,
        .pViewportState = nullptr,
        .pRasterizationState = nullptr,
        .pMultisampleState = nullptr,
        .pDepthStencilState = nullptr,
        .pColorBlendState = nullptr,
        .pDynamicState = vi_dynamic.empty() ? nullptr : &dyn_ci,
        .layout = VK_NULL_HANDLE,
        .renderPass = VK_NULL_HANDLE,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };

    vk::Pipeline created = device.GetLogical().TryCreateGraphicsPipeline(pipe_ci, *pipeline_cache);
    if (!created) {
        return VK_NULL_HANDLE;
    }
    std::scoped_lock lk{mutex};
    const auto [it, inserted] =
        vertex_input_cache.emplace(key, CachedLibrary{std::move(created), nullptr});
    return *it->second.pipeline;
}

VkPipeline GraphicsPipelineLibraryCache::GetOrCreateShaderLibrary(
    const ByteKey& key, const GraphicsPipelineLibraryInput& input,
    std::span<const VkDynamicState> shader_dynamic, VkPipelineCreateFlags lib_flags,
    bool& out_hit) {
    {
        std::scoped_lock lk{mutex};
        if (const auto it = shader_cache.find(key); it != shader_cache.end()) {
            out_hit = true;
            return *it->second.pipeline;
        }
    }
    out_hit = false;

    const VkPipelineTessellationStateCreateInfo tess_copy = ZeroPNextCopy(*input.tessellation_ci);

    VkPipelineViewportSwizzleStateCreateInfoNV swizzle_copy{};
    VkPipelineViewportDepthClipControlCreateInfoEXT ndc_copy{};
    VkPipelineViewportStateCreateInfo vp_copy = ZeroPNextCopy(*input.viewport_ci);
    void** vp_next = const_cast<void**>(&vp_copy.pNext);
    if (input.swizzle_ci) {
        swizzle_copy = ZeroPNextCopy(*input.swizzle_ci);
        *vp_next = &swizzle_copy;
        vp_next = const_cast<void**>(&swizzle_copy.pNext);
    }
    if (input.ndc_info) {
        ndc_copy = ZeroPNextCopy(*input.ndc_info);
        *vp_next = &ndc_copy;
    }

    VkPipelineRasterizationLineStateCreateInfoEXT line_copy{};
    VkPipelineRasterizationConservativeStateCreateInfoEXT cons_copy{};
    VkPipelineRasterizationProvokingVertexStateCreateInfoEXT pv_copy{};
    VkPipelineRasterizationStateCreateInfo rast_copy = ZeroPNextCopy(*input.rasterization_ci);
    void** rast_next = const_cast<void**>(&rast_copy.pNext);
    if (input.line_state_ci) {
        line_copy = ZeroPNextCopy(*input.line_state_ci);
        *rast_next = &line_copy;
        rast_next = const_cast<void**>(&line_copy.pNext);
    }
    if (input.conservative_raster_ci) {
        cons_copy = ZeroPNextCopy(*input.conservative_raster_ci);
        *rast_next = &cons_copy;
        rast_next = const_cast<void**>(&cons_copy.pNext);
    }
    if (input.provoking_vertex_ci) {
        pv_copy = ZeroPNextCopy(*input.provoking_vertex_ci);
        *rast_next = &pv_copy;
    }

    const VkPipelineMultisampleStateCreateInfo ms_copy = ZeroPNextCopy(*input.multisample_ci);
    const VkPipelineDepthStencilStateCreateInfo ds_copy = ZeroPNextCopy(*input.depth_stencil_ci);
    const VkPipelineDynamicStateCreateInfo dyn_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<u32>(shader_dynamic.size()),
        .pDynamicStates = shader_dynamic.empty() ? nullptr : shader_dynamic.data(),
    };
    const VkGraphicsPipelineLibraryCreateInfoEXT gpl_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT |
                 VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
    };
    const VkGraphicsPipelineCreateInfo pipe_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &gpl_ci,
        .flags = lib_flags,
        .stageCount = static_cast<u32>(input.shader_stages.size()),
        .pStages = input.shader_stages.data(),
        .pVertexInputState = nullptr,
        .pInputAssemblyState = nullptr,
        .pTessellationState = &tess_copy,
        .pViewportState = &vp_copy,
        .pRasterizationState = &rast_copy,
        .pMultisampleState = &ms_copy,
        .pDepthStencilState = &ds_copy,
        .pColorBlendState = nullptr,
        .pDynamicState = shader_dynamic.empty() ? nullptr : &dyn_ci,
        .layout = *input.layout_bundle->pipeline_layout,
        .renderPass = input.render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };

    vk::Pipeline created = device.GetLogical().TryCreateGraphicsPipeline(pipe_ci, *pipeline_cache);
    if (!created) {
        return VK_NULL_HANDLE;
    }
    std::scoped_lock lk{mutex};
    const auto [it, inserted] =
        shader_cache.emplace(key, CachedLibrary{std::move(created), input.layout_bundle});
    return *it->second.pipeline;
}

VkPipeline GraphicsPipelineLibraryCache::GetOrCreateFragmentOutput(
    const ByteKey& key, const GraphicsPipelineLibraryInput& input,
    std::span<const VkDynamicState> fo_dynamic, VkPipelineCreateFlags lib_flags, bool& out_hit) {
    {
        std::scoped_lock lk{mutex};
        if (const auto it = fragment_output_cache.find(key); it != fragment_output_cache.end()) {
            out_hit = true;
            return *it->second.pipeline;
        }
    }
    out_hit = false;

    const VkPipelineMultisampleStateCreateInfo ms_copy = ZeroPNextCopy(*input.multisample_ci);
    const VkPipelineColorBlendStateCreateInfo cb_copy = ZeroPNextCopy(*input.color_blend_ci);
    const VkPipelineDynamicStateCreateInfo dyn_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<u32>(fo_dynamic.size()),
        .pDynamicStates = fo_dynamic.empty() ? nullptr : fo_dynamic.data(),
    };
    const VkGraphicsPipelineLibraryCreateInfoEXT gpl_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_OUTPUT_INTERFACE_BIT_EXT,
    };
    const VkGraphicsPipelineCreateInfo pipe_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &gpl_ci,
        .flags = lib_flags,
        .stageCount = 0,
        .pStages = nullptr,
        .pVertexInputState = nullptr,
        .pInputAssemblyState = nullptr,
        .pTessellationState = nullptr,
        .pViewportState = nullptr,
        .pRasterizationState = nullptr,
        .pMultisampleState = &ms_copy,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &cb_copy,
        .pDynamicState = fo_dynamic.empty() ? nullptr : &dyn_ci,
        .layout = VK_NULL_HANDLE,
        .renderPass = input.render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };

    vk::Pipeline created = device.GetLogical().TryCreateGraphicsPipeline(pipe_ci, *pipeline_cache);
    if (!created) {
        return VK_NULL_HANDLE;
    }
    std::scoped_lock lk{mutex};
    const auto [it, inserted] =
        fragment_output_cache.emplace(key, CachedLibrary{std::move(created), nullptr});
    return *it->second.pipeline;
}

bool GraphicsPipelineLibraryCache::PrewarmShaderStage(VkShaderStageFlagBits stage,
                                                        VkShaderModule module,
                                                        VkPipelineLayout layout,
                                                        VkRenderPass compatible_rp,
                                                        bool required_subgroup_size,
                                                        u32 guest_warp_size) {
    if (!supported || !module || !layout || !compatible_rp) {
        return false;
    }

    const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT subgroup_size_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
        .pNext = nullptr,
        .requiredSubgroupSize = guest_warp_size,
    };
    const VkPipelineShaderStageCreateInfo stage_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
        .pNext = required_subgroup_size ? &subgroup_size_ci : nullptr,
        .flags = 0,
        .stage = stage,
        .module = module,
        .pName = "main",
        .pSpecializationInfo = nullptr,
    };

    VkPipelineCreateFlags lib_flags = VK_PIPELINE_CREATE_LIBRARY_BIT_KHR;
    if (!fast_linking) {
        lib_flags |= VK_PIPELINE_CREATE_RETAIN_LINK_TIME_OPTIMIZATION_INFO_BIT_EXT;
    }

    if (stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
        const VkPipelineMultisampleStateCreateInfo ms_ci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = VK_FALSE,
            .minSampleShading = 0.0f,
            .pSampleMask = nullptr,
            .alphaToCoverageEnable = VK_FALSE,
            .alphaToOneEnable = VK_FALSE,
        };
        const VkPipelineDepthStencilStateCreateInfo ds_ci{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .depthTestEnable = VK_FALSE,
            .depthWriteEnable = VK_FALSE,
            .depthCompareOp = VK_COMPARE_OP_ALWAYS,
            .depthBoundsTestEnable = VK_FALSE,
            .stencilTestEnable = VK_FALSE,
            .front = {},
            .back = {},
            .minDepthBounds = 0.0f,
            .maxDepthBounds = 1.0f,
        };
        const VkGraphicsPipelineLibraryCreateInfoEXT gpl_ci{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = VK_GRAPHICS_PIPELINE_LIBRARY_FRAGMENT_SHADER_BIT_EXT,
        };
        const VkGraphicsPipelineCreateInfo pipe_ci{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = &gpl_ci,
            .flags = lib_flags,
            .stageCount = 1,
            .pStages = &stage_ci,
            .pVertexInputState = nullptr,
            .pInputAssemblyState = nullptr,
            .pTessellationState = nullptr,
            .pViewportState = nullptr,
            .pRasterizationState = nullptr,
            .pMultisampleState = &ms_ci,
            .pDepthStencilState = &ds_ci,
            .pColorBlendState = nullptr,
            .pDynamicState = nullptr,
            .layout = layout,
            .renderPass = compatible_rp,
            .subpass = 0,
            .basePipelineHandle = VK_NULL_HANDLE,
            .basePipelineIndex = 0,
        };
        return static_cast<bool>(
            device.GetLogical().TryCreateGraphicsPipeline(pipe_ci, *pipeline_cache));
    }

    const std::array<VkDynamicState, 2> dyn_states{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    const VkPipelineDynamicStateCreateInfo dyn_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = static_cast<u32>(dyn_states.size()),
        .pDynamicStates = dyn_states.data(),
    };
    const VkPipelineViewportStateCreateInfo vp_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = nullptr,
        .scissorCount = 1,
        .pScissors = nullptr,
    };
    const VkPipelineRasterizationStateCreateInfo rast_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f,
    };
    const VkGraphicsPipelineLibraryCreateInfoEXT gpl_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_LIBRARY_CREATE_INFO_EXT,
        .pNext = nullptr,
        .flags = VK_GRAPHICS_PIPELINE_LIBRARY_PRE_RASTERIZATION_SHADERS_BIT_EXT,
    };
    const VkGraphicsPipelineCreateInfo pipe_ci{
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = &gpl_ci,
        .flags = lib_flags,
        .stageCount = 1,
        .pStages = &stage_ci,
        .pVertexInputState = nullptr,
        .pInputAssemblyState = nullptr,
        .pTessellationState = nullptr,
        .pViewportState = &vp_ci,
        .pRasterizationState = &rast_ci,
        .pMultisampleState = nullptr,
        .pDepthStencilState = nullptr,
        .pColorBlendState = nullptr,
        .pDynamicState = &dyn_ci,
        .layout = layout,
        .renderPass = compatible_rp,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = 0,
    };
    return static_cast<bool>(
        device.GetLogical().TryCreateGraphicsPipeline(pipe_ci, *pipeline_cache));
}

GraphicsPipelineLibraryStats GraphicsPipelineLibraryCache::Stats() const noexcept {
    std::scoped_lock lk{mutex};
    return GraphicsPipelineLibraryStats{
        .vertex_input_libraries = vertex_input_cache.size(),
        .shader_libraries = shader_cache.size(),
        .fragment_output_libraries = fragment_output_cache.size(),
        .layout_bundles = layout_cache.size(),
        .vertex_input_hits = vertex_input_hits.load(std::memory_order_relaxed),
        .shader_hits = shader_hits.load(std::memory_order_relaxed),
        .fragment_output_hits = fragment_output_hits.load(std::memory_order_relaxed),
        .linked_pipelines = linked_pipelines.load(std::memory_order_relaxed),
        .fallback_monolithic = fallback_monolithic.load(std::memory_order_relaxed),
    };
}

} // namespace Vulkan
