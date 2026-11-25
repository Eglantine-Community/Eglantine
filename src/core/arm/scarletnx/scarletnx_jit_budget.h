// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <charconv>
#include <cstddef>
#include <optional>
#include <string_view>
#include <system_error>

namespace Core {

inline constexpr std::size_t ScarletNxDefaultJitBudget = 2048;
inline constexpr std::size_t ScarletNxMaxJitBudget = 8192;

inline std::optional<std::size_t> ParseScarletNxJitBudget(const char* option) {
    if (!option) {
        return ScarletNxDefaultJitBudget;
    }
    const std::string_view text{option};
    const auto end = text.data() + text.size();
    std::size_t budget{};
    const auto [ptr, error] = std::from_chars(text.data(), end, budget);
    if (error != std::errc{} || ptr != end || budget < ScarletNxDefaultJitBudget ||
        budget > ScarletNxMaxJitBudget) {
        return std::nullopt;
    }
    return budget;
}

} // namespace Core
