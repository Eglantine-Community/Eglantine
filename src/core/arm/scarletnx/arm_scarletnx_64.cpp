// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstring>
#include <string_view>

#include <sys/mman.h>

#include "common/assert.h"
#include "common/cpu_features.h"
#include "common/literals.h"
#include "common/logging.h"
#include "common/settings.h"
#include "core/arm/scarletnx/arm_scarletnx_64.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_thread.h"

namespace Core {

namespace {

using namespace Common::Literals;
namespace ctx = boost::context::detail;

// Reserved, not committed: guest calls nest on it.
constexpr std::size_t StackSize = 32_MiB;
constexpr std::size_t GuardSize = 64_KiB;

bool SameContext(const Kernel::Svc::ThreadContext& a, const Kernel::Svc::ThreadContext& b,
                 bool in_svc) {
    // An SVC returns its results in x0-x7.
    for (std::size_t i = in_svc ? 8 : 0; i < a.r.size(); i++)
        if (a.r[i] != b.r[i])
            return false;
    return a.fp == b.fp && a.lr == b.lr && a.sp == b.sp && a.pc == b.pc
        && (a.pstate >> 28) == (b.pstate >> 28) && a.v == b.v && a.fpcr == b.fpcr
        && a.fpsr == b.fpsr && a.tpidr == b.tpidr;
}

std::string_view FaultActionName(ScarletNxFaultAction action) {
    switch (action) {
    case ScarletNxFaultAction::NotDataProtectionFault:
        return "not-data-protection-fault";
    case ScarletNxFaultAction::NotCachedBacking:
        return "no-cached-backing";
    case ScarletNxFaultAction::RetryAlreadyRequested:
        return "retry-already-requested";
    case ScarletNxFaultAction::ReadCache:
        return "read-block-failed";
    case ScarletNxFaultAction::InvalidateCache:
        return "invalidate-nce-failed";
    }
    return "unknown";
}

std::string_view FaultAccess(const ScarletNxFault& fault) {
    if (fault.x86_trap != 14) {
        return "unknown";
    }
    if (fault.IsInstructionFetch()) {
        return "execute";
    }
    return fault.IsWrite() ? "write" : "read";
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

ArmScarletNx64::ArmScarletNx64(std::shared_ptr<ScarletNxProcess> process)
    : ArmInterface{true}, m_process{std::move(process)} {
    void* const stack = mmap(nullptr, StackSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    ASSERT_MSG(stack != MAP_FAILED, "cannot allocate a guest stack");
    m_stack = static_cast<u8*>(stack);
    mprotect(m_stack, GuardSize, PROT_NONE);

    m_cpu.mem = m_process->m_arena;
    m_cpu.rt = Runtime();
    m_cpu.thread = this;
    m_cpu.cntfrq = static_cast<u32>(Common::g_wall_clock.GetGuestCNTFRQ());

    m_fault_scope.memory = &m_process->m_process.GetMemory();
    m_fault_scope.timing = &m_process->m_system.CoreTiming();
    m_fault_scope.page_table = &m_process->m_process.GetPageTable().GetImpl();
    m_fault_scope.arena = m_process->m_arena;
    m_fault_scope.arena_size = ScarletNxProcess::ArenaSize;
}

ArmScarletNx64::~ArmScarletNx64() {
    munmap(m_stack, StackSize);
}

void ArmScarletNx64::Svc(CPU* cpu, u32 imm) {
    auto& self = From(cpu);
    self.m_svc = imm;
    self.Yield(HaltReason::SupervisorCall, true, true);
}

void ArmScarletNx64::Undefined(CPU* cpu, u64 pc, u32 word) {
    // A reserved encoding rejected from inside an instruction, which then goes on.
    if (cpu->pc != pc) {
        LOG_ERROR(Core_ARM, "reserved encoding {:08X} at {:#x}", word, pc);
        return;
    }
    LOG_CRITICAL(Core_ARM, "cannot execute {:08X} at {:#x}", word, pc);
    From(cpu).Yield(HaltReason::PrefetchAbort, false, false);
    UNREACHABLE();
}

void ArmScarletNx64::Breakpoint(CPU* cpu, u64 pc, u32 imm) {
    // As Dynarmic does without a debugger: report it and go on.
    LOG_CRITICAL(Core_ARM, "BRK #{:#x} at {:#x}", imm, pc);
    cpu->pc = pc + 4;
}

void ArmScarletNx64::Poll(CPU* cpu) {
    auto& self = From(cpu);
    __atomic_store_n(&cpu->poll, 0, __ATOMIC_RELAXED);
    if (self.m_interrupt.exchange(false))
        self.Yield(HaltReason::BreakLoop, true, false);
}

void ArmScarletNx64::Miss(CPU* cpu, u64 pc) {
    auto& self = From(cpu);
    if (self.m_interrupt.exchange(false)) {
        cpu->pc = pc;
        self.Yield(HaltReason::BreakLoop, false, false);
        UNREACHABLE();
    }
    const auto function = self.m_process->FindFunction(pc);
    if (self.m_process->m_profile) {
        self.m_process->CountDispatchMiss(function);
    }
    if (function.entry) {
        snx_recompiled_bind(function.module, cpu);
        function.entry(cpu, pc);
        return;
    }
    cpu->pc = pc;
    if (function.failed) {
        Undefined(cpu, pc, 0);
    } else {
        self.Yield(HaltReason::CodeCacheMiss, false, false);
    }
    UNREACHABLE();
}

void ArmScarletNx64::CodeChanged(CPU* cpu, u64 addr) {
    constexpr u64 ICacheLineSize = 64;
    auto& process = *From(cpu).m_process;
    process.InvalidateCacheRange(addr & ~(ICacheLineSize - 1), ICacheLineSize);
    for (size_t i = 0; i < Hardware::NUM_CPU_CORES; ++i) {
        process.m_process.GetArmInterface(i)->SignalInterrupt(nullptr);
    }
}

void ArmScarletNx64::Entry(ctx::transfer_t transfer) {
    auto* const self = static_cast<ArmScarletNx64*>(transfer.data);
    self->m_host = transfer.fctx;
    self->Execute();
}

void ArmScarletNx64::Execute() {
    if (sigsetjmp(m_fault_scope.env, 1) != 0) {
        Yield(HaltReason::PrefetchAbort, false, false);
        UNREACHABLE();
    }

    // Every guest return to below the first frame comes back here.
    while (true)
        snx_lookup(&m_cpu, m_cpu.pc)(&m_cpu, m_cpu.pc);
}

void ArmScarletNx64::Yield(HaltReason reason, bool resumable, bool in_svc) {
    m_halt = reason;
    m_resumable = resumable;
    m_suspended_in_svc = in_svc;
    m_host = ctx::jump_fcontext(m_host, nullptr).fctx;
}

void ArmScarletNx64::ResetGuest() {
    m_guest = ctx::make_fcontext(m_stack + StackSize, StackSize - GuardSize, &Entry);
    m_cpu.disp = nullptr;
    m_cpu.code_lo = m_cpu.code_hi = 0;
}

HaltReason ArmScarletNx64::RunThread(Kernel::KThread* thread) {
    if (m_interrupt.exchange(false))
        return HaltReason::BreakLoop;

    const auto generation = m_process->CodeGeneration();
    const bool resume = m_resumable && m_suspended_thread_id == thread->GetThreadId() &&
                        m_suspended_generation == generation &&
                        SameContext(m_ctx, m_suspended_ctx, m_suspended_in_svc);
    LoadCpu();
    if (!resume)
        ResetGuest();

    m_resumable = false;
    m_fault_scope.fault.signal = 0;
    m_fault_scope.last_attempt.signal = 0;
    SetScarletNxFaultScope(&m_fault_scope);
    m_guest = ctx::jump_fcontext(m_guest, this).fctx;
    SetScarletNxFaultScope(nullptr);

    SaveCpu();
    if (m_resumable) {
        m_suspended_thread_id = thread->GetThreadId();
        m_suspended_generation = generation;
        m_suspended_ctx = m_ctx;
    }
    return m_halt;
}

void ArmScarletNx64::ResolveCodeCacheMiss() {
    // QueryInfo can reschedule. Capture the PC before handing control back to the kernel.
    m_process->Resolve(m_ctx.pc);
}

void ArmScarletNx64::ReportMemoryFault() {
    const auto fault = m_fault_scope.fault;
    if (!fault.signal) {
        return;
    }
    const auto attempt = m_fault_scope.last_attempt;
    auto& process = *m_process;
    const auto physical_address = [&](const ScarletNxFault& sample) {
        return sample.backing
                   ? GetInteger(process.m_system.DeviceMemory().GetPhysicalAddr(sample.backing))
                   : 0;
    };
    // Generated code does not materialize the guest PC for every load or store.
    LOG_CRITICAL(Core_ARM,
                 "ScarletNX fastmem fault: va={:#x} saved_pc={:#x} host_rip={:#x} access={} "
                 "sig={} si_code={} x86_trap={} x86_error={:#x} page_tag={} marked={} "
                 "block={} backing={:#x} pa={:#x} disposition={} reactive_flushing_setting={}",
                 fault.address, m_ctx.pc, fault.host_pc, FaultAccess(fault), fault.signal,
                 fault.signal_code, fault.x86_trap, fault.x86_error, static_cast<u8>(fault.page_type),
                 fault.marked, fault.block, fault.backing, physical_address(fault),
                 FaultActionName(m_fault_scope.action),
                 Settings::values.use_reactive_flushing.GetValue());
    if (attempt.signal) {
        LOG_CRITICAL(Core_ARM,
                     "ScarletNX last fastmem retry: va={:#x} host_rip={:#x} access={} "
                     "x86_error={:#x} page_tag={} backing={:#x} pa={:#x} handler={} "
                     "handler_returned={} (true requests replay, not proof of success)",
                     attempt.address, attempt.host_pc, FaultAccess(attempt), attempt.x86_error,
                     static_cast<u8>(attempt.page_type), attempt.backing, physical_address(attempt),
                     attempt.IsWrite() ? "InvalidateNCE" : "ReadBlock",
                     m_fault_scope.last_attempt_returned);
    }

    // The scheduler-aware query runs after ExitContext. It describes the later guest mapping,
    // not the permissions at the instant of the signal.
    const auto& table = process.m_process.GetPageTable().GetBasePageTable();
    Kernel::KMemoryInfo info{};
    Kernel::Svc::PageInfo page_info{};
    if (table.QueryInfo(&info, &page_info, fault.address).IsSuccess()) {
        const auto region = info.GetSvcMemoryInfo();
        LOG_CRITICAL(Core_ARM,
                     "ScarletNX fastmem mapping after CPU exit: va={:#x} base={:#x} size={:#x} "
                     "state={:#x} guest_perm={:#x}",
                     fault.address, region.base_address, region.size, static_cast<u32>(region.state),
                     static_cast<u32>(region.permission));
    }
}

HaltReason ArmScarletNx64::StepThread(Kernel::KThread*) {
    // Never reached: the debugger selects Dynarmic.
    LOG_ERROR(Core_ARM, "ScarletNX cannot single step");
    return HaltReason::BreakLoop;
}

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
    m_ctx.pstate = get_nzcv(&m_cpu);
    std::memcpy(m_ctx.v.data(), m_cpu.v, sizeof(m_cpu.v));
    m_ctx.fpcr = m_cpu.fpcr;
    m_ctx.fpsr = m_cpu.fpsr;
    m_ctx.tpidr = m_cpu.tpidr;
}

void ArmScarletNx64::GetContext(Kernel::Svc::ThreadContext& ctx) const {
    ctx = m_ctx;
}

void ArmScarletNx64::SetContext(const Kernel::Svc::ThreadContext& ctx) {
    m_ctx = ctx;
}

void ArmScarletNx64::SetTpidrroEl0(u64 value) {
    m_tpidrro = value;
}

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

u32 ArmScarletNx64::GetSvcNumber() const {
    return m_svc;
}

void ArmScarletNx64::SignalInterrupt(Kernel::KThread*) {
    // Generated code reads poll on backward branches and calls Poll, which yields.
    m_interrupt.store(true);
    __atomic_store_n(&m_cpu.poll, 1, __ATOMIC_RELAXED);
}

void ArmScarletNx64::ClearInstructionCache() {
    m_process->ClearInstructionCache();
    SignalInterrupt(nullptr);
}

void ArmScarletNx64::InvalidateCacheRange(u64 addr, std::size_t size) {
    m_process->InvalidateCacheRange(addr, size);
    SignalInterrupt(nullptr);
}

void ArmScarletNx64::InvalidateCacheOnExecuteRevocation(u64 addr, std::size_t size) {
    InvalidateCacheRange(addr, size);
}

} // namespace Core
