// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-License-Identifier: GPL-3.0-or-later

#include <limits>
#include <vector>
#include <catch2/catch_test_macros.hpp>

#include "common/scope_exit.h"
#include "core/hle/service/nvdrv/devices/nx2_stub_device.h"

TEST_CASE("NX2 sparse aliases cover the entire tile backing", "[nx2_memory]") {
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    constexpr u64 base = 0xf0000000;
    constexpr u64 device = 0x90000000;
    registry.RecordDeviceRange(base, 0x4000, device);
    registry.RecordDeviceRange(base + 0x1000, 0x1000, device + 0x10000);
    SCOPE_EXIT {
        registry.ReleaseBacking(base, 0x4000);
        registry.ReleaseBacking(base + 0x1000, 0x1000);
    };

    // A small tile fits the most recent mapping; a tile crossing its end must use the outer one.
    CHECK(registry.DeviceAddressOf(base + 0x1800, 0x100) == device + 0x10800);
    CHECK(registry.DeviceAddressOf(base + 0x1800, 0x1000) == device + 0x1800);
    CHECK(registry.DeviceAddressOf(base, 0x4000) == device);
    CHECK_FALSE(registry.DeviceAddressOf(base + 0x3000, 0x2000));
    CHECK_FALSE(registry.DeviceAddressOf(base, 0));
    CHECK_FALSE(registry.DeviceAddressOf(base, std::numeric_limits<u64>::max()));
    CHECK_FALSE(registry.DeviceAddressOf(std::numeric_limits<u64>::max(), 1));
}

TEST_CASE("NX2 released backing cannot supply a sparse alias", "[nx2_memory]") {
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    constexpr u64 base = 0xf1000000;
    registry.RecordDeviceRange(base, 0x1000, 0xa0000000);
    REQUIRE(registry.DeviceAddressOf(base, 0x1000));
    registry.ReleaseBacking(base, 0x1000);
    CHECK_FALSE(registry.DeviceAddressOf(base, 1));
}

TEST_CASE("NX2 destroyed memory gets fresh backing when its address is reused", "[nx2_memory]") {
    auto& registry = Service::Nvidia::Devices::Nx2GpuMemoryRegistry::Get();
    constexpr u64 address = 0xf2000000;
    constexpr u64 size = 0x2000;
    constexpr size_t asid = 0x1234;
    u64 next_device = 0xb0000000;
    u64 bound_device = 0;
    bool owns_backing = false;
    std::vector<char> operations;
    registry.AddListener(
        &operations,
        [&](u64, u64 cpu, u64 bytes) {
            if (cpu != address || bytes != size) {
                return;
            }
            bound_device = registry.AcquireDeviceRange(asid, cpu, bytes, [&] {
                next_device += 0x20000;
                return next_device;
            });
            owns_backing = true;
            registry.RecordDeviceRange(cpu, bytes, bound_device);
            operations.push_back('m');
        },
        [&](u64 gpu, u64 bytes) {
            if (gpu == address && bytes == size) {
                operations.push_back('u');
            }
        },
        [&](u64 cpu, u64 bytes) {
            if (cpu == address && bytes == size && owns_backing) {
                CHECK(registry.ReleaseDeviceRange(asid, cpu, bytes));
                owns_backing = false;
                operations.push_back('r');
            }
        });
    SCOPE_EXIT {
        registry.Unregister(address, size);
        registry.RemoveListener(&operations);
    };

    registry.Register(address, size);
    const u64 old_device = bound_device;
    REQUIRE(registry.DeviceAddressOf(address, size) == old_device);
    registry.Unregister(address, size);
    CHECK_FALSE(registry.DeviceAddressOf(address, size));
    REQUIRE(operations == std::vector<char>{'m', 'u', 'r'});

    // The old physical pages can now be ordinary actor data; this registration needs a new map.
    registry.Register(address, size);
    CHECK(bound_device != old_device);
    CHECK(registry.DeviceAddressOf(address, size) == bound_device);
}
