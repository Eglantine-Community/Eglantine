// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 Torzu Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2024 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <vulkan/vulkan_core.h>
#include "common/assert.h"
#include "common/common_types.h"

#include "video_core/host_shaders/present_area_frag_spv.h"
#include "video_core/host_shaders/present_bicubic_frag_spv.h"
#include "video_core/host_shaders/present_gaussian_frag_spv.h"
#include "video_core/host_shaders/present_lanczos_frag_spv.h"
#include "video_core/host_shaders/present_spline1_frag_spv.h"
#include "video_core/host_shaders/present_mitchell_frag_spv.h"
#include "video_core/host_shaders/present_bspline_frag_spv.h"
#include "video_core/host_shaders/present_zero_tangent_frag_spv.h"
#include "video_core/host_shaders/present_mmpx_frag_spv.h"
#include "video_core/host_shaders/vulkan_present_frag_spv.h"
#include "video_core/host_shaders/vulkan_present_scaleforce_fp16_frag_spv.h"
#include "video_core/host_shaders/vulkan_present_scaleforce_fp32_frag_spv.h"
#include "video_core/renderer_vulkan/present/filters.h"
#include "video_core/renderer_vulkan/present/util.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"

namespace Vulkan {
namespace {

bool SupportsCubicSource(const Device& device, VkFormat format) {
    if (format == VK_FORMAT_UNDEFINED) {
        return true;
    }
    if (!device.IsFormatSupported(format, VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_CUBIC_BIT_EXT,
                                  FormatType::Optimal)) {
        return false;
    }

    const VkPhysicalDeviceImageViewImageFormatInfoEXT view{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_VIEW_IMAGE_FORMAT_INFO_EXT,
        .imageViewType = VK_IMAGE_VIEW_TYPE_2D,
    };
    const VkPhysicalDeviceImageFormatInfo2 info{
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2,
        .pNext = &view,
        .format = format,
        .type = VK_IMAGE_TYPE_2D,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
    };
    VkFilterCubicImageViewImageFormatPropertiesEXT cubic{
        .sType = VK_STRUCTURE_TYPE_FILTER_CUBIC_IMAGE_VIEW_IMAGE_FORMAT_PROPERTIES_EXT,
    };
    VkImageFormatProperties2 properties{
        .sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2,
        .pNext = &cubic,
    };
    return device.GetPhysical().GetImageFormatProperties2(info, properties) == VK_SUCCESS &&
           cubic.filterCubic == VK_TRUE;
}

} // Anonymous namespace

std::unique_ptr<WindowAdaptPass> MakeNearestNeighbor(const Device& device, VkFormat frame_format, bool push_descriptor) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateNearestNeighborSampler(device),
        BuildShader(device, VULKAN_PRESENT_FRAG_SPV), push_descriptor);
}

std::unique_ptr<WindowAdaptPass> MakeBilinear(const Device& device, VkFormat frame_format, bool push_descriptor) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
        BuildShader(device, VULKAN_PRESENT_FRAG_SPV), push_descriptor);
}

std::unique_ptr<WindowAdaptPass> MakeSpline1(const Device& device, VkFormat frame_format, bool push_descriptor) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
        BuildShader(device, PRESENT_SPLINE1_FRAG_SPV), push_descriptor);
}

std::unique_ptr<WindowAdaptPass> MakeBicubic(const Device& device, VkFormat frame_format,
                                             VkCubicFilterWeightsQCOM qcom_weights,
                                             bool push_descriptor, VkFormat source_format) {
    if (device.IsExtFilterCubicSupported() &&
        (device.IsQcomFilterCubicWeightsSupported() ||
         qcom_weights == VK_CUBIC_FILTER_WEIGHTS_CATMULL_ROM_QCOM) &&
        SupportsCubicSource(device, source_format)) {
        return std::make_unique<WindowAdaptPass>(device, frame_format, CreateCubicSampler(device,
            qcom_weights), BuildShader(device, VULKAN_PRESENT_FRAG_SPV), push_descriptor);
    } else {
        return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device), [&](){
            switch (qcom_weights) {
            case VK_CUBIC_FILTER_WEIGHTS_CATMULL_ROM_QCOM:
                return BuildShader(device, PRESENT_BICUBIC_FRAG_SPV);
            case VK_CUBIC_FILTER_WEIGHTS_ZERO_TANGENT_CARDINAL_QCOM:
                return BuildShader(device, PRESENT_ZERO_TANGENT_FRAG_SPV);
            case VK_CUBIC_FILTER_WEIGHTS_B_SPLINE_QCOM:
                return BuildShader(device, PRESENT_BSPLINE_FRAG_SPV);
            case VK_CUBIC_FILTER_WEIGHTS_MITCHELL_NETRAVALI_QCOM:
                return BuildShader(device, PRESENT_MITCHELL_FRAG_SPV);
            default:
                UNREACHABLE();
            }
        }(), push_descriptor);
    }
}

