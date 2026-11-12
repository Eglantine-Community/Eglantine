// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>

#include "common/common_types.h"
#include "common/env_gate.h"
#include "video_core/vulkan_common/vulkan.h"

namespace Vulkan {

inline bool BuildsUnoptimized(VkDriverId driver_id, u64 shader_hash) {
    // The Legend of Zelda: Ocarina of Time (0400486024DEC000) compute shaders on AMD drivers
    static constexpr std::array<u64, 2> amd_proprietary{
        0xeff3ccbe1438679dULL, // compute shader built as the AV1 intro starts: 6 min (#200)
        0xf9ea14a22394f5b1ULL, // compute shader built as the AV1 intro starts: 18 min (#200)
    };
    static const bool enabled{Common::EnvGate("DALIA_AMD_UNOPT_COMPUTE") &&
                              Common::EnvGate("EGLANTINE_AMD_UNOPT_COMPUTE")};
    const bool is_amd = driver_id == VK_DRIVER_ID_AMD_PROPRIETARY ||
                        driver_id == VK_DRIVER_ID_AMD_OPEN_SOURCE;
    return enabled && is_amd &&
           std::ranges::find(amd_proprietary, shader_hash) != amd_proprietary.end();
}

} // namespace Vulkan
