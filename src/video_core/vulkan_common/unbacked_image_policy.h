// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace Vulkan {

/// Above this texel count the allocator queries the image's actual memory requirements.
inline constexpr u64 LargeImageTexelThreshold = u64{1} << 28;

/// Restrict oversized allocations to the heap budget reported to VMA.
/// A zero size means that the driver could not report the memory requirement.
constexpr bool KeepImageWithinBudget(u64 texels, u64 required_bytes, u64 device_local_bytes) {
    if (texels <= LargeImageTexelThreshold) {
        return false;
    }
    return required_bytes == 0 || required_bytes > device_local_bytes / 4;
}

} // namespace Vulkan
