// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>

#include "common/common_types.h"

namespace Core {
class System;
}

namespace Tegra::Engines {
class EngineInterface;
}

namespace VideoCommon {

// Splits the GPU thread in two (NX2_GPU_THREAD_SPLIT.md). The GPU thread (the front) takes the
// CPU's commands, parses command lists and runs the puller; engine method calls and everything
// behind them (register state, macros, draws, the rasterizer and its caches) run in order on a
// second thread (the back), fed through a single-producer single-consumer ring. Work the front
// would do on back-owned state is deferred into the same ring; the front waits for the back to
// catch up (Drain) only where it needs a result from that state, e.g. a semaphore it acquires.
// On for Switch 2 titles with the 3D and compute engines' macros on the front, against copies of
// the registers they read (Maxwell3D and KeplerCompute Front*), which sends the writes they make.
// EDEN_NX2_SPLIT=1 keeps the macros on the back; EDEN_NX2_SPLIT=0 runs everything on one thread.
class GpuSplit {
public:
    static GpuSplit& Get() {
        return instance;
    }

    /// Whether the front sends work to the back (front thread only).
    [[nodiscard]] bool Active() const {
        return active;
    }
    /// Whether the front runs the engines' macros (EDEN_NX2_SPLIT=2).
    [[nodiscard]] bool FrontMacros() const {
        return front_macros;
    }

    /// Whether the calling thread is the back.
    [[nodiscard]] static bool OnBackThread() {
        return on_back_thread;
    }
    /// Marks the calling thread as the GPU thread (the front, or the only one without the split).
    static void MarkGpuThread() {
        on_gpu_thread = true;
    }
    /// Whether the calling thread owns the Vulkan scheduler: the back while it runs, else the
    /// GPU thread. Other threads (CPU cores, services) must not record or submit work.
    [[nodiscard]] static bool OwnsScheduler() {
        return Get().BackRunning() ? on_back_thread : on_gpu_thread;
    }
    /// Called on the back before it sleeps waiting for work (the rasterizer submits deferred work).
    void SetIdleHook(void (*hook)(void*), void* argument) {
        idle_hook = hook;
        idle_argument = argument;
    }
    /// Any thread: whether the back thread runs (false before it starts and once it stopped).
    [[nodiscard]] bool BackRunning() const {
        return back_running.load(std::memory_order_acquire);
    }
    /// Whether the back is done with the Vulkan scheduler until it gets more work: set after its
    /// idle hook ran, cleared before it executes entries.
    [[nodiscard]] bool BackQuiet() const {
        return back_quiet.load(std::memory_order_acquire) ||
               !back_running.load(std::memory_order_acquire);
    }
    /// Any thread: has the back call the idle hook soon, between entries or at once if it sleeps.
    void RequestIdleHook() {
        hook_requested.store(true, std::memory_order_seq_cst);
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }

    /// Starts the back thread once the split is wanted (EDEN_NX2_SPLIT=1, a Switch 2 title,
    /// commands read without cache flushes). Called by the front between commands.
    void MaybeStart(Core::System& system);
    void Stop();

    /// An engine method write, executed as DmaPusher would (method sink, ConsumeSink, call).
    /// `segment`: the engine's current_dma_segment for the call, when the engine reads it.
    void Method(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument, bool is_last,
                const GPUVAddr* segment) {
        SelectEngine(engine);
        const size_t words = segment != nullptr ? 2 : 1;
        u64* const entry = Reserve(words);
        entry[0] = static_cast<u64>(Kind::Method) | (is_last ? IsLast : 0) |
                   (segment != nullptr ? HasSegment : 0) | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        if (segment != nullptr) {
            entry[1] = *segment;
        }
        Commit(words);
    }
    void MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                     u32 amount, u32 methods_pending, const GPUVAddr* segment);
    /// A write made by a macro: CallMethod without the method sink, as the engines' macros do.
    void MacroWrite(Tegra::Engines::EngineInterface* engine, u32 method, u32 argument) {
        SelectEngine(engine);
        u64* const entry = Reserve(1);
        entry[0] = static_cast<u64>(Kind::Method) | IsLast | Direct | (u64{method & 0xffff} << 8) |
                   (u64{argument} << 32);
        Commit(1);
    }

    /// Runs `func` on the back after everything sent before it. Must be trivially copyable.
    template <typename Func>
    void Defer(const Func& func) {
        static_assert(std::is_trivially_copyable_v<Func> && alignof(Func) <= alignof(u64));
        constexpr size_t payload = (sizeof(Func) + sizeof(u64) - 1) / sizeof(u64);
        u64* const entry = Reserve(2 + payload);
        entry[0] = static_cast<u64>(Kind::Call) | (u64{payload} << 8);
        entry[1] = reinterpret_cast<u64>(&CallThunk<Func>);
        std::memcpy(entry + 2, &func, sizeof(Func));
        Commit(2 + payload);
    }
    void DeferFunction(std::function<void()>&& func);

    /// Runs `func` on the back and waits for it: for work on back-owned state, such as a buffer
    /// cache download that records into the Vulkan scheduler. A drained back still submits from
    /// its idle hook, so the front never touches that state itself.
    template <typename Func>
    void Run(const Func& func) {
        if (on_back_thread || !active) {
            func();
            return;
        }
        Defer(func);
        Drain();
    }

    /// EDEN_NX2_SPLIT_CHECK=1 (debug): reports, with its stack, a thread other than the back
    /// using back-owned state (the Vulkan scheduler) while the back runs entries.
    static inline bool check_owner{};
    static void CheckOwner(const char* where);

