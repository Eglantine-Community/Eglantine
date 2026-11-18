// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include "common/common_types.h"

namespace VideoCommon {

constexpr u64 GpuTimestampDelta(u64 begin, u64 end, u32 valid_bits) {
    if (valid_bits == 0 || valid_bits > 64) {
        return 0;
    }
    return valid_bits == 64 ? end - begin : (end - begin) & ((u64{1} << valid_bits) - 1);
}

} // namespace VideoCommon
