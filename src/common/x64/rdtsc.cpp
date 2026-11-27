// SPDX-FileCopyrightText: Copyright 2023 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <thread>

#ifndef _MSC_VER
#include <cpuid.h>
#endif

#include "common/steady_clock.h"
#include "common/uint128.h"
#include "common/x64/rdtsc.h"

namespace Common::X64 {

const TscOrdering& GetTscOrdering() {
    static const TscOrdering ordering = [] {
        const auto cpuid = [](u32 leaf) {
            std::array<u32, 4> regs{};
#ifdef _MSC_VER
            std::array<int, 4> values{};
            __cpuid(values.data(), static_cast<int>(leaf));
            for (std::size_t index = 0; index < values.size(); ++index) {
                regs[index] = static_cast<u32>(values[index]);
            }
#else
            __cpuid(leaf, regs[0], regs[1], regs[2], regs[3]);
#endif
            return regs;
        };

        const auto [max_basic, ebx, ecx, edx] = cpuid(0);
        const bool intel = ebx == 0x756e6547 && edx == 0x49656e69 && ecx == 0x6c65746e;
        const bool amd = ebx == 0x68747541 && edx == 0x69746e65 && ecx == 0x444d4163;
        const u32 max_ext = cpuid(0x80000000)[0];
        const bool has_rdtscp = max_ext >= 0x80000001 && (cpuid(0x80000001)[3] & (1u << 27));
        if (intel) {
            return TscOrdering{TscFence::Load, has_rdtscp};
        }
        if (amd) {
            const bool lfence_serializes = max_ext >= 0x80000021 && (cpuid(0x80000021)[0] & 4u);
            const u32 version = max_basic >= 1 ? cpuid(1)[0] : 0;
            const u32 family = (version >> 8) & 0xf;
            const u32 display_family = family + (family == 0xf ? (version >> 20) & 0xff : 0);
            // AMD erratum 791 prevents relying on RDTSCP retirement order on Family 16h.
            return TscOrdering{lfence_serializes ? TscFence::Load : TscFence::Memory,
                               has_rdtscp && display_family >= 0x17};
        }
        return TscOrdering{TscFence::CpuId, false};
    }();
    return ordering;
}

u64 ReadGuestCounterTsc() {
    const auto fence = GetTscOrdering().fence;
    if (fence == TscFence::Load) {
        return FencedRDTSC();
    }
#ifdef _MSC_VER
    std::array<int, 4> cpuid{};
    const auto serialize = [fence, &cpuid] {
        _ReadWriteBarrier();
        if (fence == TscFence::Memory) {
            _mm_mfence();
        } else {
            __cpuid(cpuid.data(), 0);
        }
        _ReadWriteBarrier();
    };
    serialize();
    const u64 result = __rdtsc();
    serialize();
    return result;
#else
    u32 lo;
    u32 hi;
    if (fence == TscFence::Memory) {
        asm volatile("mfence\n\t"
                     "rdtsc\n\t"
                     "mfence"
                     : "=a"(lo), "=d"(hi)
                     :
                     : "memory");
    } else {
        asm volatile("movl $0, %%eax\n\t"
                     "cpuid\n\t"
                     "rdtsc\n\t"
                     "movl %%eax, %[lo]\n\t"
                     "movl %%edx, %[hi]\n\t"
                     "movl $0, %%eax\n\t"
                     "cpuid"
                     : [lo] "=&r"(lo), [hi] "=&r"(hi)
                     :
                     : "rax", "rbx", "rcx", "rdx", "cc", "memory");
    }
    return (static_cast<u64>(hi) << 32) | lo;
#endif
}

template <u64 Nearest>
static u64 RoundToNearest(u64 value) {
    const auto mod = value % Nearest;
    return mod >= (Nearest / 2) ? (value - mod + Nearest) : (value - mod);
}

u64 EstimateRDTSCFrequency() {
    // Discard the first result measuring the rdtsc.
    FencedRDTSC();
    std::this_thread::sleep_for(std::chrono::milliseconds{1});
    FencedRDTSC();

    // Get the current time.
    const auto start_time = RealTimeClock::Now();
    const u64 tsc_start = FencedRDTSC();
    // Wait for 100 milliseconds.
    std::this_thread::sleep_for(std::chrono::milliseconds{100});
    const auto end_time = RealTimeClock::Now();
    const u64 tsc_end = FencedRDTSC();
    // Calculate differences.
    const u64 timer_diff = static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(end_time - start_time).count());
    const u64 tsc_diff = tsc_end - tsc_start;
    const u64 tsc_freq = MultiplyAndDivide64(tsc_diff, 1000000000ULL, timer_diff);
    return RoundToNearest<100'000>(tsc_freq);
}

} // namespace Common::X64
