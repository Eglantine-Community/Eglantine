// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <mutex>

#include <signal.h>
#include <ucontext.h>

#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/scarletnx/scarletnx_api.h"
#include "core/arm/scarletnx/scarletnx_jit_budget.h"
#include "core/arm/scarletnx/scarletnx_process.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/memory.h"

namespace Core {

namespace {

constexpr u64 CodeWindowSize = 128 * 1024;

constexpr u64 CodeWindow(u64 pc) {
    return pc & ~(CodeWindowSize - 1);
}

size_t ReadJitBudget() {
    const auto budget = ParseScarletNxJitBudget(std::getenv("EDEN_SCARLETNX_JIT_BUDGET"));
    if (!budget) {
        LOG_WARNING(Core_ARM, "Invalid EDEN_SCARLETNX_JIT_BUDGET (expected {}..{}); using {}",
                    ScarletNxDefaultJitBudget, ScarletNxMaxJitBudget, ScarletNxDefaultJitBudget);
    }
    return budget.value_or(ScarletNxDefaultJitBudget);
}

thread_local ScarletNxFaultScope* g_fault_scope{};
static struct {
    struct sigaction old_sigsegv;
    struct sigaction old_sigbus;
    std::once_flag installed;
} g_snx_data;

void ChainSignal(int sig, siginfo_t* info, void* raw_context) {
    const struct sigaction& old = sig == SIGSEGV ? g_snx_data.old_sigsegv : g_snx_data.old_sigbus;
    if (old.sa_flags & SA_SIGINFO) {
        old.sa_sigaction(sig, info, raw_context);
    } else if (old.sa_handler == SIG_DFL) {
        // The faulting instruction runs again and takes the default action.
        signal(sig, SIG_DFL);
    } else if (old.sa_handler != SIG_IGN) {
        old.sa_handler(sig);
    }
}

void FaultHandler(int sig, siginfo_t* info, void* raw_context) {
    ScarletNxFaultScope* const scope = g_fault_scope;
    const auto address = reinterpret_cast<uintptr_t>(info->si_addr);
    const auto guest_address = scope ? address - reinterpret_cast<uintptr_t>(scope->arena) : 0;
    if (!scope || info->si_code <= 0 || guest_address >= scope->arena_size) {
        ChainSignal(sig, info, raw_context);
        return;
    }

    ScarletNxFault fault{.address = guest_address, .signal = sig, .signal_code = info->si_code};
    if (raw_context) {
        const auto* context = static_cast<ucontext_t*>(raw_context);
        fault.host_pc = static_cast<u64>(context->uc_mcontext.gregs[REG_RIP]);
        fault.x86_error = static_cast<u64>(context->uc_mcontext.gregs[REG_ERR]);
        fault.x86_trap = static_cast<int>(context->uc_mcontext.gregs[REG_TRAPNO]);
    }
    const u64 page = guest_address >> Memory::YUZU_PAGEBITS;
    if (page < scope->page_table->entries.size()) {
        static_assert(std::atomic<u64>::is_always_lock_free);
        const auto raw = scope->page_table->entries[page].Raw();
        using Entry = Common::PageTable::PageEntryData;
        fault.page_type = static_cast<Common::PageType>(raw.type);
        fault.block = Entry::ExtractBlock(raw);
        fault.marked = raw.marked != 0;
        if (const auto ptr = Entry::ExtractPointer(raw, true)) {
            fault.backing = ptr + guest_address;
        }
    }

    const auto action = fault.SelectAction(scope->last_attempt);
    if (action == ScarletNxFaultAction::ReadCache ||
        action == ScarletNxFaultAction::InvalidateCache) {
        scope->last_attempt = fault;
        u8 ignored{};
        scope->last_attempt_returned =
            action == ScarletNxFaultAction::InvalidateCache
                ? scope->memory->InvalidateNCE(guest_address & ~Memory::YUZU_PAGEMASK,
                                               Memory::YUZU_PAGESIZE)
                : scope->memory->ReadBlock(guest_address, &ignored, sizeof(ignored));
        if (scope->last_attempt_returned) {
            return;
        }
    }

    scope->fault = fault;
    scope->action = action;
    siglongjmp(scope->env, 1);
}

} // namespace

class ScarletNxProfile {
    using Clock = std::chrono::steady_clock;
    using Nanoseconds = std::chrono::nanoseconds;

