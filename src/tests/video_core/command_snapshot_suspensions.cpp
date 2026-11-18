// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "core/core.h"
#include "tests/core/system_test_context.h"
#include "video_core/control/channel_state.h"
#include "video_core/dma_pusher.h"
#include "video_core/memory_manager.h"

TEST_CASE("Entries parsed without a pending acquire keep their submit-time snapshot",
          "[video_core][dma_pusher]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::MemoryManager memory_manager{system};
    Tegra::Control::ChannelState channel_state{0};
    Tegra::DmaPusher dma_pusher{system, memory_manager, channel_state};

    const u64 snapshot_suspensions = dma_pusher.suspensions.load();
    for (int entry = 0; entry < 8; ++entry) {
        REQUIRE_FALSE(dma_pusher.puller.HasPendingAcquire());
        REQUIRE(dma_pusher.puller.ResumeAcquire(dma_pusher));
    }
    REQUIRE(dma_pusher.suspensions.load() == snapshot_suspensions);
}

TEST_CASE("A synchronous snapshot refresh waits for the list's queued copy",
          "[video_core][dma_pusher]") {
    ScopedSystemTestContext context;
    Core::System system;
    Tegra::MemoryManager memory_manager{system};
    Tegra::Control::ChannelState channel_state{0};
    Tegra::DmaPusher dma_pusher{system, memory_manager, channel_state};

    Tegra::CommandList list;
    list.submitted_offsets.push_back(0);
    list.deferred_entries.push_back(0);
    REQUIRE(dma_pusher.QueueSnapshotCopy(list, 2));
    REQUIRE(list.copy_job != 0);
    REQUIRE(list.snapshot_suspensions == 2);
    REQUIRE(list.deferred_entries.empty());

    dma_pusher.RefreshSnapshot(list, 4);
    REQUIRE(list.copy_job == 0);
    REQUIRE(list.snapshot_suspensions == 4);
    Tegra::DmaPusher::WaitForSnapshotCopies();
}
