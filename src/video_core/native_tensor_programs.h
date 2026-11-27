// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>

#include "common/common_types.h"

namespace VideoCommon {

/// Translation strategy for native Ampere matrix instructions.
enum class NativeTensorLowering : u8 {
    None,    ///< rejected (the program is skipped or replaced)
    Full,    ///< inline expansion
    Compact, ///< one call of a shared helper per HMMA; needs compute shaders 32 threads wide
    Workgroup, ///< scratch-backed HMMA for straight-line compute with complete guest warps
};

/// Callers pass the physical limits before multiplying untrusted local dimensions.
[[nodiscard]] constexpr bool ValidTensorWorkgroup(const std::array<u32, 3>& group,
                                                   const u32 (&max_dim)[3],
                                                   u32 max_invocations) noexcept {
    const auto [x, y, z] = group;
    return x != 0 && y != 0 && z != 0 && x <= max_dim[0] && y <= max_dim[1] && z <= max_dim[2] &&
           x <= max_invocations / y / z;
}

[[nodiscard]] constexpr bool CanUseWorkgroupTensor(const std::array<u32, 3>& group) noexcept {
    constexpr u32 warp_size = 32;
    const auto [x, y, z] = group;
    // Only the low five bits of the product are needed.
    return x != 0 && y != 0 && z != 0 && (x * y * z) % warp_size == 0;
}

/// Compact helpers use native shuffles. Inline HMMA can also use constant broadcasts.
[[nodiscard]] constexpr NativeTensorLowering NativeTensorLoweringFor(
    const std::array<u32, 3>& workgroup, u32 subgroup_width, bool native_shuffle, bool ballot) noexcept {
    constexpr u32 warp_size = 32;
    const auto [x, y, z] = workgroup;
    // Only the first warp matters for this limit. Capping each factor keeps the product in range.
    const u32 threads = (std::min)(x, warp_size) * (std::min)(y, warp_size) * (std::min)(z, warp_size);
    if (threads < warp_size || subgroup_width < warp_size || (!native_shuffle && !ballot)) {
        return NativeTensorLowering::None;
    }
    return native_shuffle && x % subgroup_width == 0 ? NativeTensorLowering::Compact
                                                     : NativeTensorLowering::Full;
}

} // namespace VideoCommon
