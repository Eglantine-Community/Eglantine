// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <thread>

#ifdef _MSC_VER
#include <intrin.h>
#endif
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#ifdef __GLIBC__
#include <execinfo.h>
#endif
#include <cstdio>
#include <string>

#include "common/logging.h"
#include "common/settings.h"
#include "common/thread.h"
#include "core/core.h"
#include "video_core/engines/engine_interface.h"
#include "video_core/gpu_split.h"
#include "video_core/nx2.h"
#include "video_core/nx2_profile.h"

namespace VideoCommon {

namespace {

void Pause() {
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
    _mm_pause();
#elif defined(__x86_64__) || defined(__i386__)
    __builtin_ia32_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#else
    std::this_thread::yield();
#endif
}

[[maybe_unused]] std::atomic<u32> g_back_thread_id{};
std::atomic<bool> g_back_busy{};

} // Anonymous namespace

void GpuSplit::CheckOwner(const char* where) {
#ifdef _WIN32
    if (!g_back_busy.load(std::memory_order_acquire)) {
        return;
    }
    const DWORD thread = GetCurrentThreadId();
    if (thread == g_back_thread_id.load(std::memory_order_relaxed)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::fprintf(stderr, "GPU split owner check: %s on thread %lu while the back runs\n", where,
                 thread);
    void* frames[40];
    const USHORT count = RtlCaptureStackBackTrace(1, 40, frames, nullptr);
    for (USHORT i = 0; i < count; ++i) {
        HMODULE module{};
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               static_cast<LPCSTR>(frames[i]), &module)) {
            GetModuleFileNameA(module, name, sizeof(name));
        }
        const char* base_name = std::strrchr(name, '\\');
        std::fprintf(stderr, "  frame %s+%#llx\n", base_name ? base_name + 1 : name,
                     static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i]) -
                                                     reinterpret_cast<uintptr_t>(module)));
    }
    std::fflush(stderr);
#else
    // Also while the back is idle: its idle hook submits (Scheduler::FlushDeferred).
    if (on_back_thread || !instance.back_running.load(std::memory_order_acquire)) {
        return;
    }
    static std::atomic<u32> reports{};
    if (reports.fetch_add(1) >= 12) {
        return;
    }
    std::string stack;
#ifdef __GLIBC__
    void* frames[40];
    const int count = backtrace(frames, 40);
    if (char** const symbols = backtrace_symbols(frames, count)) {
        for (int i = 1; i < count; ++i) {
            stack += fmt::format("\n  {}", symbols[i]);
        }
        std::free(symbols);
    }
#endif
    LOG_ERROR(HW_GPU, "GPU split owner check: {} on a thread other than the back; stack:{}", where,
              stack);
#endif
}

GpuSplit GpuSplit::instance;

void GpuSplit::MaybeStart(Core::System& system_) {
    static const char mode = [] {
        const char* value = std::getenv("EDEN_NX2_SPLIT");
        return value != nullptr ? value[0] : '2';
    }();
    const bool wanted = mode == '1' || mode == '2';
    if (!wanted || !Tegra::g_nx2_ampere.load(std::memory_order_relaxed)) {
        return;
    }
    // The front reads command lists straight from guest memory; a read that flushes the caches
    // first (safe DMA, accurate GPU) would touch back-owned state. The settings can change
    // mid-game: hand the engines back to the front (and start again when they allow it).
    const bool safe_reads = Settings::IsDMALevelDefault() ? Settings::IsGPULevelHigh()
                                                          : Settings::IsDMALevelSafe();
    const bool allowed = !safe_reads && !Settings::IsGPULevelHigh() &&
                         Settings::values.renderer_backend.GetValue() == Settings::RendererBackend::Vulkan;
    if (active && !allowed) {
        Drain();
        Stop();
        LOG_INFO(HW_GPU, "GPU thread split: stopped, the accuracy settings need the caches on the front");
        return;
    }
    if (active || !allowed) {
        return;
    }
    system = &system_;
    check_owner = std::getenv("EDEN_NX2_SPLIT_CHECK") != nullptr;
    ring = std::make_unique<u64[]>(RingWords);
    back_thread = std::jthread([this](std::stop_token stop_token) { BackLoop(stop_token); });
    active = true;
#ifdef ARCHITECTURE_x86_64
    // Front macros run compiled (macro_turing_jit.h) only.
    const char* jit = std::getenv("EDEN_NX2_MME_JIT");
    front_macros = mode == '2' && (jit == nullptr || jit[0] != '0');
#endif
    LOG_INFO(HW_GPU, "GPU thread split: engine methods run on a second thread{}",
             front_macros ? ", macros on the first" : "");
}

