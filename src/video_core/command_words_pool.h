// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <cstddef>
#include <limits>
#include <new>

namespace Tegra {

// Command snapshot allocation reuse from Lichii 8dcea11bf6. Copying and ownership
// stay on their existing threads; only unused allocations are cached.
namespace CommandWordsPool {
inline constexpr std::size_t MaxPooledBytes = std::size_t{16} << 20;
[[nodiscard]] void* Allocate(std::size_t bytes);
void Deallocate(void* pointer, std::size_t bytes) noexcept;
void Trim() noexcept;
[[nodiscard]] std::size_t PooledBytes() noexcept;
} // namespace CommandWordsPool

template <typename T>
struct CommandWordsAllocator {
    using value_type = T;
    static_assert(alignof(T) <= __STDCPP_DEFAULT_NEW_ALIGNMENT__);

    CommandWordsAllocator() noexcept = default;
    template <typename U>
    CommandWordsAllocator(const CommandWordsAllocator<U>&) noexcept {}

    [[nodiscard]] T* allocate(std::size_t count) {
        if (count > std::numeric_limits<std::size_t>::max() / sizeof(T)) {
            throw std::bad_array_new_length{};
        }
        return static_cast<T*>(CommandWordsPool::Allocate(count * sizeof(T)));
    }
    void deallocate(T* pointer, std::size_t count) noexcept {
        CommandWordsPool::Deallocate(pointer, count * sizeof(T));
    }
    template <typename U>
    bool operator==(const CommandWordsAllocator<U>&) const noexcept {
        return true;
    }
};

} // namespace Tegra
