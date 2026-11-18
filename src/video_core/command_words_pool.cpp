// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <mutex>
#include <vector>

#include "video_core/command_words_pool.h"

namespace Tegra::CommandWordsPool {
namespace {
constexpr std::size_t SmallestBlock = std::size_t{4} << 10;
constexpr std::size_t ClassCount = 11;
constexpr std::size_t MaxBlocksPerClass = 32;
constexpr std::size_t MaxBytesPerClass = std::size_t{8} << 20;

bool Enabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("EDEN_NX2_COMMAND_WORDS_POOL");
        return value == nullptr || value[0] != '0';
    }();
    return enabled;
}

struct Pool {
    Pool() {
        for (auto& blocks : free) {
            blocks.reserve(MaxBlocksPerClass);
        }
    }
    ~Pool() {
        for (auto& blocks : free) {
            for (void* block : blocks) {
                ::operator delete(block);
            }
        }
    }
    std::mutex mutex;
    std::array<std::vector<void*>, ClassCount> free;
    std::size_t bytes{};
};

Pool& GetPool() {
    static Pool pool;
    return pool;
}

std::size_t ClassOf(std::size_t bytes) {
    const std::size_t blocks = bytes == 0 ? 1 : 1 + (bytes - 1) / SmallestBlock;
    return static_cast<std::size_t>(std::bit_width(blocks - 1));
}
} // namespace

void* Allocate(std::size_t bytes) {
    const std::size_t index = ClassOf(bytes);
    if (!Enabled() || index >= ClassCount) {
        return ::operator new(bytes);
    }
    Pool& pool = GetPool();
    const std::size_t block_bytes = SmallestBlock << index;
    {
        std::scoped_lock lock{pool.mutex};
        if (auto& blocks = pool.free[index]; !blocks.empty()) {
            void* block = blocks.back();
            blocks.pop_back();
            pool.bytes -= block_bytes;
            return block;
        }
    }
    return ::operator new(block_bytes);
}

void Deallocate(void* pointer, std::size_t bytes) noexcept {
    if (pointer == nullptr) {
        return;
    }
    const std::size_t index = ClassOf(bytes);
    if (Enabled() && index < ClassCount) {
        Pool& pool = GetPool();
        const std::size_t block_bytes = SmallestBlock << index;
        const std::size_t max_blocks = (std::min)(MaxBlocksPerClass,
                                                 MaxBytesPerClass / block_bytes);
        std::scoped_lock lock{pool.mutex};
        if (auto& blocks = pool.free[index]; blocks.size() < max_blocks &&
            pool.bytes <= MaxPooledBytes - block_bytes) {
            blocks.push_back(pointer);
            pool.bytes += block_bytes;
            return;
        }
    }
    ::operator delete(pointer);
}

void Trim() noexcept {
    Pool& pool = GetPool();
    std::scoped_lock lock{pool.mutex};
    for (auto& blocks : pool.free) {
        for (void* block : blocks) {
            ::operator delete(block);
        }
        blocks.clear();
    }
    pool.bytes = 0;
}

std::size_t PooledBytes() noexcept {
    Pool& pool = GetPool();
    std::scoped_lock lock{pool.mutex};
    return pool.bytes;
}

} // namespace Tegra::CommandWordsPool