void GpuSplit::Stop() {
    if (!back_thread.joinable()) {
        return;
    }
    back_thread.request_stop();
    wake.fetch_add(1, std::memory_order_seq_cst);
    wake.notify_one();
    back_thread.join();
    active = false;
    // The split is static and outlives the emulation session. The next session's back thread
    // starts reading at 0: with the old positions kept it skipped through the new, empty ring a
    // wrap at a time and never reached the old write position (the next game's first GPU work
    // never ran, it waited on its syncpoint forever), and the engines it held were freed.
    ring.reset();
    write_position = 0;
    last_writer_position = 0;
    last_compute_writer_position = 0;
    last_all_compute_writer_position = 0;
    last_unknown_writer_position = 0;
    last_any_writer_position = 0;
    writer_large_overflow_pos = 0;
    last_wildcard_release_position = 0;
    last_query_note_position = 0;
    last_query_drain_position = 0;
    last_sync_query_note_position = 0;
    last_wildcard_syncpoint_position = 0;
    syncpoint_positions.fill(0);
    release_positions.fill({});
    writer_large_ranges.fill({});
    writer_pages.Clear();
    writer_lines.Clear();
    pending_query_pages.Clear();
    pending_query_lines.Clear();
    sync_query_pages.Clear();
    sync_query_lines.Clear();
    sync_payload_head = 0;
    sync_payload_count = 0;
    release_checks = 0;
    wildcard_hits = 0;
    exact_hits = 0;
    wildcard_overflows = 0;
    release_table_overflows = 0;
    rel_payload_ok = 0;
    rel_payload_diag.fill(0);
    rel_no_pl_reasons.fill(0);
    last_q_diag_kind = 0;
    last_wr_diag_kind = 0;
    wildcard_sources.fill(0);
    published_local = 0;
    consumed_cache = 0;
    publish_every = PublishEvery;
    front_engine = nullptr;
    back_engine = nullptr;
    front_macros = false;
    published.store(0, std::memory_order_relaxed);
    consumed.store(0, std::memory_order_relaxed);
    back_waiting.store(false, std::memory_order_relaxed);
}

void GpuSplit::RecordPendingReleasePayloadResult(bool ok, u8 reason, u8 no_pl_reason) {
    if (!Nx2Profile::Enabled()) {
        return;
    }
    if (ok) {
        ++rel_payload_ok;
        if (reason == 5) {
            ++rel_payload_diag[5];
        }
    } else if (reason < rel_payload_diag.size()) {
        ++rel_payload_diag[reason];
        if (reason == 1 && no_pl_reason < rel_no_pl_reasons.size()) {
            ++rel_no_pl_reasons[no_pl_reason];
        }
    }
    const u64 total = rel_payload_ok + rel_payload_diag[1] + rel_payload_diag[2] +
                      rel_payload_diag[3] + rel_payload_diag[4];
    if (total != 0 && (total % 25000) == 0) {
        LOG_WARNING(HW_GPU,
                    "GpuSplit rel_payload: total={}, ok={} (post_compute={}), rej=[no_pl={}, "
                    "wr_range={}, unk_wr={}, wildcard={}], overflows=[rel={}, wr_large={}, "
                    "pq_l={}, pq_p={}], no_pl_by=[3d_nosync={}, 3d_op={}, 3d_rpt={}, "
                    "cmp_b19={}, cmp_b26={}, cmp_nosync={}, cmp_6c3={}, cmp_oth={}, "
                    "pull_nosync={}, pull_null={}, subword={}]",
                    total, rel_payload_ok, rel_payload_diag[5], rel_payload_diag[1],
                    rel_payload_diag[2], rel_payload_diag[3], rel_payload_diag[4],
                    release_table_overflows, writer_large_overflow_pos != 0 ? 1 : 0,
                    pending_query_lines.overflow_pos != 0 ? 1 : 0,
                    pending_query_pages.overflow_pos != 0 ? 1 : 0,
                    rel_no_pl_reasons[1], rel_no_pl_reasons[2], rel_no_pl_reasons[3],
                    rel_no_pl_reasons[4], rel_no_pl_reasons[5], rel_no_pl_reasons[6],
                    rel_no_pl_reasons[7], rel_no_pl_reasons[8], rel_no_pl_reasons[9],
                    rel_no_pl_reasons[10], rel_no_pl_reasons[11]);
    }
}

void GpuSplit::RecordPendingReleaseEvent(bool is_wildcard, u8 source) {
    if (!Nx2Profile::Enabled() || !is_wildcard) {
        return;
    }
    if (source == 7) {
        ++wildcard_overflows;
    } else if (source < wildcard_sources.size()) {
        ++wildcard_sources[source];
    }
}

