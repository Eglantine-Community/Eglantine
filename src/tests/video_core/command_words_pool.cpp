// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <atomic>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "video_core/command_words_pool.h"

TEST_CASE("Command word blocks reuse their size class and obey a global byte limit",
          "[video_core][command_words_pool]") {
    namespace Pool = Tegra::CommandWordsPool;
    Pool::Trim();
    void* block = Pool::Allocate(5000);
    Pool::Deallocate(block, 5000);
    REQUIRE(Pool::PooledBytes() == 8192);
    void* reused = Pool::Allocate(6000);
    REQUIRE(reused == block);
    REQUIRE(Pool::PooledBytes() == 0);
    Pool::Deallocate(reused, 6000);
    Pool::Trim();

    std::vector<std::pair<void*, std::size_t>> live;
    for (std::size_t size : {std::size_t{1} << 20, std::size_t{2} << 20,
                             std::size_t{4} << 20}) {
        for (int i = 0; i < 8; ++i) {
            live.emplace_back(Pool::Allocate(size), size);
        }
    }
    for (const auto& [pointer, size] : live) {
        Pool::Deallocate(pointer, size);
        REQUIRE(Pool::PooledBytes() <= Pool::MaxPooledBytes);
    }
    void* large = Pool::Allocate((std::size_t{4} << 20) + 1);
    const auto before = Pool::PooledBytes();
    Pool::Deallocate(large, (std::size_t{4} << 20) + 1);
    REQUIRE(Pool::PooledBytes() == before);
    Pool::Trim();
    REQUIRE(Pool::PooledBytes() == 0);
    Tegra::CommandWordsAllocator<std::uint64_t> allocator;
    REQUIRE_THROWS_AS(allocator.allocate(std::numeric_limits<std::size_t>::max()),
                      std::bad_array_new_length);
}

TEST_CASE("Command word blocks preserve distinct live allocations across threads",
          "[video_core][command_words_pool]") {
    namespace Pool = Tegra::CommandWordsPool;
    Pool::Trim();
    std::atomic<bool> correct{true};
    {
        std::vector<std::jthread> threads;
        for (int t = 1; t <= 8; ++t) {
            threads.emplace_back([&, t] {
                for (int i = 0; i < 500; ++i) {
                    const std::size_t size = 4000 + std::size_t(i % 5) * 1000;
                    auto* bytes = static_cast<unsigned char*>(Pool::Allocate(size));
                    std::memset(bytes, t, size);
                    std::this_thread::yield();
                    for (std::size_t j = 0; j < size; ++j) {
                        if (bytes[j] != t) {
                            correct.store(false);
                        }
                    }
                    Pool::Deallocate(bytes, size);
                }
            });
        }
    }
    REQUIRE(correct.load());
    REQUIRE(Pool::PooledBytes() <= Pool::MaxPooledBytes);
    Pool::Trim();
    REQUIRE(Pool::PooledBytes() == 0);
}
