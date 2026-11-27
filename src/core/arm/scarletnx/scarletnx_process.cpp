// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <cstdlib>
#include <mutex>
#include <span>

#include <signal.h>
#include <sys/mman.h>
#include <ucontext.h>

#include "common/cpu_features.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/scarletnx/scarletnx_api.h"
#include "core/arm/scarletnx/scarletnx_process.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/kernel.h"
#include "core/hle/nvn/bridge.h"
#include "core/memory.h"

namespace Core {

namespace {

constexpr u64 MaxImageSize = 128ULL << 20;
constexpr u64 CompileWindow = 256ULL << 10;
constexpr size_t InstructionsPerBatch = 8192;
constexpr size_t DispatchBytes = (ScarletNxProcess::ArenaSize / 4) * sizeof(GuestFn);
thread_local ScarletNxFaultScope* g_fault_scope{};
static struct {
    struct sigaction old_sigsegv;
    struct sigaction old_sigbus;
    struct sigaction old_sigtrap;
    std::once_flag install_once;
    bool installed{};
} g_snx_data;

void ChainSignal(int sig, siginfo_t* info, void* raw_context) {
    const struct sigaction& old = sig == SIGSEGV ? g_snx_data.old_sigsegv
                                  : sig == SIGBUS ? g_snx_data.old_sigbus
                                                  : g_snx_data.old_sigtrap;
    if (old.sa_flags & SA_SIGINFO) {
        old.sa_sigaction(sig, info, raw_context);
    } else if (old.sa_handler == SIG_DFL) {
        // The faulting instruction runs again and takes the default action.
        signal(sig, SIG_DFL);
    } else if (old.sa_handler != SIG_IGN) {
        old.sa_handler(sig);
    }
}

constexpr u64 X86TraceFlag = 1 << 8;

void TraceHandler(int sig, siginfo_t* info, void* raw_context) {
    ScarletNxFaultScope* const scope = g_fault_scope;
    if (!scope || info->si_code != TRAP_TRACE || scope->pending_write_count == 0) {
        ChainSignal(sig, info, raw_context);
        return;
    }
    auto& context = *static_cast<ucontext_t*>(raw_context);
    scope->CompleteWrites();
    if (scope->previous_trace_flag) {
        ChainSignal(sig, info, raw_context);
    } else {
        context.uc_mcontext.gregs[REG_EFL] &= ~X86TraceFlag;
    }
}

void FaultHandler(int sig, siginfo_t* info, void* raw_context) {
    ScarletNxFaultScope* const scope = g_fault_scope;
    const uintptr_t address = reinterpret_cast<uintptr_t>(info->si_addr);
    const uintptr_t arena = scope ? reinterpret_cast<uintptr_t>(scope->arena) : 0;
    if (!scope || address < arena || address - arena >= scope->arena_size) {
        ChainSignal(sig, info, raw_context);
        return;
    }
    const u64 guest_address = address - arena;
    auto& context = *static_cast<ucontext_t*>(raw_context);
    const bool write = (context.uc_mcontext.gregs[REG_ERR] & 2) != 0;
    const auto can_retry = [&] {
        const u64 host_pc = context.uc_mcontext.gregs[REG_RIP];
        if (scope->retry_address != guest_address || scope->retry_host_pc != host_pc) {
            scope->retry_address = guest_address;
            scope->retry_host_pc = host_pc;
            scope->retries = 0;
        }
        return ++scope->retries <= 2;
    };
    if (sig == SIGSEGV) {
        const auto fault = scope->memory->ResolveScarletNxFastmemFault(guest_address, write);
        if (fault == Memory::Memory::ScarletNxFault::Resolved) {
            scope->retries = 0;
            return;
        }
        if (fault == Memory::Memory::ScarletNxFault::CompleteWrite) {
            const u64 page = guest_address & ~Memory::YUZU_PAGEMASK;
            const auto writes = std::span{scope->pending_writes}.first(scope->pending_write_count);
            if (std::ranges::find(writes, page) != writes.end()) {
                // A different core may have protected the page before the retried store.
                if (can_retry()) {
                    return;
                }
            } else if (scope->pending_write_count < scope->pending_writes.size()) {
                if (scope->pending_write_count == 0) {
                    scope->previous_trace_flag =
                        (context.uc_mcontext.gregs[REG_EFL] & X86TraceFlag) != 0;
                }
                scope->retries = 0;
                scope->pending_writes[scope->pending_write_count++] = page;
                context.uc_mcontext.gregs[REG_EFL] |= X86TraceFlag;
                return;
            }
        }
        if (fault == Memory::Memory::ScarletNxFault::Retry && can_retry()) {
            return;
        }
    }
    scope->fault_address = guest_address;
    siglongjmp(scope->env, 1);
}

} // namespace

void ScarletNxFaultScope::CompleteWrites() {
    for (u32 i = 0; i < pending_write_count; ++i) {
        memory->CompleteScarletNxFastmemWrite(pending_writes[i]);
    }
    pending_write_count = 0;
    retries = 0;
}

bool InstallScarletNxFaultHandler() {
    std::call_once(g_snx_data.install_once, [] {
        struct sigaction action{};
        action.sa_flags = SA_SIGINFO;
        action.sa_sigaction = &FaultHandler;
        sigemptyset(&action.sa_mask);
        if (sigaction(SIGSEGV, &action, &g_snx_data.old_sigsegv) != 0) {
            return;
        }
        if (sigaction(SIGBUS, &action, &g_snx_data.old_sigbus) != 0) {
            sigaction(SIGSEGV, &g_snx_data.old_sigsegv, nullptr);
            return;
        }
        action.sa_sigaction = &TraceHandler;
        if (sigaction(SIGTRAP, &action, &g_snx_data.old_sigtrap) != 0) {
            sigaction(SIGSEGV, &g_snx_data.old_sigsegv, nullptr);
            sigaction(SIGBUS, &g_snx_data.old_sigbus, nullptr);
            return;
        }
        g_snx_data.installed = true;
    });
    return g_snx_data.installed;
}

void SetScarletNxFaultScope(ScarletNxFaultScope* scope) { g_fault_scope = scope; }

u64 ScarletNxTicks() { return g_fault_scope->timing->GetClockTicks(); }

ScarletNxProcess::ScarletNxProcess(System& system, Kernel::KProcess& process)
    : m_system{system}, m_process{process},
      m_arena{system.DeviceMemory().buffer.VirtualBasePointer()} {
    m_recompiler = snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM);
    if (!m_recompiler) {
        return;
    }
    // The Cortex-A57 profile also controls system register semantics, including FPCR.FZ16.
    const int target = m_process.IsSwitch2() ? SNX_RECOMPILER_A64_MAX : SNX_RECOMPILER_A64_SWITCH;
    if (snx_recompiler_set_a64_target(m_recompiler, target) != 0) {
        snx_recompiler_free(m_recompiler);
        m_recompiler = nullptr;
        return;
    }
    snx_recompiler_set_hook_direct_calls(m_recompiler, 1);
    snx_recompiler_set_jobs(m_recompiler, 2);