    struct Counter {
        u64 calls{};
        u64 successes{};
        u64 ns{};
        u64 bytes{};
    };

public:
    ScarletNxProfile(size_t jit_budget, bool inprocess_loader, bool reuse_llvm_target)
        : m_jit_budget{jit_budget}, m_inprocess_loader{inprocess_loader},
          m_reuse_llvm_target{reuse_llvm_target} {}

    enum Stage : size_t {
        Query, CompileLockWait, Recheck, Snapshot, Library, Publish, Discard, StageCount,
    };
    enum Outcome : size_t {
        Published, AlreadyKnown, InvalidatedBefore, InvalidatedAfter, NotExecutable,
        SnapshotFailed, CompileFailed, MissingEntry, OutcomeCount,
    };

    struct ResolveData {
        Clock::time_point start = Clock::now();
        Clock::time_point stage_start = start;
        std::array<Counter, StageCount> stages{};
        std::array<Counter, SNX_PROFILE_STAGE_COUNT> library{};
    };

    class ResolveSample {
    public:
        explicit ResolveSample(ScarletNxProfile* profile) : m_profile{profile} {
            if (m_profile) {
                m_data.emplace();
            }
        }

        ~ResolveSample() {
            if (m_data) {
                m_profile->Complete(*m_data, outcome, Elapsed(Clock::now() - m_data->start));
            }
        }

        void Start() {
            if (m_data) {
                m_data->stage_start = Clock::now();
            }
        }

        void Record(Stage stage, bool success = true, u64 bytes = 0) {
            if (m_data) {
                m_data->stages[stage] = {1, success, Elapsed(Clock::now() - m_data->stage_start),
                                         bytes};
            }
        }

        void AttachLibrary(snx_recompiler* recompiler) {
            if (m_data) {
                snx_recompiler_set_profile(recompiler, &LibrarySample, &*m_data);
            }
        }

        void DetachLibrary(snx_recompiler* recompiler) {
            if (m_data) {
                snx_recompiler_set_profile(recompiler, nullptr, nullptr);
            }
        }

        Outcome outcome{MissingEntry};

    private:
        static void LibrarySample(void* context, snx_profile_stage stage, uint64_t ns,
                                  uint64_t bytes, int success) {
            auto& data = *static_cast<ResolveData*>(context);
            Add(data.library[stage], {1, success != 0, ns, bytes});
        }

        ScarletNxProfile* m_profile;
        std::optional<ResolveData> m_data;
    };

    void CountDispatch(const ScarletNxProcess::Function& function) {
        auto& counter = function.entry  ? m_dispatch_hit
                        : function.failed ? m_dispatch_failed
                                          : m_dispatch_resolve;
        counter.fetch_add(1, std::memory_order_relaxed);
    }

