// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/container/unordered_map.h"

#include <boost/container/static_vector.hpp>

#include "video_core/renderer_vulkan/maxwell_to_vk.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {
namespace {
using VideoCore::Surface::PixelFormat;
using VideoCore::Surface::SurfaceType;

VkAttachmentDescription AttachmentDescription(const Device& device, PixelFormat format,
                                              VkSampleCountFlagBits samples,
                                              VkAttachmentLoadOp load_op) {
    using MaxwellToVK::SurfaceFormat;

    const SurfaceType surface_type = VideoCore::Surface::GetFormatType(format);
    const bool has_stencil = surface_type == SurfaceType::DepthStencil ||
                             surface_type == SurfaceType::Stencil;

    return {
        .flags = {},
        .format = SurfaceFormat(device, FormatType::Optimal, true, format).format,
        .samples = samples,
        .loadOp = load_op,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = has_stencil ? load_op : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp =
            has_stencil ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_GENERAL,
        .finalLayout = VK_IMAGE_LAYOUT_GENERAL,
    };
}

struct ResolveModes {
    VkResolveModeFlagBits depth;
    VkResolveModeFlagBits stencil;
};

ResolveModes SampleZeroModes(PixelFormat format) {
    const SurfaceType type = VideoCore::Surface::GetFormatType(format);
    return {
        .depth = type == SurfaceType::Depth || type == SurfaceType::DepthStencil
                     ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE,
        .stencil = type == SurfaceType::Stencil || type == SurfaceType::DepthStencil
                       ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE,
    };
}
} // Anonymous namespace

bool SupportsDepthStencilResolve(const Device& device, PixelFormat format) {
    if (format == PixelFormat::Invalid || !device.IsKhrDepthStencilResolveSupported()) {
        return false;
    }
    const ResolveModes modes = SampleZeroModes(format);
    return (modes.depth != VK_RESOLVE_MODE_NONE || modes.stencil != VK_RESOLVE_MODE_NONE) &&
           (device.GetDepthResolveModes() & modes.depth) == modes.depth &&
           (device.GetStencilResolveModes() & modes.stencil) == modes.stencil;
}

RenderPassCache::RenderPassCache(const Device& device_) : device{&device_} {}