    /// Makes what was sent visible to the back.
    void Publish();
    /// Waits until the back executed everything sent so far.
    void Drain();

    static constexpr size_t ReleaseSets = 8192;
    static constexpr size_t ReleaseWays = 8;
    static constexpr size_t ReleaseMask = ReleaseSets - 1;
    static size_t ReleaseBucket(u64 aligned4_address) {
        return static_cast<size_t>(((aligned4_address >> 2) * 0x9e3779b97f4a7c15ULL) >> 51) &
               ReleaseMask;
    }

    u64& RefreshConsumedCache() {
        consumed_cache = consumed.load(std::memory_order_acquire);
        return consumed_cache;
    }

    /// Computes/copies can write GPU-written command lists.
    void NoteWriter() {
        last_writer_position = write_position;
        last_unknown_writer_position = write_position;
    }
    void NoteComputeWriter() {
        last_writer_position = write_position;
        last_compute_writer_position = write_position;
        last_all_compute_writer_position = write_position;
    }
    void NoteAllComputeWriter() {
        last_all_compute_writer_position = write_position;
    }
    template <typename Translator>
    void NoteWriterGpuRange(u64 gpu_addr, u64 size_bytes, Translator&& gpu_to_cpu) {
        if (!active || gpu_addr == 0 || size_bytes == 0) {
            return;
        }
        const u64 end = gpu_addr + size_bytes;
        if (end < gpu_addr) {
            NoteWriter();
            return;
        }
        if ((gpu_addr & ~u64{0xfff}) == ((end - 1) & ~u64{0xfff})) [[likely]] {
            if (const auto dev = gpu_to_cpu(gpu_addr); dev && *dev != 0) {
                NoteWriterRange(*dev, size_bytes);
            } else {
                NoteWriter();
            }
            return;
        }
        if ((end - (gpu_addr & ~u64{0xfff})) > 0x400000) {
            NoteWriter();
            return;
        }
        u64 run_dev = 0;
        u64 run_len = 0;
        u32 run_count = 0;
        for (u64 page = gpu_addr & ~u64{0xfff}; page < end; page += 0x1000) {
            const u64 seg_start = (std::max)(page, gpu_addr);
            const u64 seg_end = (std::min)(page + 0x1000, end);
            const u64 seg_len = seg_end - seg_start;
            const auto dev = gpu_to_cpu(seg_start);
            if (!dev || *dev == 0) {
                if (run_len > 0) {
                    NoteWriterRange(run_dev, run_len);
                }
                NoteWriter();
                return;
            }
            if (run_len == 0) {
                run_dev = *dev;
                run_len = seg_len;
            } else if (*dev == run_dev + run_len) {
                run_len += seg_len;
            } else {
                NoteWriterRange(run_dev, run_len);
                if (++run_count >= 16) {
                    NoteWriter();
                    return;
                }
                run_dev = *dev;
                run_len = seg_len;
            }
        }
        if (run_len > 0) {
            NoteWriterRange(run_dev, run_len);
        }
    }
    void NoteWriterRange(u64 device_start, u64 size_bytes) {
        if (!active || device_start == 0 || size_bytes == 0) {
            return;
        }
        const u64 pos = write_position;
        if (pos > last_any_writer_position) {
            last_any_writer_position = pos;
        }
        if (size_bytes > 4096) {
            const u64 end = device_start + size_bytes;
            size_t victim = 0;
            size_t stale = LargeWriteCount;
            for (size_t i = 0; i < LargeWriteCount; ++i) {
                auto& r = writer_large_ranges[i];
                if (r.start == device_start && r.end == end) {
                    if (pos > r.pos) {
                        r.pos = pos;
                    }
                    return;
                }
                if (r.pos <= consumed_cache && stale == LargeWriteCount) {
                    stale = i;
                }
                if (r.pos < writer_large_ranges[victim].pos) {
                    victim = i;
                }
            }
            if (stale != LargeWriteCount) {
                writer_large_ranges[stale] = {device_start, end, pos};
                return;
            }
            RefreshConsumedCache();
            if (writer_large_ranges[victim].pos > consumed_cache &&
                writer_large_ranges[victim].pos > writer_large_overflow_pos) {
                writer_large_overflow_pos = writer_large_ranges[victim].pos;
            }
            writer_large_ranges[victim] = {device_start, end, pos};
            return;
        }
        auto refresh = [this] { RefreshConsumedCache(); };
        const u64 end = device_start + size_bytes;
        const u64 page_lo = device_start >> 12;
        const u64 page_hi = (end - 1) >> 12;
        for (u64 page = page_lo; page <= page_hi; ++page) {
            writer_pages.Record(page << 12, pos, consumed_cache, refresh);
        }
        const u64 line_lo = device_start & ~u64{0x3f};
        const u64 line_hi = (end - 1) & ~u64{0x3f};
        for (u64 line = line_lo;; line += 64) {
            writer_lines.Record(line, pos, consumed_cache, refresh,
                                ComputeLineWordMask(line, device_start, end));
            if (line == line_hi) {
                break;
            }
        }
    }
    [[nodiscard]] u64 LastWriterPosition() const {
        return last_writer_position;
    }
    [[nodiscard]] u64 ActiveLastWriterPosition() const {
        const u64 cur_consumed = consumed.load(std::memory_order_acquire);
        return last_writer_position > cur_consumed
                   ? (std::min)(last_writer_position, write_position)
                   : 0;
    }
    [[nodiscard]] u64 ActiveLastComputeWriterPosition() const {
        const u64 cur_consumed = consumed.load(std::memory_order_acquire);
        return last_compute_writer_position > cur_consumed
                   ? (std::min)(last_compute_writer_position, write_position)
                   : 0;
    }
    [[nodiscard]] u64 ActiveLastAllComputeWriterPosition() const {
        const u64 cur_consumed = consumed.load(std::memory_order_acquire);
        return last_all_compute_writer_position > cur_consumed
                   ? (std::min)(last_all_compute_writer_position, write_position)
                   : 0;
    }
    [[nodiscard]] u64 LastUnknownWriterPosition() const {
        const u64 cur_consumed = consumed.load(std::memory_order_acquire);
        return last_unknown_writer_position > cur_consumed
                   ? (std::min)(last_unknown_writer_position, write_position)
                   : 0;
    }
    void NotePendingSyncpoint(u32 syncpoint_id) {
        if (!active) {
            return;
        }
        if (syncpoint_id < TrackedSyncpoints) {
            syncpoint_positions[syncpoint_id] = write_position;
        } else {
            last_wildcard_syncpoint_position = write_position;
        }
    }
    [[nodiscard]] u64 PendingSyncpointPosition(u32 syncpoint_id) const {
        if (!active) {
            return 0;
        }
        const u64 cur_consumed = consumed.load(std::memory_order_acquire);
        const u64 pos =
            syncpoint_id < TrackedSyncpoints
                ? (std::max)(syncpoint_positions[syncpoint_id], last_wildcard_syncpoint_position)
                : write_position;
        return pos > cur_consumed ? (std::min)(pos, write_position) : 0;
    }
    [[nodiscard]] u64 WriterRangePosition(u64 device_start, u64 size_bytes) const {
        const u64 cur_consumed = consumed.load(std::memory_order_acquire);
        last_wr_diag_kind = 0;
        u64 pos = 0;
        if (writer_large_overflow_pos > cur_consumed) {
            pos = writer_large_overflow_pos;
            last_wr_diag_kind = 5;
        }
        if (device_start == 0 || size_bytes == 0) {
            if (last_any_writer_position > cur_consumed && last_any_writer_position > pos) {
                pos = last_any_writer_position;
                last_wr_diag_kind = 7;
            }
            return pos;
        }
        const u64 query_end = device_start + size_bytes;
        for (const auto& r : writer_large_ranges) {
            if (r.pos > cur_consumed && r.pos > pos && device_start < r.end && r.start < query_end) {
                pos = r.pos;
                last_wr_diag_kind = 4;
            }
        }
        u8 tbl_kind = 0;
        const u64 range_pos = QueryRangePos(writer_pages, writer_lines, device_start, size_bytes,
                                            cur_consumed, last_any_writer_position, &tbl_kind);
        if (range_pos > pos) {
            pos = range_pos;
            last_wr_diag_kind = tbl_kind == 4 ? u8{6} : tbl_kind;
        }
        return pos;
    }
    [[nodiscard]] u8 LastQueryDiagKind() const {
        return last_q_diag_kind;
    }
    [[nodiscard]] u8 LastWriterDiagKind() const {
        return last_wr_diag_kind;
    }
    /// Waits until the back executed everything up to `target` (without flushing idle hooks).
    bool WaitForPosition(u64 target);
    /// Waits until the back executed the compute and copy work sent so far (not what came after).
    /// Returns whether any of it was still queued.
    bool WaitForWriters();