std::unique_ptr<WindowAdaptPass> MakeGaussian(const Device& device, VkFormat frame_format, bool push_descriptor) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
        BuildShader(device, PRESENT_GAUSSIAN_FRAG_SPV), push_descriptor);
}

std::unique_ptr<WindowAdaptPass> MakeLanczos(const Device& device, VkFormat frame_format, bool push_descriptor) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
        BuildShader(device, PRESENT_LANCZOS_FRAG_SPV), push_descriptor);
}

std::unique_ptr<WindowAdaptPass> MakeScaleForce(const Device& device, VkFormat frame_format,
                                                bool push_descriptor) {
    auto const select_fn = [&]() {
        return device.IsFloat16Supported()
            ? BuildShader(device, VULKAN_PRESENT_SCALEFORCE_FP16_FRAG_SPV)
            : BuildShader(device, VULKAN_PRESENT_SCALEFORCE_FP32_FRAG_SPV);
    };
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
                                             select_fn(), push_descriptor);
}

std::unique_ptr<WindowAdaptPass> MakeArea(const Device& device, VkFormat frame_format, bool push_descriptor) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateBilinearSampler(device),
        BuildShader(device, PRESENT_AREA_FRAG_SPV), push_descriptor);
}

std::unique_ptr<WindowAdaptPass> MakeMmpx(const Device& device, VkFormat frame_format, bool push_descriptor) {
    return std::make_unique<WindowAdaptPass>(device, frame_format, CreateNearestNeighborSampler(device),
                                             BuildShader(device, PRESENT_MMPX_FRAG_SPV), push_descriptor);
}

std::unique_ptr<WindowAdaptPass> MakeWindowAdaptPass(
    const Device& device, VkFormat format, VkFormat source_format, Settings::ScalingFilter filter,
    bool push) {
    using Settings::ScalingFilter;
    switch (filter) {
    case ScalingFilter::NearestNeighbor: return MakeNearestNeighbor(device, format, push);
    case ScalingFilter::Bicubic:
        return MakeBicubic(device, format, VK_CUBIC_FILTER_WEIGHTS_CATMULL_ROM_QCOM, push,
                           source_format);
    case ScalingFilter::ZeroTangent:
        return MakeBicubic(device, format, VK_CUBIC_FILTER_WEIGHTS_ZERO_TANGENT_CARDINAL_QCOM, push,
                           source_format);
    case ScalingFilter::BSpline:
        return MakeBicubic(device, format, VK_CUBIC_FILTER_WEIGHTS_B_SPLINE_QCOM, push,
                           source_format);
    case ScalingFilter::Mitchell:
        return MakeBicubic(device, format, VK_CUBIC_FILTER_WEIGHTS_MITCHELL_NETRAVALI_QCOM, push,
                           source_format);
    case ScalingFilter::Spline1: return MakeSpline1(device, format, push);
    case ScalingFilter::Gaussian: return MakeGaussian(device, format, push);
    case ScalingFilter::Lanczos: return MakeLanczos(device, format, push);
    case ScalingFilter::ScaleForce: return MakeScaleForce(device, format, push);
    case ScalingFilter::Area: return MakeArea(device, format, push);
    case ScalingFilter::Mmpx: return MakeMmpx(device, format, push);
    default: return MakeBilinear(device, format, push);
    }
}

} // namespace Vulkan
