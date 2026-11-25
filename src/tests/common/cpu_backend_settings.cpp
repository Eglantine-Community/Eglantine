// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>

#include "common/settings.h"

TEST_CASE("ScarletNX CPU settings are opt-in", "[common][settings][cpu-backend]") {
    using Settings::CpuBackend;
#ifdef HAS_NCE
    constexpr auto default_backend = CpuBackend::Nce;
#else
    constexpr auto default_backend = CpuBackend::Dynarmic;
#endif
    Settings::Linkage linkage;
    Settings::SwitchableSetting<CpuBackend, true> backend{linkage, default_backend, "cpu_backend",
                                                         Settings::Category::Cpu};
    CHECK(Settings::values.cpu_backend.GetDefault() == default_backend);

    backend.LoadString("0");
    CHECK(backend.GetValue() == CpuBackend::Dynarmic);
    backend.LoadString("1");
    CHECK(backend.GetValue() == default_backend);
    backend.LoadString("2");
#ifdef HAS_SCARLETNX
    CHECK(backend.GetValue() == CpuBackend::ScarletNx);
#else
    CHECK(backend.GetValue() == default_backend);
#endif
    backend.LoadString("not a CPU backend");
    CHECK(backend.GetValue() == default_backend);
}