    void Report(std::string_view reason) {
        std::array<Counter, StageCount> resolve;
        std::array<Counter, SNX_PROFILE_STAGE_COUNT> library;
        std::array<u64, OutcomeCount> outcomes;
        u64 count;
        u64 total_ns;
        {
            std::scoped_lock lk{m_lock};
            resolve = m_resolve;
            library = m_library;
            outcomes = m_outcomes;
            count = m_count;
            total_ns = m_total_ns;
        }

        constexpr std::array resolve_labels{
            "exec_query", "compile_lock_wait", "recheck", "snapshot", "library", "publish", "discard",
        };
        constexpr std::array library_labels{
            "discovery", "cache_lookup", "cache_read", "cache_image_load", "llvm_init",
            "llvm_units", "llvm_metadata", "rtlib", "rtlib_cache_read", "rtlib_cache_write",
            "link", "image_load", "image_write", "dlopen", "cache_write",
            "inprocess_load", "inprocess_map", "inprocess_descriptor",
            "llvm_new_target_ir", "llvm_reused_target_ir",
        };
        constexpr std::array outcome_labels{
            "published", "already_known", "invalidated_before", "invalidated_after",
            "not_executable", "snapshot_failed", "compile_failed", "missing_entry",
        };
        const auto hit = m_dispatch_hit.load(std::memory_order_relaxed);
        const auto failed = m_dispatch_failed.load(std::memory_order_relaxed);
        const auto requested = m_dispatch_resolve.load(std::memory_order_relaxed);
        std::string outcome_text;
        for (size_t i = 0; i < outcomes.size(); ++i) {
            fmt::format_to(std::back_inserter(outcome_text), " {}={}", outcome_labels[i],
                           outcomes[i]);
        }
        LOG_INFO(Core_ARM,
                 "ScarletNX profile {}: elapsed_ms={:.3f} resolves={} resolve_total_ms={:.3f} "
                 "dispatch_misses={} dispatch_hit={} dispatch_failed={} dispatch_resolve={} "
                 "jit_budget={} jit_loader={} llvm_target_reuse={}{}",
                 reason, Milliseconds(Elapsed(Clock::now() - m_start)), count, Milliseconds(total_ns),
                 hit + failed + requested, hit, failed, requested, m_jit_budget,
                 m_inprocess_loader ? "inprocess" : "dlopen", m_reuse_llvm_target, outcome_text);
        LOG_INFO(Core_ARM, "ScarletNX profile {} resolve stages (calls/ok/ms/bytes), resolves={}:{}",
                 reason, count, Format(resolve_labels, resolve));
        LOG_INFO(Core_ARM, "ScarletNX profile {} library stages (calls/ok/ms/bytes), resolves={}:{}",
                 reason, count, Format(library_labels, library));

        const auto time = [&](snx_profile_stage stage) { return library[stage].ns; };
        u64 resolve_stages_ns = 0;
        for (const auto& stage : resolve) {
            resolve_stages_ns += stage.ns;
        }
        const auto library_stages_ns =
            time(SNX_PROFILE_DISCOVERY) + time(SNX_PROFILE_CACHE_LOOKUP) +
            time(SNX_PROFILE_LLVM_INIT) + time(SNX_PROFILE_LLVM_UNITS) +
            time(SNX_PROFILE_LLVM_METADATA) + time(SNX_PROFILE_LINK) +
            time(SNX_PROFILE_IMAGE_LOAD) + time(SNX_PROFILE_CACHE_WRITE);
        LOG_INFO(Core_ARM,
                 "ScarletNX profile {} exclusive ms, resolves={}: resolve_other={:.3f} "
                 "library_other={:.3f} cache_lookup={:.3f} image_load_both={:.3f} "
                 "llvm_metadata={:.3f} rtlib={:.3f} inprocess_load={:.3f}",
                 reason, count, DifferenceMs(total_ns, resolve_stages_ns),
                 DifferenceMs(resolve[Library].ns, library_stages_ns),
                 DifferenceMs(time(SNX_PROFILE_CACHE_LOOKUP),
                              time(SNX_PROFILE_CACHE_READ) + time(SNX_PROFILE_CACHE_IMAGE_LOAD)),
                 DifferenceMs(time(SNX_PROFILE_IMAGE_LOAD) + time(SNX_PROFILE_CACHE_IMAGE_LOAD),
                              time(SNX_PROFILE_IMAGE_WRITE) + time(SNX_PROFILE_DLOPEN) +
                                  time(SNX_PROFILE_INPROCESS_LOAD)),
                 DifferenceMs(time(SNX_PROFILE_LLVM_METADATA), time(SNX_PROFILE_RTLIB)),
                 DifferenceMs(time(SNX_PROFILE_RTLIB), time(SNX_PROFILE_RTLIB_CACHE_READ) +
                                                           time(SNX_PROFILE_RTLIB_CACHE_WRITE)),
                 DifferenceMs(time(SNX_PROFILE_INPROCESS_LOAD),
                              time(SNX_PROFILE_INPROCESS_MAP) + time(SNX_PROFILE_INPROCESS_DESCRIPTOR)));
    }

private:
    static u64 Elapsed(Clock::duration duration) {
        return static_cast<u64>(std::chrono::duration_cast<Nanoseconds>(duration).count());
    }

    static double Milliseconds(u64 ns) {
        return static_cast<double>(ns) / 1'000'000;
    }

