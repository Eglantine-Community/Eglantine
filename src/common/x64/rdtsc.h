// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef _MSC_VER
#include <intrin.h>
#endif

#include "common/common_types.h"

namespace Common::X64 {

enum class TscFence {
    Load,
    Memory,
    CpuId,
};

struct TscOrdering {
    TscFence fence;
    bool ordered_read;
};

const TscOrdering& GetTscOrdering();
u64 ReadGuestCounterTsc();

#ifdef _MSC_VER
__forceinline static u64 FencedRDTSC() {
    _mm_lfence();
    _ReadWriteBarrier();
    const u64 result = __rdtsc();
    _mm_lfence();
    _ReadWriteBarrier();
    return result;
}
#else
static inline u64 FencedRDTSC() {
    u64 eax;
    u64 edx;
    asm volatile("lfence\n\t"
                 "rdtsc\n\t"
                 "lfence\n\t"
                 : "=a"(eax), "=d"(edx)
                 :
                 : "memory");
    return (edx << 32) | eax;
}
#endif

u64 EstimateRDTSCFrequency();

} // namespace Common::X64