void GpuSplit::RecordPendingReleaseCheck(bool hit_wildcard, bool hit_exact) {
    if (!Nx2Profile::Enabled()) {
        return;
    }
    ++release_checks;
    if (hit_wildcard) {
        ++wildcard_hits;
    } else if (hit_exact) {
        ++exact_hits;
    }
    if (release_checks % 25000 == 0) {
        LOG_INFO(HW_GPU,
                 "GpuSplit pending_release: checks={}, wildcard_hits={}, exact_hits={}, "
                 "overflows={}, q_pending={}, zero_src=[{}, {}, {}, {}, {}, {}, {}]",
                 release_checks, wildcard_hits, exact_hits, wildcard_overflows,
                 last_query_note_position > last_query_drain_position ? 1 : 0, wildcard_sources[0],
                 wildcard_sources[1], wildcard_sources[2], wildcard_sources[3],
                 wildcard_sources[4], wildcard_sources[5], wildcard_sources[6]);
    }
}

u64* GpuSplit::ReserveSlow(size_t words) {
    size_t index = write_position & RingMask;
    if (index + words > RingWords) {
        // Entries never wrap: skip to the start of the ring.
        const size_t rest = RingWords - index;
        WaitForSpace(rest);
        ring[index] = static_cast<u64>(Kind::Wrap);
        write_position += rest;
        index = 0;
    }
    WaitForSpace(words);
    return &ring[index];
}

void GpuSplit::WaitForSpace(size_t words) {
    if (write_position + words - consumed_cache <= RingWords) {
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    for (u32 spins = 0;; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (write_position + words - consumed_cache <= RingWords) {
            return;
        }
        if (spins < 4096) {
            Pause();
        } else {
            std::this_thread::yield();
        }
    }
}

void GpuSplit::Publish() {
    if (published_local == write_position) {
        return;
    }
    published_local = write_position;
    publish_every = std::min<size_t>(publish_every * 2, PublishEvery);
    published.store(write_position, std::memory_order_seq_cst);
    if (back_waiting.load(std::memory_order_seq_cst)) {
        wake.fetch_add(1, std::memory_order_seq_cst);
        wake.notify_one();
    }
}

void GpuSplit::Drain() {
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache == write_position && BackQuiet()) {
        ClearPendingQueriesUpTo(write_position);
        return;
    }
    Publish();
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    // Everything consumed is not enough: the back submits deferred work (Scheduler::DeferFlush)
    // from its idle hook afterwards, and the caller may use the scheduler once this returns.
    bool hook_asked = false;
    for (u32 spins = 0; spins < 32; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache == write_position) {
            if (BackQuiet()) {
                publish_every = 8;
                ClearPendingQueriesUpTo(write_position);
                return;
            }
            if (!hook_asked) {
                RequestIdleHook();
                hook_asked = true;
            }
        }
        Pause();
    }
    SleepUntilConsumed(write_position, true);
    publish_every = 8;
    ClearPendingQueriesUpTo(write_position);
}

void GpuSplit::SleepUntilConsumed(u64 target, bool wait_quiet) {
    // Sleeping instead of yield-spinning keeps cores free for the back thread and guest CPU.
    // seq_cst pairs with StoreConsumed / NotifyFrontQuiet: either the back sees front_target,
    // or this sees its store.
    front_target.store(target, std::memory_order_seq_cst);
    auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds{5};
    bool hook_asked = false;
    std::unique_lock lock{front_mutex};
    while ((consumed_cache = consumed.load(std::memory_order_seq_cst)) < target ||
           (wait_quiet && !BackQuiet())) {
        if (wait_quiet && consumed_cache >= target && !hook_asked) {
            RequestIdleHook();
            hook_asked = true;
            if (BackQuiet()) {
                break;
            }
        }
        if (front_cv.wait_for(lock, std::chrono::milliseconds{1}) != std::cv_status::timeout) {
            continue;
        }
        // Still waiting on an asleep back thread: its wake-up was lost. Wake it again.
        if (back_waiting.load(std::memory_order_seq_cst)) {
            wake.fetch_add(1, std::memory_order_seq_cst);
            wake.notify_all();
        }
        if (std::chrono::steady_clock::now() >= next_report) {
            next_report += std::chrono::seconds{5};
            LOG_WARNING(HW_GPU,
                        "GPU split drain waiting on the back thread: {} of {} words consumed, "
                        "back {} ({})",
                        consumed_cache, target, back_waiting.load() ? "parked" : "running",
                        g_back_busy.load() ? "inside a command" : "between batches");
        }
    }
    front_target.store(~u64{0}, std::memory_order_relaxed);
}