    static double DifferenceMs(u64 inclusive, u64 children) {
        return Milliseconds(inclusive - std::min(inclusive, children));
    }

    static void Add(Counter& total, const Counter& sample) {
        total.calls += sample.calls;
        total.successes += sample.successes;
        total.ns += sample.ns;
        total.bytes += sample.bytes;
    }

    template <size_t Size>
    static std::string Format(const std::array<const char*, Size>& names,
                              const std::array<Counter, Size>& counters) {
        std::string text;
        for (size_t i = 0; i < Size; ++i) {
            const auto& c = counters[i];
            fmt::format_to(std::back_inserter(text), " {}={}/{}/{:.3f}/{}", names[i], c.calls,
                           c.successes, Milliseconds(c.ns), c.bytes);
        }
        return text;
    }

    void Complete(const ResolveData& sample, Outcome outcome, u64 ns) {
        bool milestone;
        {
            std::scoped_lock lk{m_lock};
            for (size_t i = 0; i < m_resolve.size(); ++i) {
                Add(m_resolve[i], sample.stages[i]);
            }
            for (size_t i = 0; i < m_library.size(); ++i) {
                Add(m_library[i], sample.library[i]);
            }
            ++m_outcomes[outcome];
            m_total_ns += ns;
            milestone = ++m_count % 4096 == 0;
        }
        if (milestone) {
            Report("milestone");
        }
    }

