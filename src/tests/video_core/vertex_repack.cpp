// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#include "video_core/renderer_vulkan/fixed_pipeline_state.h"
#include "video_core/renderer_vulkan/vertex_repack.h"

namespace {
using Vulkan::MakeVertexRepackLayout;
using Vulkan::VertexRepackAttribute;
using Vulkan::VertexRepackCopy;
using Vulkan::VertexRepackLimits;

constexpr VertexRepackLimits Limits{
    .stride_alignment = 1,
    .max_stride = 2048,
    .max_attribute_offset = 2047,
    .access_beyond_stride = true,
    .allow_zero_stride = true,
};
} // namespace

TEST_CASE("Vertex repack preserves native aligned or overlapping access", "[vertex_repack]") {
    const std::array attrs{VertexRepackAttribute{0, 3, 8, 4}};
    const auto native = MakeVertexRepackLayout(0x1001, 8, attrs, Limits);
    REQUIRE(native.supported);
    CHECK(native.extent == 11);
    CHECK_FALSE(native.layout);
    CHECK_FALSE(MakeVertexRepackLayout(0, 0, {}, Limits).layout);
}

TEST_CASE("Vertex repack gathers overlapping fields from the same source", "[vertex_repack]") {
    const std::array attrs{VertexRepackAttribute{0, 5, 3, 1},
                           VertexRepackAttribute{12, 6, 4, 4},
                           VertexRepackAttribute{31, 5, 2, 2}};
    auto limits = Limits;
    limits.access_beyond_stride = false;
    limits.stride_alignment = 8;
    const auto result = MakeVertexRepackLayout(0x1000, 7, attrs, limits);
    REQUIRE(result.supported);
    CHECK(result.extent == 10);
    REQUIRE(result.layout);
    const auto& layout = *result.layout;
    CHECK(layout.source_stride == 7);
    CHECK(layout.stride == 16);
    CHECK(layout.BindingStride() == 16);
    CHECK(layout.attribute_offsets[0] == 0);
    CHECK(layout.attribute_offsets[12] == 4);
    CHECK(layout.attribute_offsets[31] == 8);
    REQUIRE(layout.copies.size() == 3);
    CHECK(layout.copies[0] == VertexRepackCopy{5, 0, 3});
    CHECK(layout.copies[1] == VertexRepackCopy{6, 4, 4});
    CHECK(layout.copies[2] == VertexRepackCopy{5, 8, 2});
}

TEST_CASE("Vertex repack validates address and stride independently", "[vertex_repack]") {
    const std::array attrs{VertexRepackAttribute{0, 4, 4, 4}};
    REQUIRE(MakeVertexRepackLayout(0x1001, 16, attrs, Limits).layout);
    REQUIRE(MakeVertexRepackLayout(0x1000, 15, attrs, Limits).layout);
    const auto native = MakeVertexRepackLayout(0x1000, 16, attrs, Limits);
    REQUIRE(native.supported);
    CHECK_FALSE(native.layout);
}

TEST_CASE("Native unaligned fetch preserves only layouts legal under device limits",
          "[vertex_repack]") {
    const std::array attrs{VertexRepackAttribute{4, 3, 4, 4}};
    auto limits = Limits;
    limits.native_unaligned = true;
    const auto native = MakeVertexRepackLayout(0x1001, 3, attrs, limits);
    REQUIRE(native.supported);
    CHECK_FALSE(native.layout);
    limits.access_beyond_stride = false;
    const auto repacked = MakeVertexRepackLayout(0x1001, 3, attrs, limits);
    REQUIRE(repacked.supported);
    REQUIRE(repacked.layout);
    CHECK(repacked.layout->stride == 4);
    CHECK(repacked.layout->attribute_offsets[4] == 0);
}

TEST_CASE("Vertex repack gathers one record for a legal zero stride", "[vertex_repack]") {
    const std::array attrs{VertexRepackAttribute{4, 3, 4, 4}};
    const auto result = MakeVertexRepackLayout(0x1000, 0, attrs, Limits);
    REQUIRE(result.supported);
    REQUIRE(result.layout);
    CHECK(result.layout->source_stride == 0);
    CHECK(result.layout->BindingStride() == 0);
    CHECK(result.layout->stride == 4);
    REQUIRE(result.layout->copies.size() == 1);
    CHECK(result.layout->copies[0] == VertexRepackCopy{3, 0, 4});
    auto limits = Limits;
    limits.allow_zero_stride = false;
    CHECK_FALSE(MakeVertexRepackLayout(0x1000, 0, attrs, limits).supported);
    limits.allow_zero_stride = true;
    limits.access_beyond_stride = false;
    CHECK_FALSE(MakeVertexRepackLayout(0x1000, 0, attrs, limits).supported);
}

