// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <limits>
#include <optional>

#include <vulkan/vulkan_core.h>
#include "common/common_types.h"

namespace Vulkan {

struct IndexRestartPlan {
    VkIndexType host_type{};
    std::optional<u32> native_marker;
};

[[nodiscard]] constexpr IndexRestartPlan PlanIndexRestart(VkIndexType type,
                                                         std::optional<u32> guest_marker,
                                                         bool native_uint8, bool custom_restart) {
    const u32 fixed_marker = type == VK_INDEX_TYPE_UINT8_EXT ? 0xffU :
                             type == VK_INDEX_TYPE_UINT16 ? 0xffffU :
                             std::numeric_limits<u32>::max();
    const bool custom = guest_marker && *guest_marker != fixed_marker;
    // VUID-vkCmdDrawIndexed-primitiveRestartIndex-12401 limits the command to the bound type.
    const bool can_set = custom && custom_restart && *guest_marker <= fixed_marker;
    if (type == VK_INDEX_TYPE_UINT8_EXT && (!native_uint8 || (custom && !can_set))) {
        return {.host_type = VK_INDEX_TYPE_UINT16};
    }
    if (type == VK_INDEX_TYPE_UINT16 && custom && !can_set) {
        return {.host_type = VK_INDEX_TYPE_UINT32};
    }
    return {.host_type = type, .native_marker = can_set ? guest_marker : std::nullopt};
}

struct IndexWideningSize {
    u64 input_bytes;
    u64 output_words;
    u32 work_items;
};

/// Both kernels operate on whole 32-bit words, including the last partial output pair for UINT8.
[[nodiscard]] constexpr IndexWideningSize GetIndexWideningSize(VkIndexType source, u32 count) {
    const u32 pairs = count / 2 + count % 2;
    if (source == VK_INDEX_TYPE_UINT16) {
        return {u64{count} * sizeof(u16), count, pairs};
    }
    return {count, pairs, pairs};
}

} // namespace Vulkan
