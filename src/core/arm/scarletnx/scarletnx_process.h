// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <atomic>
#include <csetjmp>
#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <signal.h>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/common_types.h"
#include "common/page_table.h"

struct CPU;
struct snx_recompiler;
struct snx_recompiled;

namespace Kernel {
class KProcess;
}

namespace Core::Memory {
class Memory;
}

namespace Core {

class System;
class ScarletNxProfile;
namespace Timing {
class CoreTiming;
}

class ScarletNxProcess {
public:
    using SnxGuestFn = void (*)(CPU*, u64);

    // DeviceMemory reserves 39 bits of address space for the fastmem arena.
    static constexpr u64 ArenaSize = 1ULL << 39;

    struct Function {
        snx_recompiled* module{};
        SnxGuestFn entry{};
        bool failed{};
    };

    ScarletNxProcess(System& system, Kernel::KProcess& process);
    ~ScarletNxProcess();

    static bool ShouldUse(System& system, const Kernel::KProcess& process);
    static constexpr std::string_view MemoryModeFallback(bool fastmem, bool reactive_flushing) {
        if (!fastmem) {
            return "requires fastmem";
        }
        if (reactive_flushing) {
            return "requires Reactive Flushing disabled";
        }
        return {};
    }

    Function FindFunction(u64 pc);
    void Resolve(u64 pc);

    void InvalidateCacheRange(u64 addr, u64 size);
    void ClearInstructionCache();

    u64 CodeGeneration() const {
        return m_code_generation.load(std::memory_order_acquire);
    }

private:
    friend class ArmScarletNx64;

    struct CodeRange {
        u64 addr;
        u64 size;
    };

    struct Module {
        CodeRange range;
        u64 lo;
        u64 hi;
        snx_recompiled* mod;
    };

    std::optional<CodeRange> GetCodeRange(u64 pc) const;
    Function FindFunctionLocked(u64 pc) const;
    void CountDispatchMiss(const Function& function);

    std::unordered_map<u64, std::vector<Module>> m_windows;
    std::vector<snx_recompiled*> m_retired_modules;
    std::unordered_set<u64> m_failed_entries;
    std::atomic<u64> m_code_generation{};

    System& m_system;
    Kernel::KProcess& m_process;
    u8* m_arena{};
    const std::size_t m_compile_instruction_budget;
    snx_recompiler* m_recompiler{};
    std::unique_ptr<ScarletNxProfile> m_profile;

    // One compilation at a time, without blocking dispatch or invalidation.
    std::mutex m_compile_mutex;
    std::mutex m_mutex;
};

enum class ScarletNxFaultAction {
    NotDataProtectionFault,
    NotCachedBacking,
    RetryAlreadyRequested,
    ReadCache,
    InvalidateCache,
};

struct ScarletNxFault {
    u64 address{};
    u64 host_pc{};
    u64 x86_error{};
    int signal{};
    int signal_code{};
    int x86_trap{};
    Common::PageType page_type{Common::PageType::Unmapped};
    u16 block{};
    bool marked{};
    uintptr_t backing{};

    constexpr bool IsWrite() const {
        return (x86_error & 0x2) != 0;
    }

    constexpr bool IsInstructionFetch() const {
        return (x86_error & 0x10) != 0;
    }

    constexpr ScarletNxFaultAction SelectAction(const ScarletNxFault& attempted) const {
        constexpr int X86PageFault = 14;
        if (signal != SIGSEGV || signal_code != SEGV_ACCERR || x86_trap != X86PageFault ||
            IsInstructionFetch()) {
            return ScarletNxFaultAction::NotDataProtectionFault;
        }
        if (page_type != Common::PageType::RasterizerCachedMemory || !backing) {
            return ScarletNxFaultAction::NotCachedBacking;
        }
        if (attempted.signal && address == attempted.address && host_pc == attempted.host_pc &&
            IsWrite() == attempted.IsWrite()) {
            return ScarletNxFaultAction::RetryAlreadyRequested;
        }
        return IsWrite() ? ScarletNxFaultAction::InvalidateCache : ScarletNxFaultAction::ReadCache;
    }
};

/// A fastmem fault on a rasterizer-cached page can request one retry at the same host instruction.
/// Unhandled faults leave generated code via env.
struct ScarletNxFaultScope {
    sigjmp_buf env;
    Memory::Memory* memory{};
    Timing::CoreTiming* timing{};
    const Common::PageTable* page_table{};
    u8* arena{};
    u64 arena_size{};
    ScarletNxFault fault{};
    ScarletNxFault last_attempt{};
    ScarletNxFaultAction action{};
    bool last_attempt_returned{};
};

void InstallScarletNxFaultHandler();
void SetScarletNxFaultScope(ScarletNxFaultScope* scope);

/// CNTPCT_EL0 of the emulated system.
u64 ScarletNxTicks();

} // namespace Core