VkRenderPass RenderPassCache::Get(const RenderPassKey& key) {
    std::scoped_lock lock{mutex};
    if (const auto it = cache.find(key); it != cache.end()) {
        return *it->second;
    }
    static constexpr size_t MAX_ATTACHMENTS =
        std::tuple_size_v<decltype(RenderPassKey::color_formats)> + 2;
    boost::container::static_vector<VkAttachmentDescription, MAX_ATTACHMENTS> descriptions;
    std::array<VkAttachmentReference, 8> references{};
    u32 num_attachments{};
    u32 num_colors{};
    for (size_t index = 0; index < key.color_formats.size(); ++index) {
        const PixelFormat format{key.color_formats[index]};
        const bool is_valid{format != PixelFormat::Invalid};
        references[index] = VkAttachmentReference{
            .attachment = is_valid ? num_colors : VK_ATTACHMENT_UNUSED,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        };
        if (is_valid) {
            const VkAttachmentLoadOp load_op = (key.color_clear_mask & (1u << index)) != 0
                                                   ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                   : VK_ATTACHMENT_LOAD_OP_LOAD;
            descriptions.push_back(AttachmentDescription(*device, format, key.samples, load_op));
            num_attachments = static_cast<u32>(index + 1);
            ++num_colors;
        }
    }
    const bool has_depth{key.depth_format != PixelFormat::Invalid};
    VkAttachmentReference depth_reference{};
    if (has_depth) {
        depth_reference = VkAttachmentReference{
            .attachment = num_colors,
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkAttachmentLoadOp depth_load_op = key.depth_stencil_clear
                                                     ? VK_ATTACHMENT_LOAD_OP_CLEAR
                                                     : VK_ATTACHMENT_LOAD_OP_LOAD;
        descriptions.push_back(AttachmentDescription(*device, key.depth_format, key.samples, depth_load_op));
    }
    const bool do_resolve_depth_stencil = key.resolve_depth_stencil && has_depth &&
                                          key.samples != VK_SAMPLE_COUNT_1_BIT &&
                                          SupportsDepthStencilResolve(*device, key.depth_format);
    VkAttachmentReference depth_resolve_reference{};
    if (do_resolve_depth_stencil) {
        depth_resolve_reference = VkAttachmentReference{
            .attachment = static_cast<u32>(descriptions.size()),
            .layout = VK_IMAGE_LAYOUT_GENERAL,
        };
        const VkAttachmentDescription resolve_desc =
            AttachmentDescription(*device, key.depth_format, VK_SAMPLE_COUNT_1_BIT,
                                  VK_ATTACHMENT_LOAD_OP_DONT_CARE);
        descriptions.push_back(resolve_desc);
    }
    const VkSubpassDescription subpass{
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = num_attachments,
        .pColorAttachments = references.data(),
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = has_depth ? &depth_reference : nullptr,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };
    const VkSubpassDependency dependency{
            .srcSubpass = 0,  // Current subpass
            .dstSubpass = 0,  // Same subpass (self-dependency)
            .srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                            VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                            VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT
    };

    if (device->IsKhrCreateRenderPass2Supported()) {
        boost::container::static_vector<VkAttachmentDescription2, MAX_ATTACHMENTS> descriptions2;
        for (const VkAttachmentDescription& description : descriptions) {
            descriptions2.push_back(VkAttachmentDescription2{
                .sType = VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2,
                .pNext = nullptr,
                .flags = description.flags,
                .format = description.format,
                .samples = description.samples,
                .loadOp = description.loadOp,
                .storeOp = description.storeOp,
                .stencilLoadOp = description.stencilLoadOp,
                .stencilStoreOp = description.stencilStoreOp,
                .initialLayout = description.initialLayout,
                .finalLayout = description.finalLayout,
            });
        }
        const auto promote = [](const VkAttachmentReference& reference) {
            return VkAttachmentReference2{
                .sType = VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2,
                .pNext = nullptr,
                .attachment = reference.attachment,
                .layout = reference.layout,
                .aspectMask = 0,
            };
        };
        std::array<VkAttachmentReference2, 8> references2{};
        for (size_t index = 0; index < references.size(); ++index) {
            references2[index] = promote(references[index]);
        }
        const VkAttachmentReference2 depth_reference2 = promote(depth_reference);
        const VkAttachmentReference2 depth_resolve_reference2 = promote(depth_resolve_reference);
        const ResolveModes resolve_modes = do_resolve_depth_stencil
                                               ? SampleZeroModes(key.depth_format)
                                               : ResolveModes{};
        const VkSubpassDescriptionDepthStencilResolve depth_stencil_resolve{
            .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE,
            .pNext = nullptr,
            .depthResolveMode = resolve_modes.depth,
            .stencilResolveMode = resolve_modes.stencil,
            .pDepthStencilResolveAttachment = &depth_resolve_reference2,
        };
        const VkSubpassDescription2 subpass2{
            .sType = VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2,
            .pNext = do_resolve_depth_stencil ? &depth_stencil_resolve : nullptr,
            .flags = 0,
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .viewMask = 0,
            .inputAttachmentCount = 0,
            .pInputAttachments = nullptr,
            .colorAttachmentCount = num_attachments,
            .pColorAttachments = references2.data(),
            .pResolveAttachments = nullptr,
            .pDepthStencilAttachment = has_depth ? &depth_reference2 : nullptr,
            .preserveAttachmentCount = 0,
            .pPreserveAttachments = nullptr,
        };
        const VkSubpassDependency2 dependency2{
            .sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2,
            .pNext = nullptr,
            .srcSubpass = dependency.srcSubpass,
            .dstSubpass = dependency.dstSubpass,
            .srcStageMask = dependency.srcStageMask,
            .dstStageMask = dependency.dstStageMask,
            .srcAccessMask = dependency.srcAccessMask,
            .dstAccessMask = dependency.dstAccessMask,
            .dependencyFlags = dependency.dependencyFlags,
            .viewOffset = 0,
        };
        auto pass = device->GetLogical().CreateRenderPass2({
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2,
            .pNext = nullptr,
            .flags = 0,
            .attachmentCount = static_cast<u32>(descriptions2.size()),
            .pAttachments = descriptions2.empty() ? nullptr : descriptions2.data(),
            .subpassCount = 1,
            .pSubpasses = &subpass2,
            .dependencyCount = 1,
            .pDependencies = &dependency2,
            .correlatedViewMaskCount = 0,
            .pCorrelatedViewMasks = nullptr,
        });
        return *cache.emplace(key, std::move(pass)).first->second;
    }

    auto pass = device->GetLogical().CreateRenderPass({
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = static_cast<u32>(descriptions.size()),
        .pAttachments = descriptions.empty() ? nullptr : descriptions.data(),
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 1,
        .pDependencies = &dependency,
    });
    return *cache.emplace(key, std::move(pass)).first->second;
}

} // namespace Vulkan