void GpuSplit::StoreConsumed(u64 position) {
    consumed.store(position, std::memory_order_seq_cst);
    if (position >= front_target.load(std::memory_order_seq_cst)) {
        // Taking the lock orders this after the front's last look at `consumed` or before it.
        { std::scoped_lock lock{front_mutex}; }
        front_cv.notify_one();
    }
    if (Tegra::Nx2SemaphoreWaiters::Get().HasWaiters()) {
        Tegra::Nx2SemaphoreWaiters::Get().NotifyWake();
    }
}

void GpuSplit::NotifyFrontQuiet() {
    if (front_target.load(std::memory_order_seq_cst) != ~u64{0}) {
        { std::scoped_lock lock{front_mutex}; }
        front_cv.notify_one();
    }
    if (Tegra::Nx2SemaphoreWaiters::Get().HasWaiters()) {
        Tegra::Nx2SemaphoreWaiters::Get().NotifyWake();
    }
}

bool GpuSplit::WaitForPosition(u64 target) {
    target = std::min(target, write_position);
    consumed_cache = consumed.load(std::memory_order_acquire);
    if (consumed_cache >= target) {
        return false;
    }
    Nx2Profile::Scope scope{Nx2Profile::Zone::SplitDrain};
    wait_target.store(target, std::memory_order_seq_cst);
    Publish();
    for (u32 spins = 0; spins < 32; ++spins) {
        consumed_cache = consumed.load(std::memory_order_acquire);
        if (consumed_cache >= target) {
            wait_target.store(~u64{0}, std::memory_order_relaxed);
            return true;
        }
        Pause();
    }
    SleepUntilConsumed(target);
    wait_target.store(~u64{0}, std::memory_order_relaxed);
    return true;
}

bool GpuSplit::WaitForWriters() {
    return WaitForPosition(last_writer_position);
}

void GpuSplit::SwitchEngine(Tegra::Engines::EngineInterface* engine) {
    front_engine = engine;
    u64* const entry = Reserve(2);
    entry[0] = static_cast<u64>(Kind::Engine);
    entry[1] = reinterpret_cast<u64>(engine);
    Commit(2);
}

void GpuSplit::MultiMethod(Tegra::Engines::EngineInterface* engine, u32 method, const u32* words,
                           u32 amount, u32 methods_pending, const GPUVAddr* segment) {
    SelectEngine(engine);
    // Long runs (constant buffer and inline uploads) go in pieces, as when a method's data
    // continues in the next GPFIFO entry.
    while (amount > 0) {
        const u32 piece = std::min(amount, MaxMultiWords);
        const size_t head = segment != nullptr ? 3 : 2;
        const size_t size = head + (piece + 1) / 2;
        u64* const entry = Reserve(size);
        entry[0] = static_cast<u64>(Kind::Multi) | (segment != nullptr ? HasSegment : 0) |
                   (u64{method & 0xffff} << 8) | (u64{piece} << 32);
        entry[1] = methods_pending;
        if (segment != nullptr) {
            entry[2] = *segment;
        }
        entry[size - 1] = 0;
        std::memcpy(entry + head, words, size_t{piece} * sizeof(u32));
        Commit(size);
        words += piece;
        amount -= piece;
        methods_pending -= piece;
    }
}

void GpuSplit::DeferFunction(std::function<void()>&& func) {
    auto* const heap = new std::function<void()>(std::move(func));
    Defer([heap] {
        (*heap)();
        delete heap;
    });
}

size_t GpuSplit::Execute(const u64* entry) {
    const u64 head = entry[0];
    switch (static_cast<Kind>(head & 0xf)) {
    case Kind::Wrap:
        return 0;
    case Kind::Engine:
        back_engine = reinterpret_cast<Tegra::Engines::EngineInterface*>(entry[1]);
        return 2;
    case Kind::Method: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 argument = static_cast<u32>(head >> 32);
        auto* const engine = back_engine;
        if ((head & Direct) != 0) {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            engine->CallMethod(*system, method, argument, true);
        } else if (!engine->execution_mask[method]) {
            engine->method_sink.emplace_back(method, argument);
        } else {
            Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
            engine->ConsumeSink(*system);
            if ((head & HasSegment) != 0) {
                engine->current_dma_segment = entry[1];
            }
            engine->CallMethod(*system, method, argument, (head & IsLast) != 0);
        }
        return (head & HasSegment) != 0 ? 2 : 1;
    }
    case Kind::Multi: {
        const u32 method = static_cast<u32>((head >> 8) & 0xffff);
        const u32 amount = static_cast<u32>(head >> 32);
        const size_t data = (head & HasSegment) != 0 ? 3 : 2;
        auto* const engine = back_engine;
        Nx2Profile::Scope method_scope{Nx2Profile::Zone::Method};
        engine->ConsumeSink(*system);
        if ((head & HasSegment) != 0) {
            engine->current_dma_segment = entry[2];
        }
        engine->CallMultiMethod(*system, method, reinterpret_cast<const u32*>(entry + data),
                                amount, static_cast<u32>(entry[1]));
        return data + (amount + 1) / 2;
    }
    case Kind::Call: {
        const size_t payload = static_cast<size_t>((head >> 8) & 0xffffff);
        reinterpret_cast<void (*)(const u64*)>(entry[1])(entry + 2);
        return 2 + payload;
    }
    }
    return 1;
}