    const size_t m_jit_budget;
    const bool m_inprocess_loader;
    const bool m_reuse_llvm_target;
    Clock::time_point m_start = Clock::now();
    std::mutex m_lock;
    std::array<Counter, StageCount> m_resolve{};
    std::array<Counter, SNX_PROFILE_STAGE_COUNT> m_library{};
    std::array<u64, OutcomeCount> m_outcomes{};
    u64 m_count{};
    u64 m_total_ns{};
    std::atomic<u64> m_dispatch_hit{};
    std::atomic<u64> m_dispatch_failed{};
    std::atomic<u64> m_dispatch_resolve{};
};

void InstallScarletNxFaultHandler() {
    std::call_once(g_snx_data.installed, [] {
        struct sigaction action {};
        action.sa_flags = SA_SIGINFO;
        action.sa_sigaction = &FaultHandler;
        sigemptyset(&action.sa_mask);
        sigaction(SIGSEGV, &action, &g_snx_data.old_sigsegv);
        sigaction(SIGBUS, &action, &g_snx_data.old_sigbus);
    });
}

void SetScarletNxFaultScope(ScarletNxFaultScope* scope) {
    g_fault_scope = scope;
}

u64 ScarletNxTicks() {
    return g_fault_scope->timing->GetClockTicks();
}

ScarletNxProcess::ScarletNxProcess(System& system, Kernel::KProcess& process)
    : m_system{system}
    , m_process{process}
    , m_arena{system.DeviceMemory().buffer.VirtualBasePointer()}
    , m_compile_instruction_budget{ReadJitBudget()}
{
    InstallScarletNxFaultHandler();

    m_recompiler = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    ASSERT(m_recompiler && "cannot create the recompiler");
    snx_recompiler_set_optimization(m_recompiler, '0');
    snx_recompiler_set_jobs(m_recompiler, 2);
    snx_recompiler_set_hook_direct_calls(m_recompiler, 1);
    if (process.IsSwitch2()) {
        snx_recompiler_set_a64_target(m_recompiler, SNX_RECOMPILER_A64_MAX);
    }
    const char* loader = std::getenv("EDEN_SCARLETNX_INPROCESS_LOADER");
    const bool inprocess_loader = loader && std::string_view{loader} == "1";
    if (inprocess_loader) {
        snx_recompiler_set_inprocess_delta_loader(m_recompiler, 1);
    }
    const char* reuse = std::getenv("EDEN_SCARLETNX_REUSE_LLVM_TARGET");
    const bool reuse_llvm_target = reuse && std::string_view{reuse} == "1";
    if (reuse_llvm_target) {
        snx_recompiler_set_reuse_delta_target(m_recompiler, 1);
    }
    if (const char* value = std::getenv("EDEN_SCARLETNX_PROFILE");
        value && std::string_view{value} == "1") {
        m_profile = std::make_unique<ScarletNxProfile>(m_compile_instruction_budget, inprocess_loader,
                                                       reuse_llvm_target);
        LOG_INFO(Core_ARM, "ScarletNX profile enabled: jit_budget={} jit_loader={} llvm_target_reuse={}",
                 m_compile_instruction_budget, inprocess_loader ? "inprocess" : "dlopen",
                 reuse_llvm_target);
    }

    // Linked modules are kept by a hash of their code, so later launches skip the recompile.
    const auto cache = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "scarletnx-fastmem";
    if (snx_recompiler_set_cache(m_recompiler, Common::FS::PathToUTF8String(cache).c_str()) != 0)
        LOG_WARNING(Core_ARM, "no code cache: {}", snx_recompiler_error(m_recompiler));
}

ScarletNxProcess::~ScarletNxProcess() {
    if (m_profile) {
        m_profile->Report("shutdown");
    }
    for (const auto& [_, modules] : m_windows) {
        for (const auto& module : modules) {
            snx_recompiled_free(module.mod);
        }
    }
    for (auto* module : m_retired_modules) {
        snx_recompiled_free(module);
    }
    snx_recompiler_free(m_recompiler);
}

bool ScarletNxProcess::ShouldUse(System& system, const Kernel::KProcess& process) {
    if (Settings::values.cpu_backend.GetValue() != Settings::CpuBackend::ScarletNx)
        return false;
    const auto fallback = [](std::string_view reason) {
        LOG_WARNING(Core_ARM, "ScarletNX: {}; using Dynarmic", reason);
        return false;
    };
    if (!process.IsApplication())
        return false;
    if (!process.Is64Bit())
        return fallback("requires an AArch64 application");
    if (!system.Kernel().IsMulticore())
        return fallback("requires multicore");
    if (system.DebuggerEnabled() || Settings::values.cpu_accuracy.GetValue() == Settings::CpuAccuracy::Debugging)
        return fallback("does not support the debugger");
    const auto& buffer = system.DeviceMemory().buffer;
    const bool fastmem = Settings::IsFastmemEnabled() && buffer.VirtualBasePointer() &&
                         buffer.VirtualSize() == ArenaSize;
    if (const auto reason =
            MemoryModeFallback(fastmem, Settings::values.use_reactive_flushing.GetValue());
        !reason.empty()) {
        return fallback(reason);
    }
    if (snx_recompiler_abi() != SNX_ABI || !snx_recompiler_has(SNX_RECOMPILER_FLAG_LLVM))
        return fallback("incompatible recompiler library");
    return true;
}

std::optional<ScarletNxProcess::CodeRange> ScarletNxProcess::GetCodeRange(u64 pc) const {
    const auto& page_table = m_process.GetPageTable().GetBasePageTable();
    Kernel::KMemoryInfo info;
    Kernel::Svc::PageInfo page_info;
    if (pc % sizeof(u32) != 0 || page_table.QueryInfo(&info, &page_info, pc).IsError() ||
        !True(info.GetPermission() & Kernel::KMemoryPermission::UserExecute)) {
        return {};
    }
    const u64 window = CodeWindow(pc);
    const u64 start = std::max(window, info.GetAddress());
    const u64 end = std::min(window + CodeWindowSize, info.GetEndAddress());
    return CodeRange{start, end - start};
}

ScarletNxProcess::Function ScarletNxProcess::FindFunctionLocked(u64 pc) const {
    if (const auto window = m_windows.find(CodeWindow(pc)); window != m_windows.end()) {
        for (const auto& module : window->second) {
            if (pc >= module.lo && pc < module.hi) {
                if (const auto entry = snx_recompiled_function(module.mod, pc)) {
                    return {module.mod, entry, false};
                }
            }
        }
    }
    return {.failed = m_failed_entries.contains(pc)};
}

ScarletNxProcess::Function ScarletNxProcess::FindFunction(u64 pc) {
    std::scoped_lock lk{m_mutex};
    return FindFunctionLocked(pc);
}

void ScarletNxProcess::CountDispatchMiss(const Function& function) {
    m_profile->CountDispatch(function);
}

void ScarletNxProcess::Resolve(u64 pc) {
    using Profile = ScarletNxProfile;
    Profile::ResolveSample sample{m_profile.get()};
    // QueryInfo can reschedule the guest thread; do not hold a host lock across it.
    const auto generation = CodeGeneration();
    const auto range = GetCodeRange(pc);
    sample.Record(Profile::Query, range.has_value());
    sample.Start();
    std::scoped_lock compile_lk{m_compile_mutex};
    sample.Record(Profile::CompileLockWait);
    sample.Start();
    if (generation != CodeGeneration()) {
        sample.Record(Profile::Recheck, false);
        sample.outcome = Profile::InvalidatedBefore;
        return;
    }
    const auto known = FindFunction(pc);
    sample.Record(Profile::Recheck);
    if (known.entry || known.failed) {
        sample.outcome = Profile::AlreadyKnown;
        return;
    }

    snx_recompiled* module{};
    if (range) {
        sample.Start();
        std::vector<u8> snapshot(range->size);
        const bool read =
            m_process.GetMemory().ReadBlockUnsafe(range->addr, snapshot.data(), snapshot.size());
        sample.Record(Profile::Snapshot, read, snapshot.size());
        if (read) {
            const snx_code_range code{range->addr, snapshot.data(), snapshot.size()};
            LOG_DEBUG(Core_ARM, "ScarletNX compiling from {:#x}", pc);
            // Compile from the actual entry rather than starting a separate JIT pool per window.
            sample.AttachLibrary(m_recompiler);
            sample.Start();
            module = snx_recompile_from(m_recompiler, &code, 1, &pc, 1, m_compile_instruction_budget,
                                        nullptr, nullptr);
            sample.Record(Profile::Library, module != nullptr);
            sample.DetachLibrary(m_recompiler);
            if (!module) {
                sample.outcome = Profile::CompileFailed;
                LOG_ERROR(Core_ARM, "ScarletNX compilation failed at {:#x}: {}", pc,
                          snx_recompiler_error(m_recompiler));
            }
        } else {
            sample.outcome = Profile::SnapshotFailed;
        }
    } else {
        sample.outcome = Profile::NotExecutable;
    }

    sample.Start();
    {
        std::scoped_lock lk{m_mutex};
        // Do not publish code that could have been changed or unmapped while it was compiled.
        if (generation == CodeGeneration()) {
            if (module && snx_recompiled_function(module, pc)) {
                auto& mod = m_windows[CodeWindow(pc)].emplace_back(Module{*range, 0, 0, module});
                snx_recompiled_range(module, &mod.lo, &mod.hi);
                sample.Record(Profile::Publish);
                sample.outcome = Profile::Published;
                return;
            }
            m_failed_entries.insert(pc);
        } else {
            sample.outcome = Profile::InvalidatedAfter;
        }
    }
    sample.Record(Profile::Publish, false);
    if (module) {
        sample.Start();
        snx_recompiled_free(module);
        sample.Record(Profile::Discard);
    }
}

void ScarletNxProcess::InvalidateCacheRange(u64 addr, u64 size) {
    std::scoped_lock lk{m_mutex};
    for (auto& [_, modules] : m_windows) {
        std::erase_if(modules, [&](const Module& mod) {
            if (addr >= mod.range.addr + mod.range.size || mod.range.addr >= addr + size) {
                return false;
            }
            snx_recompiled_invalidate(mod.mod, mod.lo, mod.hi - mod.lo, nullptr, nullptr);
            m_retired_modules.push_back(mod.mod);
            return true;
        });
    }
    std::erase_if(m_failed_entries, [&](u64 pc) { return pc >= addr && pc - addr < size; });
    m_code_generation.fetch_add(1, std::memory_order_release);
}

void ScarletNxProcess::ClearInstructionCache() {
    std::scoped_lock lk{m_mutex};
    for (const auto& [_, modules] : m_windows) {
        for (const auto& mod : modules) {
            snx_recompiled_invalidate(mod.mod, mod.lo, mod.hi - mod.lo, nullptr, nullptr);
            m_retired_modules.push_back(mod.mod);
        }
    }
    m_windows.clear();
    m_failed_entries.clear();
    m_code_generation.fetch_add(1, std::memory_order_release);
}

} // namespace Core
