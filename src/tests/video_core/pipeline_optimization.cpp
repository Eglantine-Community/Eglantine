// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/renderer_vulkan/pipeline_optimization.h"

TEST_CASE("Compute pipeline optimization is disabled only by an explicit diagnostic opt-in",
          "[video_core][vulkan][pipeline_optimization]") {
    CHECK(Vulkan::ComputeOptimizationFlags(nullptr) == 0);
    CHECK(Vulkan::ComputeOptimizationFlags("") == 0);
    CHECK(Vulkan::ComputeOptimizationFlags("0") == 0);
    CHECK(Vulkan::ComputeOptimizationFlags("true") == 0);
    CHECK(Vulkan::ComputeOptimizationFlags("1") == VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT);
}
