// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

namespace Tegra {

// Semaphore reduction ops (ADD, MIN, MAX, INC, DEC, AND, OR, XOR) shared by QMD releases and the
// compute report semaphore.
inline std::uint32_t ReduceSemaphore(std::uint32_t op, bool is_signed, std::uint32_t value,
                                     std::uint32_t payload) {
    switch (op) {
    case 0: return value + payload;
    case 1: return is_signed ? std::uint32_t((std::min)(std::int32_t(value), std::int32_t(payload)))
                             : (std::min)(value, payload);
    case 2: return is_signed ? std::uint32_t((std::max)(std::int32_t(value), std::int32_t(payload)))
                             : (std::max)(value, payload);
    case 3: return value >= payload ? 0 : value + 1;
    case 4: return value == 0 || value > payload ? payload : value - 1;
    case 5: return value & payload;
    case 6: return value | payload;
    default: return value ^ payload;
    }
}

// Native QMD 3.0 dependency scheduling; nonempty grids require a dispatch backend.
class NativeQmdScheduler {
public:
    using Word = std::uint32_t;
    using Address = std::uint64_t;
    using Descriptor = std::array<Word, 64>;
    struct RejectShaderDispatch { bool operator()(const Descriptor&) const { return false; } };
    // Optional diagnostics: called with "load"/"count"/"defer"/"run" events.
    std::function<void(std::string_view, Address, std::int32_t)> trace;
    // Optional diagnostics: called with the QMD, slot and raw words of a release it skips.
    std::function<void(Address, std::size_t, std::span<const Word, 4>)> skipped_release;
    // QMD V03_00 release flags: 7:0 address high, 19 membar, 22:20 reduction op, 23 enable,
    // 25:24 reduction format, 26 reduction enable, 28:27 non-blocking interrupt, 29 64-bit
    // payload, 31:30 structure size (0 four words with timestamp, 1 one word, 2 two words).
    static bool IsDecodableRelease(Word flags) {
        const Word size = flags >> 30;
        if (flags & (1U << 29)) return false;
        if (flags & (1U << 26)) return size == 1 && ((flags >> 24) & 3) < 2;
        return size <= 1;
    }
    template <typename Read, typename Release>
    std::string_view Signal(Address address, Word argument, Read read, Release release) {
        return Signal(address, argument, read, release, RejectShaderDispatch{});
    }
    template <typename Read, typename Release, typename Dispatch>
    std::string_view Signal(Address address, Word argument, Read read, Release release, Dispatch dispatch) {
        struct Pending { Address address; Word action; Word count; };
        std::string_view error;
        std::array<Pending, 256> pending{};
        std::size_t count = 1;
        pending[0] = {address, argument & 15, ((argument >> 14) & 0x3ff) + 1};
        for (std::size_t i = 0; i < count; ++i) {
            const auto request = pending[i];
            State* state_ptr = states.Find(request.address);
            if (!state_ptr) {
                if (states.LiveCount() >= MaxStates) {
                    // Finished QMDs with nothing deferred are only history, drop them instead of stalling.
                    states.SweepFinished();
                    if (states.LiveCount() >= MaxStates) return "QMD cache limit";
                }
                state_ptr = states.Insert(request.address);
            }
            auto& state = *state_ptr;
            Descriptor local_qmd{};
            bool just_loaded = false;
            if (request.action == 3 || request.action == 2) {
                if (request.action == 3 || !state.loaded) {
                    if (!read(request.address, local_qmd)) return "unmapped QMD";
                    just_loaded = true;
                    const auto& qmd = local_qmd;
                    if ((qmd[18] & 0xff) != 0x30 || (qmd[4] & 0x100)) return "unsupported QMD version or queue";
                    if constexpr (std::is_same_v<Dispatch, RejectShaderDispatch>) {
                        if (qmd[12] && (qmd[13] & 0xffff) && (qmd[14] & 0xffff))
                            return "native shader dispatch required";
                    }
                    state.dependencies = std::int32_t((qmd[4] >> 14) & 0xffff) + state.deferred;
                    if (trace) trace("base", request.address, std::int32_t((qmd[4] >> 14) & 0xffff));
                    if (trace) trace("load", request.address, state.dependencies);
                    state.deferred = 0;
                    state.loaded = true;
                    state.completed = false;
                }
                state.scheduled = true;
            } else if (request.action == 7 || request.action == 12) {
                const auto delta = request.action == 7 ? -std::int32_t(request.count) : std::int32_t(request.count);
                if (!state.loaded || state.completed) {
                    state.deferred += delta;
                    if (trace) trace("defer", request.address, state.deferred);
                    continue;
                }
                state.dependencies += delta;
                if (trace) trace("count", request.address, state.dependencies);
            } else if (request.action == 0 || request.action == 1) {
                continue;
            } else {
                return "unsupported PCAS action";
            }
            if (!state.loaded || !state.scheduled || state.completed || state.dependencies > 0) continue;
            if (state.dependencies < 0) return "QMD dependency underflow";
            // The SDK patches release payloads while a scheduled QMD is
            // waiting for its remaining dependencies. Fetch them at execution unless just read.
            if (!just_loaded && !read(request.address, local_qmd)) return "unmapped QMD";
            const auto& qmd = local_qmd;
            if ((qmd[18] & 0xff) != 0x30 || (qmd[4] & 0x100)) return "unsupported QMD version or queue";
            // A program Eden can't run is skipped, but the QMD still completes: its releases
            // are what other queues and the CPU wait on (the console never fails a launch).
            const bool dispatched = !(qmd[12] && (qmd[13] & 0xffff) && (qmd[14] & 0xffff)) || dispatch(qmd);
            if (!dispatched) error = "native shader dispatch required";
            if (trace) trace("run", request.address, dispatched ? 1 : 0);
            state.completed = true;
            for (std::size_t slot = 0; slot < ReleaseSlots.size(); ++slot) {
                const Word base = ReleaseSlots[slot];
                const Word flags = qmd[base + 1];
                if (!(flags & (1U << 23))) continue;
                // Skip only this slot: the others are what other queues and the CPU wait on.
                if (!IsDecodableRelease(flags)) {
                    if (skipped_release) skipped_release(request.address, slot, std::span<const Word, 4>{&qmd[base], 4});
                    if (error.empty()) error = "unsupported QMD release format";
                    continue;
                }
                const Address destination = (Address{flags & 0xff} << 32) | qmd[base];
                if (!release(destination, qmd[base + 2], flags)) return "unmapped QMD release";
            }
            if (qmd[16] & 1) {
                const Word action = (qmd[16] >> 1) & 7;
                if (action != 4 && action != 3 && action != 1) return "unsupported dependent QMD action";
                if (count == pending.size()) return "QMD chain limit";
                pending[count++] = {Address{qmd[15]} << 8, action == 4 ? 7U : action == 3 ? 3U : 2U, 1};
            }
            if (qmd[16] & (1U << 5)) return "second dependent QMD unsupported";
        }
        return error;
    }
private:
    // Release slots (address low, flags | address high, payload). NVN2 patches a third one at
    // word 52 for queue syncs (e.g. a compute queue releasing a semaphore another queue waits on).
    static constexpr std::array<Word, 3> ReleaseSlots{24, 28, 52};