void GpuSplit::BackLoop(std::stop_token stop_token) {
    Common::SetCurrentThreadName("GPUBack");
    on_back_thread = true;
    back_running.store(true, std::memory_order_release);
    Common::SetCurrentThreadPriority(Common::ThreadPriority::Critical);
    Common::SetCurrentThreadToPerformanceCores();
    system->RegisterHostThread();
#ifdef _WIN32
    g_back_thread_id.store(GetCurrentThreadId(), std::memory_order_relaxed);
#endif
    u64 read_position = 0;
    while (!stop_token.stop_requested()) {
        Nx2Profile::Report("GPU back thread");
        u64 available = published.load(std::memory_order_acquire);
        if (available == read_position) {
            Nx2Profile::Scope scope{Nx2Profile::Zone::SplitWait};
            // The front usually sends more within microseconds: spin briefly before sleeping.
            for (u32 spins = 0; spins < 32 && available == read_position &&
                                !hook_requested.load(std::memory_order_relaxed);
                 ++spins) {
                Pause();
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                hook_requested.store(false, std::memory_order_seq_cst);
                // The front may already see everything consumed: Drain() waits for back_quiet
                // too, so the deferred submit can't overlap whatever the front does next.
                if (!back_quiet.load(std::memory_order_relaxed)) {
                    if (idle_hook != nullptr) {
                        idle_hook(idle_argument);
                    }
                    back_quiet.store(true, std::memory_order_release);
                    NotifyFrontQuiet();
                }
                available = published.load(std::memory_order_acquire);
            }
            if (available == read_position) {
                // Publish bumps `wake` when it sees back_waiting: read it before the last look
                // at `published`, so a publish in between ends the wait at once.
                back_waiting.store(true, std::memory_order_seq_cst);
                const u32 seen = wake.load(std::memory_order_seq_cst);
                available = published.load(std::memory_order_seq_cst);
                // A request made after the hook ran (RequestIdleHook) runs it before sleeping.
                if (available == read_position && !stop_token.stop_requested() &&
                    !hook_requested.load(std::memory_order_seq_cst)) {
                    wake.wait(seen, std::memory_order_seq_cst);
                }
                back_waiting.store(false, std::memory_order_relaxed);
                continue;
            }
        }
        u64 last_store = read_position;
        // Before any entry runs, so a front that sees these entries consumed also sees this.
        back_quiet.store(false, std::memory_order_seq_cst);
        g_back_busy.store(true, std::memory_order_relaxed);
        while (true) {
            while (read_position != available) {
                const size_t index = read_position & RingMask;
                const size_t size = Execute(&ring[index]);
                read_position += size != 0 ? size : RingWords - index;
                if (hook_requested.load(std::memory_order_relaxed) &&
                    idle_hook != nullptr) [[unlikely]] {
                    hook_requested.store(false, std::memory_order_seq_cst);
                    idle_hook(idle_argument);
                }
                // Hand ring space back now and then while working through a long batch, and at once
                // when the front waits for this position (WaitForWriters).
                if ((read_position - last_store >= 4096 ||
                     read_position >= wait_target.load(std::memory_order_relaxed)) &&
                    read_position != available) {
                    StoreConsumed(read_position);
                    last_store = read_position;
                }
            }
            const u64 more = published.load(std::memory_order_acquire);
            if (more == read_position) {
                break;
            }
            available = more;
            if (read_position - last_store >= 4096 ||
                read_position >= wait_target.load(std::memory_order_relaxed)) {
                StoreConsumed(read_position);
                last_store = read_position;
            }
        }
        g_back_busy.store(false, std::memory_order_release);
        StoreConsumed(read_position);
    }
    back_running.store(false, std::memory_order_release);
}

} // namespace VideoCommon