TEST_CASE("Vertex repack checks output descriptor limits and format compatibility", "[vertex_repack]") {
    std::array attrs{VertexRepackAttribute{1, 41, 8, 4},
                     VertexRepackAttribute{3, 42, 4, 4}};
    auto limits = Limits;
    limits.max_stride = 16;
    limits.max_attribute_offset = 8;
    auto result = MakeVertexRepackLayout(0x1000, 48, attrs, limits);
    REQUIRE(result.supported);
    REQUIRE(result.layout);
    CHECK(result.layout->stride == 12);
    CHECK(result.layout->attribute_offsets[3] == 8);

    limits.max_attribute_offset = 7;
    CHECK_FALSE(MakeVertexRepackLayout(0x1000, 48, attrs, limits).supported);
    limits.max_attribute_offset = 8;
    limits.max_stride = 11;
    CHECK_FALSE(MakeVertexRepackLayout(0x1000, 48, attrs, limits).supported);
    limits.max_stride = 16;
    attrs[0].bit_compatible = false;
    CHECK_FALSE(MakeVertexRepackLayout(0x1000, 48, attrs, limits).supported);
    const std::array native_incompatible{VertexRepackAttribute{0, 4, 4, 4, false}};
    CHECK_FALSE(MakeVertexRepackLayout(0x1000, 16, native_incompatible, Limits).supported);
}

TEST_CASE("Vertex stride zero requires a compatible binding command on portability subset",
          "[vertex_repack]") {
    using Vulkan::ChooseVertexStridePolicy;
    using Vulkan::VertexStridePolicy;
    CHECK(ChooseVertexStridePolicy(true, true, true, false) == VertexStridePolicy::Dynamic);
    CHECK(ChooseVertexStridePolicy(false, true, true, false) == VertexStridePolicy::Unsupported);
    CHECK(ChooseVertexStridePolicy(true, true, true, true) == VertexStridePolicy::Unsupported);
    CHECK(ChooseVertexStridePolicy(true, true, false, false) == VertexStridePolicy::Fixed);
    CHECK(ChooseVertexStridePolicy(true, false, false, false) == VertexStridePolicy::Dynamic);
    CHECK(ChooseVertexStridePolicy(true, false, false, true) == VertexStridePolicy::Fixed);
}

TEST_CASE("Vertex input limits apply to locations and bindings actually used by the shader",
          "[vertex_repack]") {
    using Vulkan::FindOutOfRangeVertexInput;
    std::array<std::optional<u32>, 32> used_bindings{};
    const auto read_binding = [&](u32 location) { return used_bindings[location]; };
    used_bindings[0] = 0;
    used_bindings[15] = 15;
    CHECK_FALSE(FindOutOfRangeVertexInput(16, 16, read_binding));
    used_bindings[19] = 1;
    CHECK(FindOutOfRangeVertexInput(16, 16, read_binding) == 19);
    used_bindings[2] = 16;
    CHECK(FindOutOfRangeVertexInput(16, 16, read_binding) == 2);
    CHECK_FALSE(FindOutOfRangeVertexInput(32, 32, read_binding));
}

TEST_CASE("Maxwell vertex stride keys separate static and binding-command layouts",
          "[vertex_repack]") {
    Vulkan::FixedPipelineState legacy{};
    legacy.extended_dynamic_state.Assign(1);
    legacy.vertex_strides[0] = 8;

    auto layout = legacy;
    layout.vertex_strides[0] = 4;
    CHECK(layout.UsesDynamicVertexBindingStride());
    CHECK(layout.Size() == offsetof(Vulkan::FixedPipelineState, vertex_strides));
    CHECK(layout == legacy);
    CHECK(layout.Hash() == legacy.Hash());

    layout.static_vertex_stride.Assign(1);
    CHECK_FALSE(layout.UsesDynamicVertexBindingStride());
    CHECK(layout.Size() == offsetof(Vulkan::FixedPipelineState, xfb_state));
    CHECK(layout != legacy);

    auto other_stride = layout;
    other_stride.vertex_strides[0] = 2;
    CHECK(layout != other_stride);
    CHECK(layout.Hash() != other_stride.Hash());

    legacy.dynamic_vertex_input.Assign(1);
    CHECK_FALSE(legacy.UsesDynamicVertexBindingStride());
    auto dynamic_input = legacy;
    dynamic_input.vertex_strides[0] = 4;
    CHECK(dynamic_input == legacy);
}
