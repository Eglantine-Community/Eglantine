// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>

#include "common/common_types.h"

namespace Tegra {

// The earliest command-list copy a suspended channel needs. Ported from Lichii
// 9f932fe3c5; callers still check the rasterizer before resuming a channel.
class PendingGpuTick {
public:
    static constexpr u64 NoTick = ~u64{0};

    void Watch(u64 tick) {
        u64 wanted = earliest.load(std::memory_order_seq_cst);
        while (tick < wanted) {
            if (earliest.compare_exchange_weak(wanted, tick, std::memory_order_seq_cst)) {
                earliest.notify_all();
                break;
            }
        }
    }

    [[nodiscard]] bool Complete(u64 tick) {
        // Publish the completed tick first. Together with Watch-before-IsGpuTickDone,
        // one side then observes the other's publication.
        std::atomic_thread_fence(std::memory_order_seq_cst);
        u64 wanted = earliest.load(std::memory_order_seq_cst);
        while (wanted != NoTick && tick >= wanted) {
            if (earliest.compare_exchange_weak(wanted, NoTick, std::memory_order_seq_cst)) {
                return true;
            }
        }
        return false;
    }

    [[nodiscard]] u64 Wanted() const noexcept {
        return earliest.load(std::memory_order_acquire);
    }

    void WaitForWatch() const noexcept {
        earliest.wait(NoTick, std::memory_order_acquire);
    }

    void WakeAll() noexcept {
        earliest.store(0, std::memory_order_seq_cst);
        earliest.notify_all();
    }

    void Reset() noexcept {
        earliest.store(NoTick, std::memory_order_seq_cst);
    }

private:
    std::atomic<u64> earliest{NoTick};
};

inline PendingGpuTick g_pending_gpu_tick;
inline std::atomic<u64> g_pending_gpu_tick_sequence{};
inline constexpr u32 PendingGpuTickWakeReason = 4;

} // namespace Tegra
