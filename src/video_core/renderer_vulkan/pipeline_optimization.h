// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>
#include <string_view>

#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {

constexpr VkPipelineCreateFlags ComputeOptimizationFlags(const char* opt_in) {
    constexpr VkPipelineCreateFlags disable = VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;
    return opt_in && std::string_view{opt_in} == "1" ? disable : 0;
}

inline VkPipelineCreateFlags ComputeOptimizationFlags() {
    // Opt-in for guest compute compiler diagnostics. Vulkan does not guarantee a shorter build.
    static const auto flags =
        ComputeOptimizationFlags(std::getenv("EGLANTINE_VK_COMPUTE_DISABLE_OPTIMIZATION"));
    return flags;
}

} // namespace Vulkan