    static constexpr std::size_t MaxStates = 4096;
    static constexpr std::size_t TableSize = 8192;
    static constexpr std::size_t TableMask = TableSize - 1;

    struct State {
        std::int32_t dependencies{};
        std::int32_t deferred{};
        bool loaded{};
        bool scheduled{};
        bool completed{};
    };
    struct StateTable {
        struct Bucket {
            Address address{};
            State state{};
            std::uint8_t occupied{}; // 0 = empty, 1 = live, 2 = tombstone
        };
        static std::size_t Hash(Address addr) noexcept {
            return static_cast<std::size_t>(((addr ^ (addr >> 8)) * 0x9e3779b97f4a7c15ULL) >>
                                            51) &
                   TableMask;
        }
        std::size_t LiveCount() const noexcept {
            return live_count;
        }
        State* Find(Address addr) noexcept {
            std::size_t idx = Hash(addr);
            for (std::size_t probes = 0; probes < TableSize; ++probes) {
                auto& b = buckets[idx];
                if (b.occupied == 0) {
                    return nullptr;
                }
                if (b.occupied == 1 && b.address == addr) {
                    return &b.state;
                }
                idx = (idx + 1) & TableMask;
            }
            return nullptr;
        }
        State* Insert(Address addr) noexcept {
            std::size_t idx = Hash(addr);
            for (std::size_t probes = 0; probes < TableSize; ++probes) {
                auto& b = buckets[idx];
                if (b.occupied != 1) {
                    if (b.occupied == 2) {
                        --tombstones;
                    }
                    b.address = addr;
                    b.state = {};
                    b.occupied = 1;
                    ++live_count;
                    return &b.state;
                }
                idx = (idx + 1) & TableMask;
            }
            return nullptr;
        }
        void SweepFinished() {
            for (auto& b : buckets) {
                if (b.occupied == 1 && b.state.completed && b.state.deferred == 0) {
                    b.occupied = 2;
                    --live_count;
                    ++tombstones;
                }
            }
            if (live_count + tombstones >= 6144) {
                std::vector<std::pair<Address, State>> kept;
                kept.reserve(live_count);
                for (const auto& b : buckets) {
                    if (b.occupied == 1) {
                        kept.emplace_back(b.address, b.state);
                    }
                }
                buckets.fill({});
                live_count = 0;
                tombstones = 0;
                for (const auto& [addr, st] : kept) {
                    *Insert(addr) = st;
                }
            }
        }
        std::array<Bucket, TableSize> buckets{};
        std::size_t live_count{};
        std::size_t tombstones{};
    };
    // One table for every channel: a QMD's dependency counter is state of the QMD itself, and
    // title 1 schedules a QMD on one channel while another decrements it (a decrement that
    // arrived first was kept by the other channel's scheduler and the QMD never launched).
    static inline StateTable states;
};
} // namespace Tegra
