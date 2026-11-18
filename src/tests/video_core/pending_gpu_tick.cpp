// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <thread>

#include <catch2/catch_test_macros.hpp>

#include "video_core/pending_gpu_tick.h"

TEST_CASE("Pending GPU ticks notify only at completion", "[video_core][pending_gpu_tick]") {
    Tegra::PendingGpuTick pending;
    REQUIRE_FALSE(pending.Complete(100));
    pending.Watch(20);
    pending.Watch(30);
    pending.Watch(10);
    REQUIRE_FALSE(pending.Complete(9));
    REQUIRE(pending.Complete(10));
    REQUIRE_FALSE(pending.Complete(10));
    // Remaining channels re-register when the scheduler checks them after the wake.
    pending.Watch(20);
    pending.Watch(30);
    REQUIRE_FALSE(pending.Complete(19));
    REQUIRE(pending.Complete(30));
    REQUIRE_FALSE(pending.Complete(31));
}

TEST_CASE("A completed GPU tick is seen by the channel check or its notification",
          "[video_core][pending_gpu_tick]") {
    for (int i = 0; i < 1000; ++i) {
        Tegra::PendingGpuTick pending;
        std::atomic<u64> completed{};
        bool notified{};
        std::thread observer{[&] {
            completed.store(10, std::memory_order_release);
            notified = pending.Complete(10);
        }};
        pending.Watch(10);
        const bool ready = completed.load(std::memory_order_acquire) >= 10;
        observer.join();
        REQUIRE((ready || notified));
    }
}