    enum class ReleaseOp : u8 {
        Write = 0,
        Max = 1,
        Add = 2,
    };
    enum class ReleaseValueKind : u8 {
        Unknown = 0,
        Exact = 1,
        MinBound = 2,
    };

    struct ReleaseInfo {
        u64 position{};
        std::optional<u32> payload;
        bool payload_is_lower_bound{};
    };

    /// Records a queued fence/query write targeting `device_address` (0 = unknown address).
    /// When `payload` is provided (or chained from prior releases / idle RAM on `Max`/`Add`), the
    /// release knows either the exact 32-bit value or a guaranteed minimum before `consumed` passes.
    void NotePendingRelease(u64 device_address = 0, u8 source = 0,
                            std::optional<u32> payload = std::nullopt, u64 size_bytes = 4,
                            u8 no_pl_reason = 0, ReleaseOp op = ReleaseOp::Write,
                            std::optional<u32> add_idle_base = std::nullopt) {
        if (!active) {
            return;
        }
        if (device_address == 0) {
            last_wildcard_release_position = write_position;
            RecordPendingReleaseEvent(true, source);
            return;
        }
        const u64 words = size_bytes > 4 ? (std::min<u64>)((size_bytes + 3) / 4, 4ULL) : 1ULL;
        const u64 base4 = device_address & ~u64{3};
        struct ReleaseEval {
            bool has_pl{};
            u32 payload{};
            ReleaseValueKind kind{};
            u8 reason{};
        };
        const auto compute_state = [&](const ReleaseEntry* existing, u64 addr4,
                                       std::optional<u32> word_payload, ReleaseOp word_op,
                                       u8 word_reason) -> ReleaseEval {
            if (word_op == ReleaseOp::Write) {
                if (word_payload.has_value()) {
                    return {true, *word_payload, ReleaseValueKind::Exact, u8{0}};
                }
                return {false, 0U, ReleaseValueKind::Unknown, word_reason};
            }
            if (!word_payload.has_value()) {
                return {false, 0U, ReleaseValueKind::Unknown, word_reason};
            }
            const auto is_idle_for_base = [&]() -> bool {
                if (!add_idle_base.has_value()) {
                    return false;
                }
                if (existing != nullptr && existing->position > consumed_cache) {
                    consumed_cache = consumed.load(std::memory_order_acquire);
                    if (existing->position > consumed_cache) {
                        return false;
                    }
                } else {
                    consumed_cache = consumed.load(std::memory_order_acquire);
                }
                return last_wildcard_release_position <= consumed_cache &&
                       last_unknown_writer_position <= consumed_cache &&
                       WriterRangePosition(addr4, sizeof(u32)) <= consumed_cache &&
                       PendingSyncQueryPosition(addr4, sizeof(u32)) <= consumed_cache &&
                       !HasPendingQueryRange(addr4, sizeof(u32));
            };
            const bool has_active_existing = existing != nullptr && existing->position > consumed_cache;
            const bool can_chain =
                has_active_existing && existing->value_kind != ReleaseValueKind::Unknown &&
                last_unknown_writer_position <= existing->position &&
                WriterRangePosition(addr4, sizeof(u32)) <= existing->position;
            if (word_op == ReleaseOp::Max) {
                if (can_chain && existing->value_kind == ReleaseValueKind::Exact) {
                    const u32 combined = (std::max)(existing->payload, *word_payload);
                    return {true, combined, ReleaseValueKind::Exact, u8{0}};
                }
                if (is_idle_for_base()) {
                    const u32 combined = (std::max)(*add_idle_base, *word_payload);
                    return {true, combined, ReleaseValueKind::Exact, u8{0}};
                }
                if (can_chain) {
                    const u32 combined = (std::max)(existing->payload, *word_payload);
                    return {true, combined, existing->value_kind, u8{0}};
                }
                // `max(mem[addr], payload)` guarantees `mem[addr] >= payload` regardless of prior RAM.
                return {true, *word_payload, ReleaseValueKind::MinBound, u8{0}};
            }
            // ReleaseOp::Add: chain exact queued state or derive exact base from idle RAM.
            if (can_chain && existing->value_kind == ReleaseValueKind::Exact) {
                return {true, existing->payload + *word_payload, ReleaseValueKind::Exact, u8{0}};
            }
            if (is_idle_for_base()) {
                return {true, *add_idle_base + *word_payload, ReleaseValueKind::Exact, u8{0}};
            }
            return {false, 0U, ReleaseValueKind::Unknown, word_reason};
        };
        for (u64 i = 0; i < words; ++i) {
            const u64 addr4 = base4 + i * 4;
            const std::optional<u32> word_payload = (i == 0) ? payload : std::nullopt;
            const ReleaseOp word_op = (i == 0) ? op : ReleaseOp::Write;
            const u8 word_reason =
                word_payload.has_value() ? u8{0} : (i > 0 ? u8{11} : no_pl_reason);
            auto& set = release_positions[ReleaseBucket(addr4)];
            ReleaseEntry* victim = &set[0];
            ReleaseEntry* stale = nullptr;
            bool updated = false;
            for (ReleaseEntry& w : set) {
                if (w.aligned_address == addr4) {
                    if (write_position >= w.position) {
                        const auto [has_pl, pl_val, val_kind, rsn] =
                            compute_state(&w, addr4, word_payload, word_op, word_reason);
                        w = {write_position, addr4, pl_val, has_pl, rsn, val_kind};
                    }
                    updated = true;
                    break;
                }
                if (w.position <= consumed_cache && stale == nullptr) {
                    stale = &w;
                }
                if (w.position < victim->position) {
                    victim = &w;
                }
            }
            if (updated) {
                continue;
            }
            const auto [has_pl, pl_val, val_kind, rsn] =
                compute_state(nullptr, addr4, word_payload, word_op, word_reason);
            const ReleaseEntry entry{write_position, addr4, pl_val, has_pl, rsn, val_kind};
            if (stale != nullptr) {
                *stale = entry;
                continue;
            }
            RefreshConsumedCache();
            if (victim->position <= consumed_cache) {
                *victim = entry;
                continue;
            }
            if (victim->position > last_wildcard_release_position) {
                last_wildcard_release_position = victim->position;
            }
            ++release_table_overflows;
            *victim = entry;
        }
    }
    /// Returns the ring position (and exact 32-bit payload, when known) of any unconsumed release
    /// potentially targeting `device_address`.
    [[nodiscard]] std::optional<ReleaseInfo> PendingRelease(u64 device_address) {
        if (!active) {
            return std::nullopt;
        }
        consumed_cache = consumed.load(std::memory_order_acquire);
        const u64 wildcard_pos =
            last_wildcard_release_position > consumed_cache ? last_wildcard_release_position : 0;
        const u64 aligned4 = device_address & ~u64{3};
        const ReleaseEntry* match = nullptr;
        for (const ReleaseEntry& w : release_positions[ReleaseBucket(aligned4)]) {
            if (w.aligned_address == aligned4 && w.position > consumed_cache &&
                (match == nullptr || w.position > match->position)) {
                match = &w;
            }
        }
        const bool hit_wildcard = wildcard_pos > 0;
        const bool hit_exact = !hit_wildcard && match != nullptr;
        RecordPendingReleaseCheck(hit_wildcard, hit_exact);
        if (match == nullptr && wildcard_pos == 0) {
            return std::nullopt;
        }
        const u64 target_pos =
            (std::max)(match != nullptr ? match->position : 0, wildcard_pos);
        if (match == nullptr || wildcard_pos > match->position) {
            if (hit_exact) {
                RecordPendingReleasePayloadResult(false, 4);
            }
            return ReleaseInfo{(std::min)(target_pos, write_position), std::nullopt};
        }
        std::optional<u32> payload;
        if (!match->has_payload) {
            RecordPendingReleasePayloadResult(false, 1, match->no_pl_reason);
        } else if (WriterRangePosition(aligned4, sizeof(u32)) > match->position) {
            RecordPendingReleasePayloadResult(false, 2);
        } else if (last_unknown_writer_position > match->position) {
            RecordPendingReleasePayloadResult(false, 3);
        } else {
            payload = match->payload;
            RecordPendingReleasePayloadResult(true, last_writer_position > match->position ? 5 : 0);
        }
        return ReleaseInfo{(std::min)(target_pos, write_position), payload,
                           payload.has_value() &&
                               match->value_kind == ReleaseValueKind::MinBound};
    }
    /// Returns the ring position of any unconsumed release targeting `device_address`.
    [[nodiscard]] std::optional<u64> PendingReleasePosition(u64 device_address) {
        if (const auto rel = PendingRelease(device_address)) {
            return rel->position;
        }
        return std::nullopt;
    }
    /// True if an unconsumed release/query on the back may still write `device_address`.
    [[nodiscard]] bool HasPendingRelease(u64 device_address) {
        return PendingReleasePosition(device_address).has_value();
    }

