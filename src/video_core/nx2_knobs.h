// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstdlib>

#include "common/common_types.h"
#include "common/settings.h"

namespace VideoCommon::Nx2Knob {

// Live switches for experiment variables, to A/B a change without a restart. The EDEN_NX2_*
// variable still sets the value at start; its debug knob bit (MCP debug_knobs, set / clear)
// inverts it while the game runs. Only switches read afresh at each use can be wired here.
// Bits 0-2, 4, 7 and 8 are free: their switches were removed.
enum : u8 {
    InlineDefer = 3,   ///< EDEN_NX2_INLINE_DEFER: invalidate the caches one at a time
    FlushDraws32 = 5,  ///< EDEN_NX2_FLUSH_DRAWS: submit to the host GPU every 32 draws while set
    FlushDraws512 = 6, ///< ... every 512 draws while set (bit 5 wins)
    BackWait = 9,      ///< EDEN_NX2_BACK_WAIT=1: at a present wait for the GPU back thread to finish the
                       ///< frame's commands (and the previous fence), not for this frame's fence
    HostImport = 10,   ///< EDEN_NX2_HOST_IMPORT=1: 64 KiB native uniform banks read the guest's pages in place (no copy)
    NanSafeMinMax = 11, ///< EDEN_NX2_NAN_SAFE_MIN_MAX (on unless 0): see NanSafeMinMaxOn
    FreshBuffers = 12,  ///< Off by default: see FreshBuffersOn
    TerrainTrace = 13,  ///< Bounded emulated-GPU terrain resource history
};

/// `start_value` is what the environment chose; the knob inverts it.
[[nodiscard]] inline bool Live(bool start_value, u8 knob) {
    return start_value != Settings::GetDebugKnobAt(knob);
}

/// Saturate, clamp and FMNMX in emitted SPIR-V ignore a NaN operand, as the guest GPU does
/// (Shader::RuntimeInfo::nan_safe_min_max). Read when a stage is emitted, so a change reaches the
/// pipelines built after it: reject and restore a program (reject_shaders) to rebuild its own.
[[nodiscard]] inline bool NanSafeMinMaxOn() {
    static const bool start = [] {
        const char* const value = std::getenv("EDEN_NX2_NAN_SAFE_MIN_MAX");
        return value == nullptr || value[0] != '0';
    }();
    return Live(start, NanSafeMinMax);
}

/// Debugging stale host buffers: every bound uniform, storage, vertex and index range the GPU has
/// not written is uploaded again from guest memory, past the bind memo, the uniform stream reuse
/// and host import. Slow; set with debug knob 12 while a suspected stale-data bug shows.
[[nodiscard]] inline bool FreshBuffersOn() {
    return Live(false, FreshBuffers);
}

} // namespace VideoCommon::Nx2Knob
