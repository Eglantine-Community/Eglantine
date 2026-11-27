// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <csetjmp>
#include <map>
#include <memory>
#include <optional>
#include <mutex>
#include <vector>

#include "common/common_types.h"
#include "core/hardware_properties.h"

struct CPU;
struct snx_recompiler;
struct snx_recompiled;

namespace Kernel {
class KProcess;
}

namespace Core::Timing {
class CoreTiming;
}

namespace Core::Memory {
class Memory;
}

namespace Core {

class ArmInterface;
class System;

// MRS Xt, CNTFRQ_EL0, which upstream ScarletNX otherwise emits as a constant 19.2 MHz.
inline constexpr u32 ScarletNxCntfrqMrs = 0xd53be000;
inline constexpr u32 ScarletNxCntfrqTrap = 0x7fe0;

class ScarletNxProcess {
  public:
    using SnxGuestFn = void (*)(CPU*, u64);

    // DeviceMemory reserves 39 bits of address space for the fastmem arena.
    static constexpr u64 ArenaSize = 1ULL << 39;

    struct CodeRange {
        u64 addr;
        u64 size;
    };

    ScarletNxProcess(System& system, Kernel::KProcess& process);
    ~ScarletNxProcess();

    static bool ShouldUse(System& system, const Kernel::KProcess& process);
    [[nodiscard]] bool Ready() const { return m_recompiler != nullptr && m_dispatch != nullptr; }
    void Bind(CPU& cpu, std::size_t core);
    SnxGuestFn Lookup(u64 pc);
    [[nodiscard]] bool HandleCounterRead(CPU& cpu, u32 svc);
    void UseFallback(u64 pc, const char* reason);
    [[nodiscard]] bool FallbackRequested() const {
        return m_fallback.load(std::memory_order_relaxed);
    }

    void InvalidateCacheRange(u64 addr, u64 size);
    void ClearInstructionCache();

    /// @brief general gate for when we are about to invalidate a given block aka. trigger regen
    u64 CodeGeneration() const { return m_code_generation.load(std::memory_order_acquire); }

    struct Module {
        snx_recompiled* code;
        u64 lo;
        u64 hi;
        bool retired{};
    };

    std::optional<CodeRange> ExecutableRange(u64 pc) const;
    bool CompileLocked(CodeRange range, u64 pc);
    SnxGuestFn FunctionAt(u64 pc) const;
    void Retire(Module& module);

    std::vector<Module> m_modules;
    std::map<u64, std::vector<u32>> m_images;
    SnxGuestFn* m_dispatch{};
    std::array<CPU*, Hardware::NUM_CPU_CORES> m_cpus{};
    std::atomic<u64> m_code_generation{};
    std::atomic<bool> m_fallback{};

    // TODO: remove parent ref?
    System& m_system;
    Kernel::KProcess& m_process;
    u8* m_arena{};
    snx_recompiler* m_recompiler{};

    // ScarletNX's recompiler is used by one thread at a time.
    std::mutex m_mutex;
};

/// Recovers from host faults in guest memory on the thread running guest code: a fault on a
/// page the rasterizer protects is resolved and retried, any other jumps to env.
struct ScarletNxFaultScope {
    sigjmp_buf env;
    Memory::Memory* memory{};
    Timing::CoreTiming* timing{};
    u8* arena{};
    u64 arena_size{};
    u64 retry_address{};
    u64 retry_host_pc{};
    u32 retries{};
    u64 fault_address{};
    std::array<u64, 2> pending_writes{};
    u32 pending_write_count{};
    bool previous_trace_flag{};

    void CompleteWrites();
};

bool InstallScarletNxFaultHandler();
void SetScarletNxFaultScope(ScarletNxFaultScope* scope);

/// CNTPCT_EL0 of the emulated system.
u64 ScarletNxTicks();

} // namespace Core