    void RecordPendingReleaseEvent(bool is_wildcard, u8 source);
    void RecordPendingReleaseCheck(bool hit_wildcard, bool hit_exact);
    void RecordPendingReleasePayloadResult(bool ok, u8 reason, u8 no_pl_reason = 0);

    [[nodiscard]] u64 WritePosition() const {
        return write_position;
    }
    [[nodiscard]] u64 ConsumedPosition() const {
        return consumed.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool HasPendingSyncPayloads() const {
        return sync_payload_count != 0;
    }
    void NotePendingSyncPayload(u64 device_address, u32 size_bytes, const void* bytes) {
        if (bytes == nullptr || size_bytes == 0 || size_bytes > 16) {
            NotePendingQuery(device_address, false, size_bytes == 0 ? 16 : size_bytes);
            return;
        }
        const u64 cur_consumed = consumed.load(std::memory_order_acquire);
        while (sync_payload_count > 0 && sync_payloads[sync_payload_head].pos <= cur_consumed) {
            sync_payload_head = (sync_payload_head + 1) & SyncPayloadMask;
            --sync_payload_count;
        }
        if (sync_payload_count == MaxSyncPayloads) {
            NotePendingQuery(device_address, false, size_bytes);
            return;
        }
        const size_t tail = (sync_payload_head + sync_payload_count) & SyncPayloadMask;
        auto& entry = sync_payloads[tail];
        entry.pos = write_position;
        entry.dev_addr = device_address;
        entry.size_bytes = size_bytes;
        std::memcpy(entry.data.data(), bytes, size_bytes);
        ++sync_payload_count;
    }
    void ApplyPendingSyncPayloads(u64 device_addr, void* dst, u64 size, u64 min_pos) const {
        if (sync_payload_count == 0 || size == 0 || dst == nullptr) {
            return;
        }
        auto* dst_bytes = static_cast<u8*>(dst);
        const u64 read_end = device_addr + size;
        for (size_t i = 0; i < sync_payload_count; ++i) {
            const auto& entry = sync_payloads[(sync_payload_head + i) & SyncPayloadMask];
            if (entry.pos <= min_pos) {
                continue;
            }
            const u64 entry_end = entry.dev_addr + entry.size_bytes;
            if (entry_end <= device_addr || entry.dev_addr >= read_end) {
                continue;
            }
            const u64 ov_start = (std::max)(device_addr, entry.dev_addr);
            const u64 ov_end = (std::min)(read_end, entry_end);
            std::memcpy(dst_bytes + (ov_start - device_addr),
                        entry.data.data() + (ov_start - entry.dev_addr),
                        static_cast<size_t>(ov_end - ov_start));
        }
    }

    /// Tracks GPU queries whose host-memory write occurs either synchronously on Back before
    /// `consumed >= position` (`async_hw == false`) or via delayed HW/fence completion (`async_hw`).
    void NotePendingQuery(u64 device_address, bool async_hw = true, u64 size_bytes = 16) {
        if (!active || device_address == 0 || size_bytes == 0) {
            return;
        }
        const u64 pos = write_position;
        const u64 range_end = device_address + size_bytes;
        const u64 end = range_end - 1;
        const u64 page_lo = device_address >> 12;
        const u64 page_hi = end >> 12;
        const u64 line_lo = device_address & ~u64{0x3f};
        const u64 line_hi = end & ~u64{0x3f};
        if (!async_hw) {
            if (pos > last_sync_query_note_position) {
                last_sync_query_note_position = pos;
            }
            auto refresh = [this] { RefreshConsumedCache(); };
            for (u64 page = page_lo; page <= page_hi; ++page) {
                sync_query_pages.Record(page << 12, pos, consumed_cache, refresh);
            }
            if (size_bytes <= 256) {
                const u64 sq_lo = device_address & ~decltype(sync_query_lines)::LineMask;
                const u64 sq_hi = end & ~decltype(sync_query_lines)::LineMask;
                for (u64 unit = sq_lo;; unit += decltype(sync_query_lines)::LineSize) {
                    sync_query_lines.Record(unit, pos, consumed_cache, refresh);
                    if (unit == sq_hi) {
                        break;
                    }
                }
            } else {
                sync_query_lines.RecordOverflow(pos);
            }
            return;
        }
        if (pos > last_query_note_position) {
            last_query_note_position = pos;
        }
        auto noop_refresh = [] {};
        for (u64 page = page_lo; page <= page_hi; ++page) {
            pending_query_pages.Record(page << 12, pos, last_query_drain_position, noop_refresh);
        }
        for (u64 line = line_lo;; line += 64) {
            pending_query_lines.Record(line, pos, last_query_drain_position, noop_refresh,
                                       ComputeLineWordMask(line, device_address, range_end));
            if (line == line_hi) {
                break;
            }
        }
    }
    /// Returns max unconsumed synchronous-query ring position overlapping `[device_start, size_bytes)`.
    [[nodiscard]] u64 PendingSyncQueryPosition(u64 device_start, u64 size_bytes) const {
        if (!active || device_start == 0 || size_bytes == 0) {
            return 0;
        }
        const u64 cur_consumed = consumed.load(std::memory_order_acquire);
        if (last_sync_query_note_position <= cur_consumed) {
            return 0;
        }
        return QueryRangePos(sync_query_pages, sync_query_lines, device_start, size_bytes,
                             cur_consumed, last_sync_query_note_position);
    }
    [[nodiscard]] bool HasPendingQueryRange(u64 device_start, u64 size_bytes) const {
        if (!active || device_start == 0 || size_bytes == 0 ||
            last_query_note_position <= last_query_drain_position) {
            return false;
        }
        u8 kind = 0;
        const bool pending =
            QueryRangePos(pending_query_pages, pending_query_lines, device_start, size_bytes,
                          last_query_drain_position, last_query_note_position, &kind) >
            last_query_drain_position;
        if (pending) {
            last_q_diag_kind = kind;
        }
        return pending;
    }
    void ClearPendingQueriesUpTo(u64 pos) {
        if (pos > last_query_drain_position) {
            last_query_drain_position = pos;
        }
    }

private:
    enum class Kind : u64 {
        Wrap,   ///< The rest of the ring is unused: continue at its start
        Engine, ///< The engine the next methods go to: word 1
        Method, ///< Bits 8-23 method, 32-63 argument; flags IsLast, HasSegment (word 1)
        Multi,  ///< Bits 8-23 method, 32-63 amount; word 1 methods pending, [segment], words
        Call,   ///< Bits 8-31 payload size in words; word 1 thunk, then the payload
    };
    static constexpr u64 IsLast = 1 << 4;
    static constexpr u64 HasSegment = 1 << 5;
    static constexpr u64 Direct = 1 << 6; ///< Method: call it even if not executable (macros)

    static constexpr size_t RingWords = size_t{1} << 20; // 8 MiB
    static constexpr size_t RingMask = RingWords - 1;
    static constexpr u32 MaxMultiWords = 8192;
    static constexpr size_t PublishEvery = 256;

    template <typename Func>
    static void CallThunk(const u64* payload) {
        (*reinterpret_cast<const Func*>(payload))();
    }

    /// Room for an entry of `words` at the write position (never across the ring's end).
    u64* Reserve(size_t words) {
        const size_t index = write_position & RingMask;
        if (index + words <= RingWords && write_position + words - consumed_cache <= RingWords)
            [[likely]] {
            return &ring[index];
        }
        return ReserveSlow(words);
    }
    u64* ReserveSlow(size_t words);
    void Commit(size_t words) {
        write_position += words;
        if (write_position - published_local >= publish_every) {
            Publish();
        }
    }
    void WaitForSpace(size_t words);
    /// Front: blocks until the back stored `consumed` at or past target (and is quiet if wait_quiet).
    void SleepUntilConsumed(u64 target, bool wait_quiet = false);
    /// Back: hands ring space back and wakes a front sleeping for this position.
    void StoreConsumed(u64 position);
    /// Back: wakes a front waiting for BackQuiet() after consumed already reached target.
    void NotifyFrontQuiet();
    void SelectEngine(Tegra::Engines::EngineInterface* engine) {
        if (engine != front_engine) [[unlikely]] {
            SwitchEngine(engine);
        }
    }
    void SwitchEngine(Tegra::Engines::EngineInterface* engine);

    void BackLoop(std::stop_token stop_token);
    size_t Execute(const u64* entry);

    Core::System* system{};
    bool active{};
    bool front_macros{};
    std::unique_ptr<u64[]> ring;

    [[nodiscard]] static u16 ComputeLineWordMask(u64 line_addr, u64 range_start,
                                                   u64 range_end) noexcept {
        const u64 lo = (std::max)(line_addr, range_start) - line_addr;
        const u64 hi = (std::min)(line_addr + 64, range_end) - line_addr;
        const u32 w0 = static_cast<u32>(lo >> 2);
        const u32 w1 = static_cast<u32>((hi - 1) >> 2);
        const u32 bits = w1 - w0 + 1;
        return static_cast<u16>(bits >= 16 ? 0xffffU : (((1U << bits) - 1U) << w0));
    }

    template <size_t Sets, size_t Ways, size_t Shift, bool WordMasked = false>
    struct TaggedPosTable {
        static constexpr bool UsesWordMask = WordMasked;
        static constexpr size_t LineShift = Shift;
        static constexpr u64 LineSize = u64{1} << Shift;
        static constexpr u64 LineMask = LineSize - 1;
        struct Way {
            u64 key{};
            u64 pos{};
            u16 mask{0xffff};
        };
        static constexpr size_t SpillWays = 16;
        std::array<std::array<Way, Ways>, Sets> sets{};
        std::array<Way, SpillWays> spill{};
        size_t spill_cursor{};
        u64 spill_max_pos{};
        u64 overflow_pos{};

        void RecordOverflow(u64 pos) {
            if (pos > overflow_pos) {
                overflow_pos = pos;
            }
        }

        static constexpr size_t Mask = Sets - 1;
        static constexpr unsigned HashShift = 64 - static_cast<unsigned>(std::bit_width(Mask));

        static size_t Bucket(u64 addr) {
            return static_cast<size_t>(((addr >> Shift) * 0x9e3779b97f4a7c15ULL) >> HashShift) &
                   Mask;
        }

        void Clear() {
            sets.fill({});
            spill.fill({});
            spill_cursor = 0;
            spill_max_pos = 0;
            overflow_pos = 0;
        }

        template <typename RefreshFloor>
        void Record(u64 addr, u64 pos, u64& stale_floor, RefreshFloor&& refresh_floor,
                    u16 word_mask = 0xffff) {
            const u64 key = addr >> Shift;
            auto& set = sets[Bucket(addr)];
            Way* victim = &set[0];
            Way* stale = nullptr;
            for (Way& w : set) {
                if (w.key == key) {
                    if constexpr (WordMasked) {
                        if (w.pos <= stale_floor) {
                            w.pos = pos;
                            w.mask = word_mask;
                        } else {
                            if (pos > w.pos) {
                                w.pos = pos;
                            }
                            w.mask |= word_mask;
                        }
                    } else if (pos > w.pos) {
                        w.pos = pos;
                    }
                    return;
                }
                if (w.pos <= stale_floor && stale == nullptr) {
                    stale = &w;
                }
                if (w.pos < victim->pos) {
                    victim = &w;
                }
            }
            if (stale != nullptr) {
                *stale = {key, pos, word_mask};
                return;
            }
            refresh_floor();
            if (victim->pos <= stale_floor) {
                *victim = {key, pos, word_mask};
                return;
            }
            Way* spill_match = nullptr;
            Way* spill_stale = nullptr;
            for (Way& s : spill) {
                if (s.key == victim->key) {
                    spill_match = &s;
                    break;
                }
                if (s.pos <= stale_floor && spill_stale == nullptr) {
                    spill_stale = &s;
                }
            }
            if (spill_match != nullptr) {
                if constexpr (WordMasked) {
                    if (spill_match->pos <= stale_floor) {
                        spill_match->pos = victim->pos;
                        spill_match->mask = victim->mask;
                    } else {
                        if (victim->pos > spill_match->pos) {
                            spill_match->pos = victim->pos;
                        }
                        spill_match->mask |= victim->mask;
                    }
                } else if (victim->pos > spill_match->pos) {
                    spill_match->pos = victim->pos;
                }
            } else if (spill_stale != nullptr) {
                *spill_stale = *victim;
            } else {
                Way& evicted = spill[spill_cursor];
                spill_cursor = (spill_cursor + 1) & (SpillWays - 1);
                if (evicted.pos > overflow_pos) {
                    overflow_pos = evicted.pos;
                }
                evicted = *victim;
            }
            if (victim->pos > spill_max_pos) {
                spill_max_pos = victim->pos;
            }
            *victim = {key, pos, word_mask};
        }

        [[nodiscard]] u64 Query(u64 addr, u64 floor, u8* diag_kind = nullptr,
                                u16 query_mask = 0xffff) const {
            const u64 key = addr >> Shift;
            u64 res = 0;
            if (overflow_pos > floor) {
                res = overflow_pos;
                if (diag_kind != nullptr) {
                    *diag_kind = 3;
                }
            }
            if (spill_max_pos > floor) {
                for (const Way& s : spill) {
                    if (s.key == key && s.pos > floor &&
                        (!WordMasked || (s.mask & query_mask) != 0) && s.pos > res) {
                        res = s.pos;
                        if (diag_kind != nullptr) {
                            *diag_kind = 2;
                        }
                    }
                }
            }
            for (const Way& w : sets[Bucket(addr)]) {
                if (w.key == key && w.pos > floor &&
                    (!WordMasked || (w.mask & query_mask) != 0) && w.pos > res) {
                    res = w.pos;
                    if (diag_kind != nullptr) {
                        *diag_kind = 1;
                    }
                }
            }
            return res;
        }
    };

    template <typename Pages, typename Lines>
    [[nodiscard]] static u64 QueryRangePos(const Pages& pages, const Lines& lines,
                                           u64 device_start, u64 size_bytes, u64 floor,
                                           u64 overflow_all, u8* diag_kind = nullptr) {
        if (size_bytes > 0x40000) {
            if (overflow_all > floor) {
                if (diag_kind != nullptr) {
                    *diag_kind = 4;
                }
                return overflow_all;
            }
            return 0;
        }
        const u64 end = device_start + size_bytes;
        u64 pos = 0;
        constexpr u64 FastRangeLimit = Lines::LineShift < 6 ? 64ULL : 256ULL;
        if (size_bytes <= FastRangeLimit) {
            const u64 line_lo = device_start & ~Lines::LineMask;
            const u64 line_hi = (end - 1) & ~Lines::LineMask;
            for (u64 line = line_lo;; line += Lines::LineSize) {
                u8 line_kind = 0;
                const u16 qmask = Lines::UsesWordMask
                                      ? ComputeLineWordMask(line, device_start, end)
                                      : u16{0xffff};
                const u64 q = lines.Query(line, floor, &line_kind, qmask);
                if (q > pos) {
                    pos = q;
                    if (diag_kind != nullptr) {
                        *diag_kind = line_kind;
                    }
                }
                if (line == line_hi) {
                    break;
                }
            }
            return pos;
        }
        u64 page_start = device_start;
        while (page_start < end) {
            const u64 page = page_start >> 12;
            const u64 next_page = (page + 1) << 12;
            const u64 seg_end = (std::min)(end, next_page);
            u8 page_kind = 0;
            const u64 pq = pages.Query(page << 12, floor, &page_kind);
            if (pq > pos) {
                if (page_kind == 3 || (Lines::LineShift < 6 && (seg_end - page_start) > 256)) {
                    // Page table overflowed globally (or range too wide for word-granular lines).
                    pos = pq;
                    if (diag_kind != nullptr) {
                        *diag_kind = page_kind;
                    }
                } else {
                    const u64 line_lo = page_start & ~Lines::LineMask;
                    const u64 line_hi = (seg_end - 1) & ~Lines::LineMask;
                    for (u64 line = line_lo;; line += Lines::LineSize) {
                        u8 line_kind = 0;
                        const u16 qmask = Lines::UsesWordMask
                                              ? ComputeLineWordMask(line, device_start, end)
                                              : u16{0xffff};
                        const u64 q = lines.Query(line, floor, &line_kind, qmask);
                        if (q > pos) {
                            pos = q;
                            if (diag_kind != nullptr) {
                                *diag_kind = line_kind;
                            }
                        }
                        if (line == line_hi) {
                            break;
                        }
                    }
                }
            }
            page_start = seg_end;
        }
        return pos;
    }

    struct ReleaseEntry {
        u64 position{};
        u64 aligned_address{};
        u32 payload{};
        bool has_payload{};
        u8 no_pl_reason{};
        ReleaseValueKind value_kind{};
    };

    struct LargeWriteRange {
        u64 start{};
        u64 end{};
        u64 pos{};
    };
    static constexpr size_t LargeWriteCount = 64;
    static constexpr size_t TrackedSyncpoints = 192;

    // Front side.
    u64 write_position{};
    u64 last_writer_position{};
    u64 last_compute_writer_position{};
    u64 last_all_compute_writer_position{};
    u64 last_unknown_writer_position{};
    u64 last_any_writer_position{};
    u64 writer_large_overflow_pos{};
    u64 last_wildcard_release_position{};
    u64 last_query_note_position{};
    u64 last_query_drain_position{};
    u64 last_sync_query_note_position{};
    u64 last_wildcard_syncpoint_position{};
    std::array<u64, TrackedSyncpoints> syncpoint_positions{};
    std::array<std::array<ReleaseEntry, ReleaseWays>, ReleaseSets> release_positions{};
    std::array<LargeWriteRange, LargeWriteCount> writer_large_ranges{};
    TaggedPosTable<2048, 4, 12> writer_pages{};
    TaggedPosTable<4096, 4, 6, true> writer_lines{};
    TaggedPosTable<2048, 4, 12> pending_query_pages{};
    TaggedPosTable<4096, 4, 6, true> pending_query_lines{};
    TaggedPosTable<2048, 4, 12> sync_query_pages{};
    TaggedPosTable<8192, 4, 2> sync_query_lines{};
    u64 release_checks{};
    u64 wildcard_hits{};
    u64 exact_hits{};
    u64 wildcard_overflows{};
    u64 release_table_overflows{};
    u64 rel_payload_ok{};
    std::array<u64, 6> rel_payload_diag{};
    std::array<u64, 16> rel_no_pl_reasons{};
    mutable u8 last_q_diag_kind{};
    mutable u8 last_wr_diag_kind{};
    std::array<u64, 8> wildcard_sources{};
    u64 published_local{};
    u64 consumed_cache{};
    /// Words between publishes: small right after a drain, while the back has nothing to do.
    size_t publish_every{PublishEvery};
    Tegra::Engines::EngineInterface* front_engine{};

    // Back side.
    Tegra::Engines::EngineInterface* back_engine{};

    static GpuSplit instance;
    static inline thread_local bool on_back_thread{};
    static inline thread_local bool on_gpu_thread{};
    void (*idle_hook)(void*){};
    void* idle_argument{};
    alignas(64) std::atomic<bool> hook_requested{};
    std::atomic<bool> back_running{};
    std::atomic<bool> back_quiet{true};

    // 128 bytes: x86 spatial prefetchers pull 64-byte lines in pairs. back_waiting and wake are
    // always touched together by both threads, so they share a block.
    alignas(128) std::atomic<u64> published{};
    alignas(128) std::atomic<u64> consumed{};
    /// A position the front waits for (WaitForWriters): the back stores `consumed` once past it.
    alignas(128) std::atomic<u64> wait_target{~u64{0}};
    alignas(128) std::atomic<bool> back_waiting{};
    std::atomic<u32> wake{};
    struct SyncPayloadEntry {
        u64 pos{};
        u64 dev_addr{};
        u32 size_bytes{};
        std::array<u8, 16> data{};
    };
    static constexpr size_t MaxSyncPayloads = 128;
    static constexpr size_t SyncPayloadMask = MaxSyncPayloads - 1;
    std::array<SyncPayloadEntry, MaxSyncPayloads> sync_payloads{};
    size_t sync_payload_head{};
    size_t sync_payload_count{};

    /// A position the front sleeps for (SleepUntilConsumed): the back wakes it once past it.
    alignas(128) std::atomic<u64> front_target{~u64{0}};
    std::mutex front_mutex;
    std::condition_variable front_cv;
    std::jthread back_thread;
};

} // namespace VideoCommon
