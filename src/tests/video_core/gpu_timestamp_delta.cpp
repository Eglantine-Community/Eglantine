// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "video_core/gpu_timestamp_delta.h"

TEST_CASE("GPU timestamp deltas respect the queue's valid bits and wraparound",
          "[video_core][gpu_timestamp_delta]") {
    using VideoCommon::GpuTimestampDelta;
    REQUIRE(GpuTimestampDelta(100, 140, 64) == 40);
    REQUIRE(GpuTimestampDelta(100, 100, 36) == 0);
    REQUIRE(GpuTimestampDelta((u64{1} << 36) - 4, 3, 36) == 7);
    REQUIRE(GpuTimestampDelta(~u64{0} - 3, 3, 64) == 7);
    REQUIRE(GpuTimestampDelta(0, 1, 0) == 0);
    REQUIRE(GpuTimestampDelta(0, 1, 65) == 0);
}
