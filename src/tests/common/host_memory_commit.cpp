// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <catch2/catch_test_macros.hpp>

#include "common/host_memory.h"
#include "common/literals.h"

using namespace Common::Literals;

TEST_CASE("Deferred backing commits shared granules without committing unused ranges",
          "[common][host_memory][host_commit]") {
    constexpr size_t GranuleSize = 64_KiB;
    Common::HostMemory memory{8_MiB, 1_GiB, Common::HostMemory::BackingMode::Deferred};
    REQUIRE(memory.BackingBasePointer() != nullptr);
    REQUIRE(memory.VirtualBasePointer() != nullptr);

    MEMORY_BASIC_INFORMATION region{};
    REQUIRE(VirtualQuery(memory.BackingBasePointer(), &region, sizeof(region)) == sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);
    REQUIRE(region.RegionSize == 8_MiB);

    memory.MarkUsed(0x1000, 0x1000);
    REQUIRE(VirtualQuery(memory.BackingBasePointer(), &region, sizeof(region)) == sizeof(region));
    REQUIRE(region.State == MEM_COMMIT);
    REQUIRE(region.RegionSize == GranuleSize);

    memory.Map(0x10000, 0, GranuleSize, Common::MemoryPermission::ReadWrite, false);
    memory.Map(0x30000, 0, GranuleSize, Common::MemoryPermission::ReadWrite, false);
    volatile u8* const first = memory.VirtualBasePointer() + 0x10000;
    volatile u8* const second = memory.VirtualBasePointer() + 0x30000;
    first[0x1000] = 42;
    REQUIRE(memory.BackingBasePointer()[0x1000] == 42);
    REQUIRE(second[0x1000] == 42);

    memory.MarkUsed(0x9000, 0x1000);
    REQUIRE(second[0x9000] == 0);
    second[0x9000] = 73;
    REQUIRE(first[0x9000] == 73);

    memory.Map(0x50000, 2 * GranuleSize, GranuleSize,
               Common::MemoryPermission::ReadWrite, false);
    REQUIRE(VirtualQuery(memory.VirtualBasePointer() + 0x50000, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);
    memory.MarkUsed(2 * GranuleSize + 0x1000, 0x1000);
    REQUIRE(VirtualQuery(memory.BackingBasePointer() + GranuleSize, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);
    REQUIRE(VirtualQuery(memory.BackingBasePointer() + 2 * GranuleSize, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_COMMIT);
    REQUIRE(memory.BackingBasePointer()[2 * GranuleSize + 0x1000] == 0);
    REQUIRE(VirtualQuery(memory.VirtualBasePointer() + 0x50000, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_COMMIT);
    memory.VirtualBasePointer()[0x51000] = 91;
    REQUIRE(memory.BackingBasePointer()[2 * GranuleSize + 0x1000] == 91);

    memory.ClearBackingRegion(4 * GranuleSize, 0x1000, 0);
    REQUIRE(VirtualQuery(memory.BackingBasePointer() + 4 * GranuleSize, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);
    REQUIRE(memory.BackingBasePointer()[4 * GranuleSize] == 0);
    REQUIRE(VirtualQuery(memory.BackingBasePointer() + 4 * GranuleSize, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_COMMIT);
    REQUIRE(VirtualQuery(memory.BackingBasePointer() + 3 * GranuleSize, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);

    memory.Unmap(0x10000, GranuleSize, false);
    REQUIRE(second[0x1000] == 42);
    memory.Map(0x10000, 0, GranuleSize, Common::MemoryPermission::ReadWrite, false);
    REQUIRE(first[0x9000] == 73);
    memory.MarkDirty(0, GranuleSize);
    memory.ClearBackingRegion(0, GranuleSize, 0);
    REQUIRE(first[0x1000] == 0);
    REQUIRE(second[0x9000] == 0);
}

TEST_CASE("Deferred backing commits on virtual access fault and preserves ReadOnly traps",
          "[common][host_memory][host_commit]") {
    constexpr size_t GranuleSize = 64_KiB;
    Common::HostMemory memory{8_MiB, 1_GiB, Common::HostMemory::BackingMode::Deferred};
    REQUIRE(memory.HasLazyCommit());

    memory.Map(0x10000, 5 * GranuleSize, GranuleSize, Common::MemoryPermission::ReadWrite, false);
    memory.Map(0x30000, 6 * GranuleSize, GranuleSize, Common::MemoryPermission::ReadWrite, false);
    memory.Map(0x50000, 7 * GranuleSize, GranuleSize, Common::MemoryPermission::ReadWrite, false);

    MEMORY_BASIC_INFORMATION region{};
    REQUIRE(VirtualQuery(memory.BackingBasePointer() + 5 * GranuleSize, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);

    // Writing through virtual view lazily commits its 64 KiB physical granule.
    memory.VirtualBasePointer()[0x11000] = 55;
    REQUIRE(memory.BackingBasePointer()[5 * GranuleSize + 0x1000] == 55);
    REQUIRE(VirtualQuery(memory.BackingBasePointer() + 5 * GranuleSize, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_COMMIT);

    // Non-RW protection commits untouched pages so VirtualProtect takes effect, while RW leaves
    // MEM_RESERVE pages uncommitted.
    memory.Protect(0x30000, GranuleSize, Common::MemoryPermission::Read);
    REQUIRE(VirtualQuery(memory.VirtualBasePointer() + 0x30000, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_COMMIT);
    REQUIRE(region.Protect == PAGE_READONLY);
    REQUIRE(memory.VirtualBasePointer()[0x31000] == 0);

    memory.Protect(0x30000, GranuleSize, Common::MemoryPermission::ReadWrite);
    memory.VirtualBasePointer()[0x31000] = 88;
    REQUIRE(memory.BackingBasePointer()[6 * GranuleSize + 0x1000] == 88);

    memory.Protect(0x50000, GranuleSize, Common::MemoryPermission::ReadWrite);
    REQUIRE(VirtualQuery(memory.VirtualBasePointer() + 0x50000, &region, sizeof(region)) ==
            sizeof(region));
    REQUIRE(region.State == MEM_RESERVE);
}