    // Virtual only. Linux commits table pages when an executable entry is published.
    void* const table = mmap(nullptr, DispatchBytes, PROT_READ | PROT_WRITE,
                             MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (table == MAP_FAILED) {
        return;
    }
    m_dispatch = static_cast<SnxGuestFn*>(table);

    // Linked modules are kept by a hash of their code, so later launches skip the recompile.
    const auto cache = Common::FS::GetEdenPath(Common::FS::EdenPath::CacheDir) / "scarletnx";
    if (snx_recompiler_set_cache(m_recompiler, cache.c_str()) != 0)
        LOG_WARNING(Core_ARM, "no code cache: {}", snx_recompiler_error(m_recompiler));
}

ScarletNxProcess::~ScarletNxProcess() {
    LOG_INFO(Core_ARM, "ScarletNX: compiled {} progressive batches", m_modules.size());
    for (const auto& module : m_modules) {
        snx_recompiled_free(module.code);
    }
    if (m_dispatch) {
        munmap(m_dispatch, DispatchBytes);
    }
    if (m_recompiler) {
        snx_recompiler_free(m_recompiler);
    }
}

void ScarletNxProcess::Bind(CPU& cpu, std::size_t core) {
    cpu.disp = m_dispatch;
    cpu.code_lo = 0;
    cpu.code_hi = ArenaSize;
    m_cpus[core] = &cpu;
}

bool ScarletNxProcess::ShouldUse(System& system, const Kernel::KProcess& process) {
    static const bool requested = [] {
        const char* value = std::getenv("EDEN_NX2_SCARLETNX");
        return value && value[0] == '1';
    }();
    if (Settings::IsNceEnabled() || !process.IsApplication() || !process.Is64Bit() ||
        (!requested &&
         Settings::values.cpu_backend.GetValue() != Settings::CpuBackend::ScarletNx)) {
        return false;
    }
    const auto fall_back = [](const char* reason) {
        LOG_WARNING(Core_ARM, "ScarletNX: {}; using Dynarmic", reason);
        return false;
    };
    if (!system.Kernel().IsMulticore()) {
        return fall_back("single-core mode needs cycle counting");
    }
    if (system.DebuggerEnabled()) {
        return fall_back("the debugger needs stepping and watchpoints");
    }
    const auto accuracy = Settings::values.cpu_accuracy.GetValue();
    if (accuracy != Settings::CpuAccuracy::Auto &&
        !(accuracy == Settings::CpuAccuracy::Unsafe &&
          Settings::values.cpuopt_unsafe_ignore_global_monitor.GetValue())) {
        return fall_back("the selected accuracy needs a shared exclusive monitor");
    }
    if (Settings::values.use_reactive_flushing.GetValue()) {
        return fall_back("reactive flushing requires a CPU access callback");
    }
    if (!Settings::IsFastmemEnabled() || !system.DeviceMemory().buffer.VirtualBasePointer()) {
        return fall_back("guest memory must be mapped by fastmem");
    }
    if (snx_recompiler_abi() != SNX_ABI) {
        return fall_back("the library was built with a different ABI");
    }
    if (!snx_recompiler_has(SNX_RECOMPILER_FLAG_LLVM)) {
        return fall_back("LLVM cannot target this host");
    }
    if (!InstallScarletNxFaultHandler()) {
        return fall_back("cannot install the fastmem fault handler");
    }
    return true;
}

void ScarletNxProcess::UseFallback(u64 pc, const char* reason) {
    if (!m_fallback.exchange(true, std::memory_order_relaxed)) {
        LOG_WARNING(Core_ARM, "ScarletNX: {} at {:#x}; switching to Dynarmic", reason, pc);
        for (CPU* cpu : m_cpus) {
            if (cpu) {
                __atomic_store_n(&cpu->poll, 1, __ATOMIC_RELAXED);
            }
        }
    }
}

std::optional<ScarletNxProcess::CodeRange> ScarletNxProcess::ExecutableRange(u64 pc) const {
    const auto& page_table = m_process.GetPageTable().GetBasePageTable();
    Kernel::KMemoryInfo info;
    Kernel::Svc::PageInfo page_info;
    if (page_table.QueryInfo(&info, &page_info, pc).IsError()) {
        return std::nullopt;
    }
    if (!True(info.GetPermission() & Kernel::KMemoryPermission::UserExecute)) {
        // The loader puts the generated thunk slots in SDK data so rtld sees one module.
        // Permit only the bridge's code for this process. The target table stays data.
        const auto base = NvnHle::Bridge::ThunkCodeBaseFor(&m_process);
        if (base && pc - *base < NvnHle::Thunk::CodeBytes &&
            True(info.GetPermission() & Kernel::KMemoryPermission::UserRead)) {
            return CodeRange{*base, NvnHle::Thunk::CodeBytes};
        }
        return std::nullopt;
    }
    const u64 start = info.GetAddress();
    const u64 offset = (pc - start) / MaxImageSize * MaxImageSize;
    return CodeRange{start + offset, (std::min)(info.GetSize() - offset, MaxImageSize)};
}

ScarletNxProcess::SnxGuestFn ScarletNxProcess::FunctionAt(u64 pc) const {
    if (pc >= ArenaSize || (pc & 3) != 0) {
        return nullptr;
    }
    return __atomic_load_n(&m_dispatch[pc >> 2], __ATOMIC_ACQUIRE);
}

bool ScarletNxProcess::CompileLocked(CodeRange range, u64 pc) {
    auto& words = m_images[range.addr];
    if (words.size() * sizeof(u32) != range.size) {
        words.resize(range.size / sizeof(u32));
        if (!m_process.GetMemory().ReadBlockUnsafe(range.addr, words.data(), range.size)) {
            m_images.erase(range.addr);
            return false;
        }
        for (u32& instruction : words) {
            if ((instruction & ~31u) == ScarletNxCntfrqMrs) {
                instruction = 0xd4000001 | ((ScarletNxCntfrqTrap | (instruction & 31u)) << 5);
            }
        }
    }

    const snx_code_range image{range.addr, words.data(), range.size};
    struct Discovery {
        ScarletNxProcess* process;
        u64 begin;
    } discovery{this, pc & ~(CompileWindow - 1)};
    const auto skip = [](void* context, u64 address) -> int {
        const auto& bounds = *static_cast<Discovery*>(context);
        return address - bounds.begin >= CompileWindow || bounds.process->FunctionAt(address);
    };
    auto* const code =
        snx_recompile_from(m_recompiler, &image, 1, &pc, 1, InstructionsPerBatch, skip, &discovery);
    if (!code) {
        LOG_WARNING(Core_ARM, "ScarletNX: compile failed at {:#x}: {}", pc,
                    snx_recompiler_error(m_recompiler));
        return false;
    }

    u64 lo, hi;
    snx_recompiled_range(code, &lo, &hi);
    if (hi <= lo || hi > ArenaSize) {
        snx_recompiled_free(code);
        return false;
    }
    m_modules.push_back({code, lo, hi});
    for (u64 address = lo; address < hi; address += sizeof(u32)) {
        if (const auto function = snx_recompiled_function(code, address)) {
            SnxGuestFn empty = nullptr;
            __atomic_compare_exchange_n(&m_dispatch[address >> 2], &empty, function, false,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        }
    }
    return true;
}

ScarletNxProcess::SnxGuestFn ScarletNxProcess::Lookup(u64 pc) {
    if (pc >= ArenaSize || (pc & 3) != 0 || FallbackRequested()) {
        return nullptr;
    }
    while (true) {
        if (const auto function = FunctionAt(pc)) {
            return function;
        }
        // Page-table mutators can invalidate code while holding their table lock.
        const u64 generation = CodeGeneration();
        const auto range = ExecutableRange(pc);
        std::scoped_lock lock{m_mutex};
        if (generation != CodeGeneration()) {
            continue;
        }
        if (const auto function = FunctionAt(pc)) {
            return function;
        }
        if (!range) {
            LOG_WARNING(Core_ARM, "ScarletNX: no executable code range for guest PC {:#x}", pc);
            return nullptr;
        }
        if (!CompileLocked(*range, pc)) {
            return nullptr;
        }
        const auto function = FunctionAt(pc);
        if (!function) {
            LOG_WARNING(Core_ARM, "ScarletNX: compiled batch has no entry for guest PC {:#x}", pc);
        }
        return function;
    }
}

bool ScarletNxProcess::HandleCounterRead(CPU& cpu, u32 svc) {
    if ((svc & ~31u) != ScarletNxCntfrqTrap || cpu.pc < sizeof(u32)) {
        return false;
    }
    const u32 reg = svc & 31u;
    u32 original;
    if (!m_process.GetMemory().ReadBlockUnsafe(cpu.pc - sizeof(u32), &original, sizeof(original)) ||
        original != (ScarletNxCntfrqMrs | reg)) {
        return false;
    }
    if (reg != 31) {
        cpu.x[reg] = Common::g_wall_clock.GetGuestCNTFRQ();
    }
    return true;
}

void ScarletNxProcess::Retire(Module& module) {
    for (u64 address = module.lo; address < module.hi; address += sizeof(u32)) {
        if (auto function = snx_recompiled_function(module.code, address)) {
            __atomic_compare_exchange_n(&m_dispatch[address >> 2], &function, nullptr, false,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED);
        }
    }
    snx_recompiled_invalidate(module.code, module.lo, module.hi - module.lo, nullptr, nullptr);
    module.retired = true;
    // Other cores may still have frames using this host code. Free it when the process ends.
}

void ScarletNxProcess::InvalidateCacheRange(u64 addr, u64 size) {
    if (size == 0 || addr >= ArenaSize) {
        return;
    }
    const u64 end = addr + (std::min)(size, ArenaSize - addr);
    std::scoped_lock lock{m_mutex};
    std::erase_if(m_images, [&](const auto& entry) {
        return entry.first < end && addr < entry.first + entry.second.size() * sizeof(u32);
    });
    for (auto& mod : m_modules) {
        if (!mod.retired && mod.lo < end && addr < mod.hi) {
            Retire(mod);
        }
    }
    m_code_generation.fetch_add(1, std::memory_order_release);
}

void ScarletNxProcess::ClearInstructionCache() {
    std::scoped_lock lock{m_mutex};
    m_images.clear();
    for (auto& mod : m_modules) {
        if (!mod.retired) {
            Retire(mod);
        }
    }
    m_code_generation.fetch_add(1, std::memory_order_release);
}

} // namespace Core
