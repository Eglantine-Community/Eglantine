// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <numeric>
#include <optional>
#include <span>

#include <boost/container/small_vector.hpp>

#include "video_core/engines/maxwell_3d.h"

namespace Vulkan {

class Device;

struct VertexRepackCopy {
    u32 source_offset;
    u32 destination_offset;
    u32 byte_count;

    bool operator==(const VertexRepackCopy&) const = default;
};

struct VertexRepackLayout {
    static constexpr u32 MaxAttributes = 32;

    u32 source_stride;
    /// Size of one output record, including padding. The compute shader writes complete words.
    u32 stride;
    boost::container::small_vector<VertexRepackCopy, 4> copies;
    std::array<u16, MaxAttributes> attribute_offsets{};

    u32 BindingStride() const {
        return source_stride == 0 ? 0 : stride;
    }

    bool operator==(const VertexRepackLayout&) const = default;
};

struct VertexRepackDecision {
    bool supported{true};
    /// Highest host attribute extent when binding the original buffer.
    u32 extent{};
    std::optional<VertexRepackLayout> layout;
};

struct VertexRepackAttribute {
    u32 location;
    u32 offset;
    u32 byte_count;
    u32 alignment;
    /// A host format substitution can require numeric conversion instead of a byte copy.
    bool bit_compatible{true};
};

struct VertexRepackLimits {
    u32 stride_alignment;
    u32 max_stride;
    u32 max_attribute_offset;
    bool access_beyond_stride;
    bool allow_zero_stride;
    bool native_unaligned{};
};

[[nodiscard]] inline VertexRepackDecision MakeVertexRepackLayout(
    u64 source_address, u32 source_stride, std::span<const VertexRepackAttribute> attributes,
    const VertexRepackLimits& limits) {
    if (attributes.empty()) {
        return {};
    }
    if (limits.stride_alignment == 0 ||
        (source_stride == 0 && (!limits.allow_zero_stride || !limits.access_beyond_stride))) {
        return {.supported = false};
    }
    VertexRepackDecision decision{};
    bool needs_repack =
        source_stride > limits.max_stride || source_stride % limits.stride_alignment != 0;
    for (const auto& attr : attributes) {
        if (!attr.bit_compatible || attr.byte_count == 0 || attr.alignment == 0 ||
            attr.location >= VertexRepackLayout::MaxAttributes ||
            attr.offset > std::numeric_limits<u32>::max() - attr.byte_count) {
            return {.supported = false};
        }
        decision.extent = (std::max)(decision.extent, attr.offset + attr.byte_count);
        const u64 address_remainder =
            (source_address % attr.alignment + attr.offset % attr.alignment) % attr.alignment;
        needs_repack |= attr.offset > limits.max_attribute_offset ||
                        (!limits.native_unaligned &&
                         (address_remainder != 0 || source_stride % attr.alignment != 0));
    }
    needs_repack |= !limits.access_beyond_stride && decision.extent > source_stride;
    if (!needs_repack) {
        return decision;
    }
    auto& layout = decision.layout.emplace();
    layout.source_stride = source_stride;
    u64 output_end = 0;
    u64 output_alignment = std::lcm(u64{4}, u64{limits.stride_alignment});
    for (const auto& attr : attributes) {
        // Give each attribute its own output words even if source attributes overlap.
        const u64 alignment = std::lcm(u64{4}, u64{attr.alignment});
        const u64 least_multiple = output_alignment / std::gcd(output_alignment, alignment);
        if (least_multiple > limits.max_stride / alignment) {
            return {.supported = false};
        }
        output_alignment = least_multiple * alignment;
        const u64 offset = (output_end + alignment - 1) / alignment * alignment;
        output_end = offset + attr.byte_count;
        if (offset > limits.max_attribute_offset || offset > std::numeric_limits<u16>::max() ||
            output_end > limits.max_stride) {
            return {.supported = false};
        }
        layout.attribute_offsets[attr.location] = static_cast<u16>(offset);
        layout.copies.push_back({attr.offset, static_cast<u32>(offset), attr.byte_count});
    }
    const u64 stride = (output_end + output_alignment - 1) / output_alignment * output_alignment;
    if (stride > limits.max_stride) {
        return {.supported = false};
    }
    layout.stride = static_cast<u32>(stride);
    return decision;
}

enum class VertexStridePolicy {
    Fixed,
    Dynamic,
    Unsupported,
};

[[nodiscard]] constexpr VertexStridePolicy ChooseVertexStridePolicy(
    bool has_dynamic_stride, bool has_portability_subset, bool needs_dynamic_zero,
    bool has_positive_overlap) {
    if (needs_dynamic_zero) {
        return has_dynamic_stride && !has_positive_overlap ? VertexStridePolicy::Dynamic
                                                            : VertexStridePolicy::Unsupported;
    }
    return has_dynamic_stride && !has_portability_subset && !has_positive_overlap
               ? VertexStridePolicy::Dynamic
               : VertexStridePolicy::Fixed;
}

template <typename F>
[[nodiscard]] std::optional<u32> FindOutOfRangeVertexInput(u32 max_attributes, u32 max_bindings,
                                                          F&& used_binding) {
    using Regs = Tegra::Engines::Maxwell3D::Regs;
    if (max_attributes >= Regs::NumVertexAttributes && max_bindings >= Regs::NumVertexArrays) {
        return std::nullopt;
    }
    for (u32 location = 0; location < Regs::NumVertexAttributes; ++location) {
        const std::optional<u32> binding = used_binding(location);
        if (binding && (location >= max_attributes || *binding >= max_bindings)) {
            return location;
        }
    }
    return std::nullopt;
}

[[nodiscard]] VertexRepackDecision BuildVertexRepackLayout(
    const Tegra::Engines::Maxwell3D::Regs& regs, const Device& device, u32 binding,
    bool dynamic_input);

} // namespace Vulkan
