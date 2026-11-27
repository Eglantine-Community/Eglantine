// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cfenv>
#include <cstring>

#include <sys/mman.h>

#include "common/literals.h"
#include "common/logging.h"
#include "core/arm/scarletnx/arm_scarletnx_64.h"
#include "core/core.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"

namespace Core {

namespace {

using namespace Common::Literals;
namespace ctx = boost::context::detail;

// Reserved, not committed: guest calls nest on it, and recompiling a region runs on it too.
constexpr std::size_t StackSize = 32_MiB;
constexpr std::size_t GuardSize = 64_KiB;

bool SameContext(const Kernel::Svc::ThreadContext& a, const Kernel::Svc::ThreadContext& b,
                 bool in_svc) {
    // An SVC returns its results in x0-x7.
    for (std::size_t i = in_svc ? 8 : 0; i < a.r.size(); ++i) {
        if (a.r[i] != b.r[i]) {
            return false;
        }
    }
    return a.fp == b.fp && a.lr == b.lr && a.sp == b.sp && a.pc == b.pc &&
           (a.pstate >> 28) == (b.pstate >> 28) && a.v == b.v && a.fpcr == b.fpcr &&
           a.fpsr == b.fpsr && a.tpidr == b.tpidr;
}

} // namespace

const RtAPI* ArmScarletNx64::Runtime() {
    static const RtAPI rt = [] {
        RtAPI api;
        snx_runtime_defaults(&api);
        api.svc = &Svc;
        api.undef = &Undefined;
        api.brk = &Breakpoint;
        api.poll = &Poll;
        api.miss = &Miss;
        api.code_changed = &CodeChanged;
        api.ticks = &ScarletNxTicks;
        return api;
    }();
    return &rt;
}

ArmScarletNx64::ArmScarletNx64(std::shared_ptr<ScarletNxProcess> process,
                               std::unique_ptr<ArmInterface> fallback, std::size_t core_index)
    : ArmInterface{true}, m_process{std::move(process)}, m_fallback{std::move(fallback)},
      m_core_index{core_index} {
    void* const stack =
        mmap(nullptr, StackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED || mprotect(stack, GuardSize, PROT_NONE) != 0) {
        if (stack != MAP_FAILED) {
            munmap(stack, StackSize);
        }
        m_process->UseFallback(0, "cannot allocate the execution stack");
        return;
    }
    m_stack = static_cast<u8*>(stack);

    m_cpu.mem = m_process->m_arena;
    m_cpu.rt = Runtime();
    m_cpu.thread = this;
    m_process->Bind(m_cpu, core_index);
    m_fault_scope.memory = &m_process->m_process.GetMemory();
    m_fault_scope.timing = &m_process->m_system.CoreTiming();
    m_fault_scope.arena = m_process->m_arena;
    m_fault_scope.arena_size = ScarletNxProcess::ArenaSize;
}

ArmScarletNx64::~ArmScarletNx64() {
    LOG_INFO(Core_ARM,
             "ScarletNX: core {} summary: AOT entries={} dispatch misses served={} "
             "SVCs={} CNTFRQ reads={} Dynarmic entries={}",
             m_core_index, m_scarlet_runs, m_scarlet_dispatches, m_svc_count, m_cntfrq_count,
             m_fallback_runs);
    if (m_stack) {
        munmap(m_stack, StackSize);
    }
}

void ArmScarletNx64::Svc(CPU* cpu, u32 imm) {
    auto& self = From(cpu);
    if (self.m_process->HandleCounterRead(*cpu, imm)) {
        ++self.m_cntfrq_count;
        return;
    }
    if (self.m_svc_count++ == 0) {
        LOG_INFO(Core_ARM, "ScarletNX: core {} first guest SVC {:#x}", self.m_core_index, imm);
    }
    self.m_svc = imm;
    self.Yield(HaltReason::SupervisorCall, true, true);
}

void ArmScarletNx64::Undefined(CPU* cpu, u64 pc, u32 word) {
    auto& self = From(cpu);
    if (cpu->pc == pc) {
        self.m_process->UseFallback(pc, "unsupported guest instruction");
        self.Yield(HaltReason::BreakLoop, false, false);
        return;
    }
    // A runtime helper can report a reserved encoding after starting its instruction.
    LOG_ERROR(Core_ARM, "ScarletNX: reserved encoding {:08X} at {:#x}", word, pc);
    cpu->pc = pc;
    self.Yield(HaltReason::PrefetchAbort, false, false);
}

void ArmScarletNx64::Breakpoint(CPU* cpu, u64 pc, u32 imm) {
    LOG_ERROR(Core_ARM, "ScarletNX: BRK #{:#x} at {:#x}", imm, pc);
    cpu->pc = pc;
    From(cpu).Yield(HaltReason::InstructionBreakpoint, false, false);
}

void ArmScarletNx64::Poll(CPU* cpu) {
    auto& self = From(cpu);
    __atomic_store_n(&cpu->poll, 0, __ATOMIC_RELAXED);
    if (self.m_process->FallbackRequested() || self.m_interrupt.exchange(false)) {
        self.Yield(HaltReason::BreakLoop, true, false);
    }
}

void ArmScarletNx64::Miss(CPU* cpu, u64 pc) {
    auto& self = From(cpu);
    // Call targets can cross executable mappings (including NVN's generated SVC table).
    if (const auto function = self.m_process->Lookup(pc)) {
        if (self.m_scarlet_dispatches++ == 0) {
            LOG_INFO(Core_ARM, "ScarletNX: core {} first guest dispatch at {:#x}",
                     self.m_core_index, pc);
        }
        function(cpu, pc);
        return;
    }
    cpu->pc = pc;
    self.m_process->UseFallback(pc, "cannot compile or dispatch guest code");
    self.Yield(HaltReason::BreakLoop, false, false);
}

void ArmScarletNx64::CodeChanged(CPU* cpu, u64 addr) {
    constexpr u64 ICacheLineSize = 64;
    From(cpu).m_process->InvalidateCacheRange(addr & ~(ICacheLineSize - 1), ICacheLineSize);
}

void ArmScarletNx64::Entry(ctx::transfer_t transfer) {
    auto* const self = static_cast<ArmScarletNx64*>(transfer.data);
    self->m_host = transfer.fctx;
    // ScarletNX applies guest FPCR relative to the default host environment. Boost.Context
    // saves this fiber's FP controls across SVCs and restores the caller's on return.
    std::fesetenv(FE_DFL_ENV);
    self->Execute();
}

void ArmScarletNx64::Execute() {
    if (sigsetjmp(m_fault_scope.env, 1) != 0) {
        m_fault_scope.CompleteWrites();
        LOG_CRITICAL(Core_ARM, "invalid memory access at {:#x}", m_fault_scope.fault_address);
        Yield(HaltReason::PrefetchAbort, false, false);
    }

    while (true) {
        snx_lookup(&m_cpu, m_cpu.pc)(&m_cpu, m_cpu.pc);
    }
}

void ArmScarletNx64::Yield(HaltReason reason, bool resumable, bool in_svc) {
    m_halt = reason;
    m_resumable = resumable;
    m_suspended_in_svc = in_svc;
    m_host = ctx::jump_fcontext(m_host, nullptr).fctx;
}

void ArmScarletNx64::ResetGuest() {
    m_guest = ctx::make_fcontext(m_stack + StackSize, StackSize - GuardSize, &Entry);
}

HaltReason ArmScarletNx64::RunFallback(Kernel::KThread* thread, bool step) {
    ++m_fallback_runs;
    m_last_run_fallback = true;
    m_resumable = false;
    m_fallback->SetWatchpointArray(m_watchpoints);
    m_fallback->SetContext(m_ctx);
    m_fallback->SetTpidrroEl0(m_tpidrro);
    const HaltReason halt = step ? m_fallback->StepThread(thread) : m_fallback->RunThread(thread);
    m_fallback->GetContext(m_ctx);
    m_svc = m_fallback->GetSvcNumber();
    return halt;
}

HaltReason ArmScarletNx64::RunThread(Kernel::KThread* thread) {
    if (m_interrupt.exchange(false)) {
        return HaltReason::BreakLoop;
    }
    if (m_process->FallbackRequested()) {
        return RunFallback(thread, false);
    }

    ++m_scarlet_runs;
    m_last_run_fallback = false;
    const u64 generation = m_process->CodeGeneration();
    const bool resume = m_resumable && m_suspended_generation == generation &&
                        m_suspended_thread_id == thread->GetThreadId() &&
                        SameContext(m_ctx, m_suspended_ctx, m_suspended_in_svc);
    LoadCpu();
    if (!resume) {
        ResetGuest();
    }

    m_resumable = false;
    m_fault_scope.retries = 0;
    SetScarletNxFaultScope(&m_fault_scope);
    m_guest = ctx::jump_fcontext(m_guest, this).fctx;
    SetScarletNxFaultScope(nullptr);

    SaveCpu();
    if (m_resumable) {
        m_suspended_thread_id = thread->GetThreadId();
        m_suspended_ctx = m_ctx;
        m_suspended_generation = generation;
    }
    if (m_halt == HaltReason::BreakLoop && m_process->FallbackRequested()) {
        return RunFallback(thread, false);
    }
    return m_halt;
}

HaltReason ArmScarletNx64::StepThread(Kernel::KThread* thread) { return RunFallback(thread, true); }

void ArmScarletNx64::LoadCpu() {
    static_assert(sizeof(m_cpu.v) == sizeof(m_ctx.v));
    for (std::size_t i = 0; i < m_ctx.r.size(); i++)
        m_cpu.x[i] = m_ctx.r[i];
    m_cpu.x[29] = m_ctx.fp;
    m_cpu.x[30] = m_ctx.lr;
    m_cpu.x[31] = m_ctx.sp;
    m_cpu.pc = m_ctx.pc;
    set_nzcv(&m_cpu, m_ctx.pstate >> 28);
    std::memcpy(m_cpu.v, m_ctx.v.data(), sizeof(m_cpu.v));
    m_cpu.fpcr = m_ctx.fpcr;
    m_cpu.fpsr = m_ctx.fpsr;
    m_cpu.tpidr = m_ctx.tpidr;
    m_cpu.tpidrro = m_tpidrro;
    m_cpu.excl_on = 0;
}

void ArmScarletNx64::SaveCpu() {
    for (std::size_t i = 0; i < m_ctx.r.size(); i++) {
        m_ctx.r[i] = m_cpu.x[i];
    }
    m_ctx.fp = m_cpu.x[29];
    m_ctx.lr = m_cpu.x[30];
    m_ctx.sp = m_cpu.x[31];
    m_ctx.pc = m_cpu.pc;
    m_ctx.pstate = (m_ctx.pstate & 0x0fff'ffff) | get_nzcv(&m_cpu);
    std::memcpy(m_ctx.v.data(), m_cpu.v, sizeof(m_cpu.v));
    m_ctx.fpcr = m_cpu.fpcr;
    m_ctx.fpsr = m_cpu.fpsr;
    m_ctx.tpidr = m_cpu.tpidr;
}

void ArmScarletNx64::GetContext(Kernel::Svc::ThreadContext& ctx) const { ctx = m_ctx; }

void ArmScarletNx64::SetContext(const Kernel::Svc::ThreadContext& ctx) { m_ctx = ctx; }

void ArmScarletNx64::SetTpidrroEl0(u64 value) { m_tpidrro = value; }

void ArmScarletNx64::GetSvcArguments(std::span<uint64_t, 8> args) const {
    for (std::size_t i = 0; i < args.size(); i++) {
        args[i] = m_ctx.r[i];
    }
}

void ArmScarletNx64::SetSvcArguments(std::span<const uint64_t, 8> args) {
    for (std::size_t i = 0; i < args.size(); i++) {
        m_ctx.r[i] = args[i];
    }
}

u32 ArmScarletNx64::GetSvcNumber() const { return m_svc; }

const Kernel::DebugWatchpoint* ArmScarletNx64::HaltedWatchpoint() const {
    return m_last_run_fallback ? m_fallback->HaltedWatchpoint() : nullptr;
}

void ArmScarletNx64::RewindBreakpointInstruction() {
    if (m_last_run_fallback) {
        m_fallback->RewindBreakpointInstruction();
        m_fallback->GetContext(m_ctx);
    }
}

void ArmScarletNx64::SignalInterrupt(Kernel::KThread* thread) {
    if (m_process->FallbackRequested()) {
        m_fallback->SignalInterrupt(thread);
    }
    // Generated code reads poll on backward branches and calls Poll, which yields.
    m_interrupt.store(true);
    __atomic_store_n(&m_cpu.poll, 1, __ATOMIC_RELAXED);
}

void ArmScarletNx64::ClearInstructionCache() {
    m_fallback->ClearInstructionCache();
    m_process->ClearInstructionCache();
}

void ArmScarletNx64::InvalidateCacheRange(u64 addr, std::size_t size) {
    m_fallback->InvalidateCacheRange(addr, size);
    m_process->InvalidateCacheRange(addr, size);
}

} // namespace Core
