// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <unordered_set>
#include <vector>

#include <elf.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>

#include "common/cpu_features.h"
#include "core/arm/scarletnx/scarletnx_api.h"
#include "core/arm/scarletnx/scarletnx_jit_budget.h"
#include "core/arm/scarletnx/scarletnx_process.h"

extern "C" {
#include "common/cpu/recompiler/llvm/fresh_image.h"
int __real_snx_fresh_image_load(const SnxLinkOutput* linked, SnxFreshImage* image);
int __wrap_snx_fresh_image_load(const SnxLinkOutput* linked, SnxFreshImage* image);
int __real_mprotect(void* address, size_t size, int protection);
int __wrap_mprotect(void* address, size_t size, int protection);
}

namespace {

enum class ImageFaultKind { InvalidDescriptor, DeniedExec };
struct ImageFault {
    ImageFaultKind kind;
    uintptr_t injected_at{};
};

thread_local ImageFault* active_image_fault{};
thread_local bool protect_fresh_image{};

std::string AnonymousMappingPermissions(uintptr_t address) {
    std::ifstream maps{"/proc/self/maps"};
    for (std::string line; std::getline(maps, line);) {
        unsigned long start, end, inode;
        char permissions[5]{};
        if (std::sscanf(line.c_str(), "%lx-%lx %4s %*s %*s %lu", &start, &end, permissions,
                        &inode) == 4 &&
            address >= start && address < end) {
            return inode == 0 ? permissions : "file-backed";
        }
    }
    return {};
}

} // namespace

int __wrap_mprotect(void* address, size_t size, int protection) {
    if (protect_fresh_image && protection == (PROT_READ | PROT_EXEC)) {
        active_image_fault->injected_at = reinterpret_cast<uintptr_t>(address);
        errno = EACCES;
        return -1;
    }
    return __real_mprotect(address, size, protection);
}

int __wrap_snx_fresh_image_load(const SnxLinkOutput* linked, SnxFreshImage* image) {
    if (!active_image_fault) {
        return __real_snx_fresh_image_load(linked, image);
    }
    if (active_image_fault->kind == ImageFaultKind::DeniedExec) {
        protect_fresh_image = true;
        const int result = __real_snx_fresh_image_load(linked, image);
        protect_fresh_image = false;
        return result;
    }
    // Replace the generated descriptor thunk with a pointer to a zero-ABI descriptor
    // inside the image's RW segment. Do not touch its segment or relocation metadata.
    const auto& layout = linked->fresh;
    constexpr size_t DescriptorBytes = 48;
    // ENDBR64; LEA RAX,[RIP+disp32]; RET
    std::array<u8, 12> thunk{0xf3, 0x0f, 0x1e, 0xfa, 0x48, 0x8d, 0x05, 0, 0, 0, 0, 0xc3};
    if (layout.rw_file_size < DescriptorBytes ||
        layout.rw_file_size - DescriptorBytes < layout.rw_reloc_size ||
        layout.descriptor < layout.rx_offset || layout.rx_size < thunk.size() ||
        layout.descriptor - layout.rx_offset > layout.rx_size - thunk.size()) {
        return __real_snx_fresh_image_load(linked, image);
    }
    const size_t descriptor = layout.rw_offset + layout.rw_file_size - DescriptorBytes;
    const auto relative = static_cast<int32_t>(descriptor - (layout.descriptor + 11));
    std::memcpy(thunk.data() + 7, &relative, sizeof(relative));
    std::memcpy(linked->data + layout.descriptor, thunk.data(), thunk.size());
    std::memset(linked->data + descriptor, 0, DescriptorBytes);
    const int result = __real_snx_fresh_image_load(linked, image);
    if (!result) {
        active_image_fault->injected_at = reinterpret_cast<uintptr_t>(image->base + layout.descriptor);
    }
    return result;
}

TEST_CASE("ScarletNX passes SVC arguments, guest CNTFRQ and confined fastmem", "[scarletnx]") {
    constexpr u64 Entry = 0x10000;
    constexpr u64 Stop = Entry + 0x1000;
    constexpr std::array<u32, 5> code{
        0xd53be001, // mrs x1, cntfrq_el0
        0xf9400002, // ldr x2, [x0]
        0xd40008e1, // svc #0x47
        0x91000400, // add x0, x0, #1
        0xd65f03c0, // ret
    };
    struct Call {
        u64 pc{};
        u64 address{};
        u64 frequency{};
        u64 loaded{};
        u32 number{};
        bool exception{};
    };

    REQUIRE(snx_recompiler_abi() == SNX_ABI);
    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 1);
    snx_recompiler_set_hook_direct_calls(compiler.get(), 1);
    REQUIRE(snx_recompiler_set_a64_target(compiler.get(), SNX_RECOMPILER_A64_MAX) == 0);
    const snx_code_range range{Entry, code.data(), sizeof(code)};
    std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)> module{
        snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, code.size(), nullptr, nullptr),
        &snx_recompiled_free};
    INFO(snx_recompiler_error(compiler.get()));
    REQUIRE(module);

    RtAPI runtime{};
    snx_runtime_defaults(&runtime);
    runtime.svc = +[](CPU* cpu, u32 number) {
        *static_cast<Call*>(cpu->thread) = {cpu->pc, cpu->x[0], cpu->x[1], cpu->x[2], number};
        cpu->x[0] = 41;
    };
    runtime.undef = +[](CPU* cpu, u64, u32) { static_cast<Call*>(cpu->thread)->exception = true; };
    runtime.brk = runtime.undef;

    // Production leaves offset zero unmapped. Here it holds a distinct sentinel so the test
    // can verify where an out-of-range address goes without intentionally faulting.
    std::array<u64, 2> memory{29, 42};
    struct Access {
        u64 address;
        size_t index;
    };
    constexpr std::array accesses{
        Access{8, 1},
        Access{0xabcd000000000008ULL, 1},
        Access{Core::ScarletNxProcess::ArenaSize + 8, 0},
        Access{Core::ScarletNxProcess::ArenaSize - 8, 0},
    };
    for (const auto [address, index] : accesses) {
        CAPTURE(address);
        Call call;
        CPU cpu{};
        cpu.mem = reinterpret_cast<u8*>(memory.data());
        cpu.rt = &runtime;
        cpu.thread = &call;
        cpu.cntfrq = Common::WallClock::Switch2CNTFRQ;
        cpu.x[0] = address;
        cpu.x[30] = Stop;
        REQUIRE(snx_recompiled_bind(module.get(), &cpu) == 0);
        snx_recompiled_call(module.get(), &cpu, Entry);
        CHECK_FALSE(call.exception);
        CHECK(call.number == 0x47);
        CHECK(call.pc == Entry + 12);
        CHECK(call.address == address);
        CHECK(call.frequency == Common::WallClock::Switch2CNTFRQ);
        CHECK(call.loaded == memory[index]);
        CHECK(cpu.x[0] == 42);
        CHECK(cpu.pc == Stop);
    }

    REQUIRE(snx_recompiled_function(module.get(), Entry) != nullptr);
    CHECK(snx_recompiled_invalidate(module.get(), Entry, sizeof(u32), nullptr, nullptr) != 0);
    CHECK(snx_recompiled_function(module.get(), Entry) == nullptr);
}

TEST_CASE("ScarletNX matches Dynarmic's PAuth hints and authenticated returns", "[scarletnx][pauth]") {
    constexpr u64 Entry = 0x10000;
    constexpr u64 Return = Entry + 0x200;
    constexpr u32 UndefinedWord = 0x00001234; // UDF #0x1234
    struct Variant {
        int target;
        u32 last;
        bool undefined = false;
    };
    constexpr std::array variants{
        Variant{SNX_RECOMPILER_A64_SWITCH, 0xd65f03c0}, // RET
        Variant{SNX_RECOMPILER_A64_MAX, 0xd65f0bff},    // RETAA
        Variant{SNX_RECOMPILER_A64_MAX, 0xd65f0fff},    // RETAB
        Variant{SNX_RECOMPILER_A64_SWITCH, 0xd65f0fff, true},
        Variant{SNX_RECOMPILER_A64_MAX, UndefinedWord, true},
    };
    struct Fault {
        u64 pc{};
        u32 word{};
    };

    RtAPI runtime{};
    snx_runtime_defaults(&runtime);
    runtime.svc = +[](CPU*, u32) {};
    runtime.undef = +[](CPU* cpu, u64 pc, u32 word) {
        *static_cast<Fault*>(cpu->thread) = {pc, word};
    };
    runtime.brk = runtime.undef;

    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 1);

    for (const auto [target, last, undefined] : variants) {
        CAPTURE(target, last);
        const std::array<u32, 5> code{
            0xd503237f, // PACIBSP
            0xaa1e03e0, // MOV X0, X30
            0xd50323ff, // AUTIBSP
            0xaa1e03e1, // MOV X1, X30
            last,
        };
        REQUIRE(snx_recompiler_set_a64_target(compiler.get(), target) == 0);
        const snx_code_range range{Entry, code.data(), sizeof(code)};
        std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)> module{
            snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, code.size(), nullptr, nullptr),
            &snx_recompiled_free};
        INFO(snx_recompiler_error(compiler.get()));
        REQUIRE(module);

        Fault fault;
        CPU cpu{};
        cpu.rt = &runtime;
        cpu.thread = &fault;
        cpu.x[30] = Return;
        cpu.x[31] = 0x7000;
        REQUIRE(snx_recompiled_bind(module.get(), &cpu) == 0);
        snx_recompiled_call(module.get(), &cpu, Entry);
        CHECK(cpu.x[0] == Return);
        CHECK(cpu.x[1] == Return);
        CHECK(cpu.x[30] == Return);
        CHECK(cpu.x[31] == 0x7000);
        if (undefined) {
            CHECK(fault.pc == Entry + 4 * (code.size() - 1));
            CHECK(fault.word == last);
            CHECK(cpu.pc == fault.pc);
        } else {
            CHECK(fault.pc == 0);
            CHECK(cpu.pc == Return);
        }
    }
}

TEST_CASE("ScarletNX keeps valid register PAuth operations as identities", "[scarletnx][pauth]") {
    constexpr u64 Entry = 0x10000;
    constexpr u64 Return = Entry + 0x200;
    constexpr u64 Pointer = 0xa5ff0000fedcba98;
    constexpr u32 PacibSp = 0xdac107fe; // PACIB X30, SP
    constexpr u32 RetX29 = 0xd65f03a0;
    constexpr std::array code{
        PacibSp,
        0xaa1e03e0u, // MOV X0, X30
        0xdac117feu, // AUTIB X30, SP
        0xaa1e03e1u, // MOV X1, X30
        0xdac127feu, // PACIZB X30
        0xdac137feu, // AUTIZB X30
        0xdac12bfeu, // PACDZA X30
        0xdac13bfeu, // AUTDZA X30
        0xaa1e03e2u, // MOV X2, X30
        0xdac143feu, // XPACI X30
        0xdac147feu, // XPACD X30
        RetX29,
    };
    struct Fault {
        u64 pc{};
        u32 word{};
    };
    RtAPI runtime{};
    snx_runtime_defaults(&runtime);
    runtime.svc = +[](CPU*, u32) {};
    runtime.undef = +[](CPU* cpu, u64 pc, u32 word) {
        *static_cast<Fault*>(cpu->thread) = {pc, word};
    };
    runtime.brk = runtime.undef;
    using Module = std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)>;
    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 1);

    CPU cpu{};
    Fault fault{};
    const auto run = [&](int target, std::span<const u32> words) {
        REQUIRE(snx_recompiler_set_a64_target(compiler.get(), target) == 0);
        const snx_code_range range{Entry, words.data(), words.size_bytes()};
        Module module{snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, words.size(),
                                         nullptr, nullptr),
                      &snx_recompiled_free};
        INFO(snx_recompiler_error(compiler.get()));
        REQUIRE(module);
        cpu = {};
        fault = {};
        cpu.rt = &runtime;
        cpu.thread = &fault;
        cpu.x[29] = Return;
        cpu.x[30] = Pointer;
        cpu.x[31] = 0x7000;
        set_nzcv(&cpu, 0xa);
        REQUIRE(snx_recompiled_bind(module.get(), &cpu) == 0);
        snx_recompiled_call(module.get(), &cpu, Entry);
    };

    run(SNX_RECOMPILER_A64_MAX, code);
    CHECK(fault.pc == 0);
    CHECK(cpu.pc == Return);
    CHECK(cpu.x[0] == Pointer);
    CHECK(cpu.x[1] == Pointer);
    CHECK(cpu.x[2] == Pointer);
    CHECK(cpu.x[30] == Pointer);
    CHECK(cpu.x[31] == 0x7000);
    CHECK(get_nzcv(&cpu) == 0xa0000000);

    const auto expect_undefined = [&](int target, u32 word) {
        CAPTURE(target, word);
        const std::array rejected{word, RetX29};
        run(target, rejected);
        CHECK(fault.pc == Entry);
        CHECK(fault.word == word);
        CHECK(cpu.pc == Entry);
    };
    expect_undefined(SNX_RECOMPILER_A64_SWITCH, PacibSp);
    expect_undefined(SNX_RECOMPILER_A64_MAX, 0xdac127de); // Z=1 with Rn != 31
    expect_undefined(SNX_RECOMPILER_A64_MAX, 0xdac143de); // XPACI with reserved Rn field
}

TEST_CASE("ScarletNX PACGA matches the Dynarmic identity model", "[scarletnx][pauth]") {
    constexpr u64 Entry = 0x10000;
    constexpr u64 Return = Entry + 0x200;
    constexpr u64 Pointer = 0xfedcba9876543210;
    constexpr u64 Modifier = 0x12345678a5a5a5a5;
    constexpr u64 Stack = 0x0f0f0f0fdeadbee0;
    constexpr u32 Observed = 0x9ad13262; // PACGA X2, X19, X17
    constexpr u32 Ret = 0xd65f03c0;
    constexpr std::array code{
        Observed,
        0x9adf3263u, // PACGA X3, X19, SP
        0x9ad133e4u, // PACGA X4, XZR, X17
        0x9adf327fu, // PACGA XZR, X19, SP
        Ret,
    };
    struct Fault {
        u64 pc{};
        u32 word{};
    };
    RtAPI runtime{};
    snx_runtime_defaults(&runtime);
    runtime.svc = +[](CPU*, u32) {};
    runtime.undef = +[](CPU* cpu, u64 pc, u32 word) {
        *static_cast<Fault*>(cpu->thread) = {pc, word};
    };
    runtime.brk = runtime.undef;
    using Module = std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)>;
    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 1);

    CPU cpu{};
    Fault fault{};
    const auto run = [&](int target, std::span<const u32> words) {
        REQUIRE(snx_recompiler_set_a64_target(compiler.get(), target) == 0);
        const snx_code_range range{Entry, words.data(), words.size_bytes()};
        Module module{snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, words.size(),
                                         nullptr, nullptr),
                      &snx_recompiled_free};
        INFO(snx_recompiler_error(compiler.get()));
        REQUIRE(module);
        cpu = {};
        fault = {};
        cpu.rt = &runtime;
        cpu.thread = &fault;
        cpu.x[17] = Modifier;
        cpu.x[19] = Pointer;
        cpu.x[30] = Return;
        cpu.x[31] = Stack;
        set_nzcv(&cpu, 0xa);
        REQUIRE(snx_recompiled_bind(module.get(), &cpu) == 0);
        snx_recompiled_call(module.get(), &cpu, Entry);
    };

    run(SNX_RECOMPILER_A64_MAX, code);
    CHECK(fault.pc == 0);
    CHECK(cpu.pc == Return);
    CHECK(cpu.x[2] == 0xece8ece000000000);
    CHECK(cpu.x[3] == 0xf1d3b59700000000);
    CHECK(cpu.x[4] == 0x1234567800000000);
    CHECK(cpu.x[17] == Modifier);
    CHECK(cpu.x[19] == Pointer);
    CHECK(cpu.x[31] == Stack);
    CHECK(get_nzcv(&cpu) == 0xa0000000);

    const auto expect_undefined = [&](int target, u32 word) {
        CAPTURE(target, word);
        const std::array rejected{word, Ret};
        run(target, rejected);
        CHECK(fault.pc == Entry);
        CHECK(fault.word == word);
        CHECK(cpu.pc == Entry);
    };
    expect_undefined(SNX_RECOMPILER_A64_SWITCH, Observed);
    expect_undefined(SNX_RECOMPILER_A64_MAX, 0x9ad13662); // Unallocated adjacent opcode
}

TEST_CASE("ScarletNX only retries data protection faults on cached backing", "[scarletnx][fastmem]") {
    using Core::ScarletNxFault;
    using Core::ScarletNxFaultAction;

    const ScarletNxFault read{
        .address = 0x21000,
        .host_pc = 0x10000,
        .signal = SIGSEGV,
        .signal_code = SEGV_ACCERR,
        .x86_trap = 14,
        .page_type = Common::PageType::RasterizerCachedMemory,
        .backing = 0x100000,
    };
    CHECK(read.SelectAction({}) == ScarletNxFaultAction::ReadCache);
    CHECK(read.SelectAction(read) == ScarletNxFaultAction::RetryAlreadyRequested);

    auto fault = read;
    fault.x86_error = 0x2;
    CHECK(fault.SelectAction(read) == ScarletNxFaultAction::InvalidateCache);
    CHECK(fault.SelectAction(fault) == ScarletNxFaultAction::RetryAlreadyRequested);

    fault = read;
    ++fault.host_pc;
    CHECK(fault.SelectAction(read) == ScarletNxFaultAction::ReadCache);

    fault = read;
    fault.page_type = Common::PageType::Memory;
    CHECK(fault.SelectAction({}) == ScarletNxFaultAction::NotCachedBacking);
    fault.page_type = Common::PageType::Unmapped;
    CHECK(fault.SelectAction({}) == ScarletNxFaultAction::NotCachedBacking);
    fault = read;
    fault.backing = 0;
    CHECK(fault.SelectAction({}) == ScarletNxFaultAction::NotCachedBacking);

    fault = read;
    fault.signal_code = SEGV_MAPERR;
    CHECK(fault.SelectAction({}) == ScarletNxFaultAction::NotDataProtectionFault);
    fault = read;
    fault.x86_error = 0x10;
    CHECK(fault.SelectAction({}) == ScarletNxFaultAction::NotDataProtectionFault);
    fault = read;
    fault.signal = SIGBUS;
    CHECK(fault.SelectAction({}) == ScarletNxFaultAction::NotDataProtectionFault);
}

TEST_CASE("ScarletNX requires fastmem without reactive flushing", "[scarletnx][cpu-backend]") {
    using Core::ScarletNxProcess;
    CHECK(ScarletNxProcess::MemoryModeFallback(false, false) == "requires fastmem");
    CHECK(ScarletNxProcess::MemoryModeFallback(false, true) == "requires fastmem");
    CHECK(ScarletNxProcess::MemoryModeFallback(true, true) == "requires Reactive Flushing disabled");
    CHECK(ScarletNxProcess::MemoryModeFallback(true, false).empty());
}

TEST_CASE("ScarletNX accepts only bounded decimal JIT budgets", "[scarletnx][jit-budget]") {
    using Core::ParseScarletNxJitBudget;
    CHECK(ParseScarletNxJitBudget(nullptr) == 2048);
    CHECK(ParseScarletNxJitBudget("2048") == 2048);
    CHECK(ParseScarletNxJitBudget("4096") == 4096);
    CHECK(ParseScarletNxJitBudget("8192") == 8192);
    for (const char* value : {"", "0", "2047", "8193", "99999999999999999999999", " 8192",
                              "8192 ", "+8192", "-8192", "8192x", "0x2000"}) {
        CAPTURE(value);
        CHECK_FALSE(ParseScarletNxJitBudget(value).has_value());
    }
}

TEST_CASE("ScarletNX batches an additional direct callee at a larger JIT budget",
          "[scarletnx][jit-budget]") {
    constexpr u64 Entry = 0x10000;
    constexpr size_t CallIndex = Core::ScarletNxDefaultJitBudget - 1;
    constexpr size_t CalleeIndex = CallIndex + 2;
    constexpr u64 Callee = Entry + CalleeIndex * sizeof(u32);
    std::vector<u32> code(CalleeIndex + 1, 0xd503201f);    // NOP
    code[CallIndex] = 0x94000002;                        // BL to Callee
    code[CallIndex + 1] = code[CalleeIndex] = 0xd65f03c0; // RET
    const snx_code_range range{Entry, code.data(), code.size() * sizeof(u32)};

    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 1);
    snx_recompiler_set_hook_direct_calls(compiler.get(), 1);
    REQUIRE(snx_recompiler_set_optimization(compiler.get(), '0') == 0);

    for (const auto budget : {Core::ScarletNxDefaultJitBudget, Core::ScarletNxMaxJitBudget}) {
        CAPTURE(budget);
        const std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)> module{
            snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, budget, nullptr, nullptr),
            &snx_recompiled_free};
        INFO(snx_recompiler_error(compiler.get()));
        REQUIRE(module);
        CHECK(snx_recompiled_function(module.get(), Entry) != nullptr);
        CHECK((snx_recompiled_function(module.get(), Callee) != nullptr) ==
              (budget == Core::ScarletNxMaxJitBudget));
    }
}

TEST_CASE("ScarletNX profiles cold compilation and linked cache reuse", "[scarletnx][profile]") {
    struct Counter {
        u64 calls{};
        u64 successes{};
        u64 ns{};
        u64 bytes{};
    };
    using Counters = std::array<Counter, SNX_PROFILE_STAGE_COUNT>;
    Counters counters{};
    const auto profile = +[](void* context, snx_profile_stage stage, uint64_t ns, uint64_t bytes,
                             int success) {
        auto& counter = (*static_cast<Counters*>(context))[stage];
        ++counter.calls;
        counter.successes += success != 0;
        counter.ns += ns;
        counter.bytes += bytes;
    };

    auto cache_path = (std::filesystem::temp_directory_path() / "scarletnx-profile-XXXXXX").string();
    const auto cleanup = [](const char* path) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    };
    const std::unique_ptr<const char, decltype(cleanup)> cache{mkdtemp(cache_path.data()), cleanup};
    REQUIRE(cache);
    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 1);
    REQUIRE(snx_recompiler_set_optimization(compiler.get(), '0') == 0);
    REQUIRE(snx_recompiler_set_cache(compiler.get(), cache.get()) == 0);
    snx_recompiler_set_profile(compiler.get(), profile, &counters);

    constexpr u64 Entry = 0x10000;
    constexpr std::array<u32, 2> code{0xd2800540, 0xd65f03c0}; // MOVZ X0, #42; RET
    const snx_code_range range{Entry, code.data(), sizeof(code)};
    const auto compile = [&] {
        const std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)> module{
            snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, code.size(), nullptr, nullptr),
            &snx_recompiled_free};
        INFO(snx_recompiler_error(compiler.get()));
        REQUIRE(module);
        RtAPI runtime{};
        snx_runtime_defaults(&runtime);
        runtime.svc = +[](CPU*, u32) {};
        runtime.undef = +[](CPU* cpu, u64, u32) { cpu->x[0] = 0; };
        runtime.brk = runtime.undef;
        CPU cpu{};
        cpu.rt = &runtime;
        cpu.x[30] = Entry + 0x1000;
        REQUIRE(snx_recompiled_bind(module.get(), &cpu) == 0);
        snx_recompiled_call(module.get(), &cpu, Entry);
        CHECK(cpu.x[0] == 42);
        CHECK(cpu.pc == cpu.x[30]);
    };

    compile();
    for (const auto stage : {SNX_PROFILE_DISCOVERY, SNX_PROFILE_LLVM_INIT, SNX_PROFILE_LLVM_UNITS,
                             SNX_PROFILE_LLVM_METADATA, SNX_PROFILE_RTLIB, SNX_PROFILE_LINK,
                             SNX_PROFILE_IMAGE_LOAD, SNX_PROFILE_IMAGE_WRITE, SNX_PROFILE_DLOPEN,
                             SNX_PROFILE_CACHE_WRITE}) {
        CAPTURE(stage);
        CHECK(counters[stage].calls == 1);
        CHECK(counters[stage].successes == 1);
    }
    CHECK(counters[SNX_PROFILE_CACHE_LOOKUP].calls == 1);
    CHECK(counters[SNX_PROFILE_CACHE_LOOKUP].successes == 0);
    CHECK(counters[SNX_PROFILE_CACHE_READ].calls == 1);
    CHECK(counters[SNX_PROFILE_CACHE_READ].bytes == 0);
    CHECK(counters[SNX_PROFILE_CACHE_IMAGE_LOAD].calls == 0);
    const auto module_bytes = counters[SNX_PROFILE_CACHE_WRITE].bytes;
    REQUIRE(module_bytes > 0);
    CHECK(counters[SNX_PROFILE_IMAGE_WRITE].bytes == module_bytes);

    counters = {};
    compile();
    for (const auto stage : {SNX_PROFILE_DISCOVERY, SNX_PROFILE_LLVM_INIT, SNX_PROFILE_CACHE_LOOKUP,
                             SNX_PROFILE_CACHE_READ, SNX_PROFILE_CACHE_IMAGE_LOAD,
                             SNX_PROFILE_IMAGE_WRITE, SNX_PROFILE_DLOPEN}) {
        CAPTURE(stage);
        CHECK(counters[stage].calls == 1);
        CHECK(counters[stage].successes == 1);
    }
    for (const auto stage : {SNX_PROFILE_LLVM_UNITS, SNX_PROFILE_LLVM_METADATA, SNX_PROFILE_RTLIB,
                             SNX_PROFILE_LINK, SNX_PROFILE_IMAGE_LOAD, SNX_PROFILE_CACHE_WRITE}) {
        CAPTURE(stage);
        CHECK(counters[stage].calls == 0);
    }
    CHECK(counters[SNX_PROFILE_CACHE_READ].bytes == module_bytes);
    CHECK(counters[SNX_PROFILE_IMAGE_WRITE].bytes == module_bytes);
    CHECK(counters[SNX_PROFILE_CACHE_LOOKUP].ns >=
          counters[SNX_PROFILE_CACHE_READ].ns + counters[SNX_PROFILE_CACHE_IMAGE_LOAD].ns);
    CHECK(counters[SNX_PROFILE_CACHE_IMAGE_LOAD].ns >=
          counters[SNX_PROFILE_IMAGE_WRITE].ns + counters[SNX_PROFILE_DLOPEN].ns);

    counters = {};
    snx_recompiler_set_profile(compiler.get(), nullptr, nullptr);
    compile();
    for (const auto& counter : counters) {
        CHECK(counter.calls == 0);
    }
}

TEST_CASE("ScarletNX serial target reuse preserves linked bytes and module lifetime",
          "[scarletnx][llvm-target]") {
    auto temp_path = (std::filesystem::temp_directory_path() / "scarletnx-target-XXXXXX").string();
    const auto cleanup = [](const char* path) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    };
    const std::unique_ptr<const char, decltype(cleanup)> temp{mkdtemp(temp_path.data()), cleanup};
    REQUIRE(temp);
    using StageCounts = std::array<unsigned, SNX_PROFILE_STAGE_COUNT>;
    using Module = std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)>;
    constexpr u64 Entry = 0x10000;
    constexpr u64 Return = Entry + 0x1000;
    const auto run = [&](bool reuse) {
        CAPTURE(reuse);
        const auto cache = std::filesystem::path{temp.get()} / (reuse ? "reuse" : "default");
        REQUIRE(std::filesystem::create_directory(cache));
        std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
            snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
        REQUIRE(compiler);
        snx_recompiler_set_jobs(compiler.get(), 2);
        snx_recompiler_set_hook_direct_calls(compiler.get(), 1);
        snx_recompiler_set_inprocess_delta_loader(compiler.get(), 1);
        snx_recompiler_set_reuse_delta_target(compiler.get(), reuse);
        REQUIRE(snx_recompiler_set_cache(compiler.get(), cache.c_str()) == 0);
        StageCounts successful{};
        snx_recompiler_set_profile(
            compiler.get(),
            +[](void* context, snx_profile_stage stage, uint64_t, uint64_t, int ok) {
                (*static_cast<StageCounts*>(context))[stage] += ok != 0;
            },
            &successful);

        std::vector<Module> modules;
        const auto compile = [&](u32 increment, char opt) {
            const bool max_target = increment % 2 != 0;
            REQUIRE(snx_recompiler_set_optimization(compiler.get(), opt) == 0);
            REQUIRE(snx_recompiler_set_a64_target(
                        compiler.get(), max_target ? SNX_RECOMPILER_A64_MAX : SNX_RECOMPILER_A64_SWITCH) == 0);
            const std::array<u32, 3> code{
                max_target ? 0xdac107feu : 0xd503201fu, // PACIB X30, SP or NOP
                0x91000000 | (increment << 10),        // ADD X0, X0, #increment
                0xd65f03c0,                           // RET
            };
            const snx_code_range range{Entry, code.data(), sizeof(code)};
            Module module{snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, code.size(),
                                             nullptr, nullptr),
                          &snx_recompiled_free};
            INFO(snx_recompiler_error(compiler.get()));
            REQUIRE(module);
            return module;
        };
        for (u32 i = 1; i <= 4; ++i) {
            modules.push_back(compile(i, '0'));
        }
        CHECK(successful[SNX_PROFILE_LLVM_NEW_TARGET_IR] == (reuse ? 1 : 8));
        CHECK(successful[SNX_PROFILE_LLVM_REUSED_TARGET_IR] == (reuse ? 7 : 0));
        // The demonstrated cache hit takes the same path without constructing any IR.
        modules.push_back(compile(4, '0'));
        CHECK(successful[SNX_PROFILE_CACHE_LOOKUP] == 1);
        CHECK(successful[SNX_PROFILE_LLVM_NEW_TARGET_IR] == (reuse ? 1 : 8));
        modules.push_back(compile(5, '1'));
        modules.push_back(compile(6, '0'));
        CHECK(successful[SNX_PROFILE_LLVM_NEW_TARGET_IR] == (reuse ? 4 : 12));
        CHECK(successful[SNX_PROFILE_LLVM_REUSED_TARGET_IR] == (reuse ? 8 : 0));
        snx_recompiler_set_reuse_delta_target(compiler.get(), 0);
        modules.push_back(compile(7, '0'));
        CHECK(successful[SNX_PROFILE_LLVM_NEW_TARGET_IR] == (reuse ? 6 : 14));
        CHECK(successful[SNX_PROFILE_INPROCESS_LOAD] == 7);
        compiler.reset();

        RtAPI runtime{};
        snx_runtime_defaults(&runtime);
        runtime.svc = +[](CPU*, u32) {};
        runtime.undef = +[](CPU* cpu, u64, u32) { cpu->x[0] = 0; };
        runtime.brk = runtime.undef;
        constexpr std::array<u64, 8> increments{1, 2, 3, 4, 4, 5, 6, 7};
        for (size_t i = 0; i < modules.size(); ++i) {
            CPU cpu{};
            cpu.rt = &runtime;
            cpu.x[0] = 100;
            cpu.x[30] = Return;
            REQUIRE(snx_recompiled_bind(modules[i].get(), &cpu) == 0);
            snx_recompiled_call(modules[i].get(), &cpu, Entry);
            CHECK(cpu.x[0] == 100 + increments[i]);
            CHECK(cpu.pc == Return);
        }

        std::map<std::string, std::string> images;
        for (const auto& file : std::filesystem::directory_iterator{cache}) {
            if (file.path().extension() == ".so") {
                std::ifstream bytes{file.path(), std::ios::binary};
                REQUIRE(bytes.good());
                const std::string data{std::istreambuf_iterator<char>{bytes}, {}};
                REQUIRE_FALSE(data.empty());
                images.emplace(file.path().filename().string(), data);
            }
        }
        REQUIRE(images.size() == 7);
        return images;
    };
    const auto control = run(false);
    const auto treatment = run(true);
    for (const auto& [name, bytes] : control) {
        CAPTURE(name);
        REQUIRE(treatment.contains(name));
        CHECK((treatment.at(name) == bytes));
    }
}

TEST_CASE("ScarletNX target reuse does not borrow a machine for parallel units",
          "[scarletnx][llvm-target]") {
    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 2);
    snx_recompiler_set_reuse_delta_target(compiler.get(), 1);
    REQUIRE(snx_recompiler_set_optimization(compiler.get(), '0') == 0);
    std::array<unsigned, SNX_PROFILE_STAGE_COUNT> calls{};
    snx_recompiler_set_profile(
        compiler.get(),
        +[](void* context, snx_profile_stage stage, uint64_t, uint64_t, int) {
            ++(*static_cast<decltype(calls)*>(context))[stage];
        },
        &calls);
    constexpr u64 Entry = 0x10000;
    constexpr size_t FunctionSize = 2000;
    // A 4,000-instruction chunk holds the first two functions, leaving a second unit.
    constexpr std::array<u64, 3> hints{
        Entry, Entry + FunctionSize * sizeof(u32), Entry + 2 * FunctionSize * sizeof(u32)};
    std::vector<u32> code(hints.size() * FunctionSize, 0xd503201f);
    for (size_t i = FunctionSize; i <= code.size(); i += FunctionSize) {
        code[i - 1] = 0xd65f03c0;
    }
    const snx_code_range range{Entry, code.data(), code.size() * sizeof(u32)};
    const std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)> module{
        snx_recompile_from(compiler.get(), &range, 1, hints.data(), hints.size(), code.size(), nullptr,
                           nullptr),
        &snx_recompiled_free};
    INFO(snx_recompiler_error(compiler.get()));
    REQUIRE(module);
    for (const auto hint : hints) {
        CHECK(snx_recompiled_function(module.get(), hint) != nullptr);
    }
    CHECK(calls[SNX_PROFILE_LLVM_UNITS] == 1);
    CHECK(calls[SNX_PROFILE_LLVM_NEW_TARGET_IR] == 0);
    CHECK(calls[SNX_PROFILE_LLVM_REUSED_TARGET_IR] == 0);
}

TEST_CASE("ScarletNX fresh image validates relocations and finalizes distinct R/RX/RW mappings",
          "[scarletnx][inprocess]") {
    constexpr size_t Page = 4096;
    constexpr size_t Reloc = 8;
    std::array<unsigned char, 2 * Page + 16> bytes{};
    SnxLinkOutput linked{
        .data = bytes.data(),
        .size = bytes.size(),
        .fresh = {
            .r_size = 64,
            .rx_offset = Page,
            .rx_size = 16,
            .rw_offset = 2 * Page,
            .rw_file_size = 16,
            .rw_mem_size = Page + 16,
            .rw_reloc_size = 16,
            .rela_offset = Reloc,
            .rela_count = 1,
            .descriptor = Page,
        },
    };
    const Elf64_Rela relative{2 * Page, ELF64_R_INFO(0, R_X86_64_RELATIVE), Page};
    const auto write = [&](const Elf64_Rela& rela) {
        std::memcpy(bytes.data() + Reloc, &rela, sizeof(rela));
    };
    struct ResidentImage {
        SnxFreshImage value{};
        ~ResidentImage() {
            snx_fresh_image_unload(&value);
        }
    };
    ResidentImage trial;
    for (const Elf64_Rela invalid : {
             Elf64_Rela{2 * Page, ELF64_R_INFO(1, R_X86_64_RELATIVE), Page},
             Elf64_Rela{2 * Page, ELF64_R_INFO(0, R_X86_64_64), Page},
             Elf64_Rela{Page, relative.r_info, Page},
             Elf64_Rela{2 * Page + 1, relative.r_info, Page},
             Elf64_Rela{2 * Page + 16, relative.r_info, Page},
             Elf64_Rela{2 * Page, relative.r_info, -1},
             Elf64_Rela{2 * Page, relative.r_info, Page + 16},
         }) {
        write(invalid);
        CHECK(snx_fresh_image_load(&linked, &trial.value) == -1);
        CHECK(trial.value.base == nullptr);
        CHECK(trial.value.size == 0);
    }
    write(relative);
    auto bad = linked;
    bad.fresh.rx_offset = 0;
    CHECK(snx_fresh_image_load(&bad, &trial.value) == -1);
    bad = linked;
    bad.fresh.rw_mem_size = SIZE_MAX;
    CHECK(snx_fresh_image_load(&bad, &trial.value) == -1);
    bad = linked;
    bad.fresh.rela_count = SIZE_MAX;
    CHECK(snx_fresh_image_load(&bad, &trial.value) == -1);
    bad = linked;
    bad.fresh.descriptor = 2 * Page;
    CHECK(snx_fresh_image_load(&bad, &trial.value) == -1);

    // Retain the mappings together, without invoking LLVM or loading files.
    std::array<ResidentImage, 128> residents;
    std::unordered_set<uintptr_t> bases;
    for (auto& [image] : residents) {
        REQUIRE(snx_fresh_image_load(&linked, &image) == 0);
        CHECK(bases.insert(reinterpret_cast<uintptr_t>(image.base)).second);
    }
    auto& image = residents.front().value;
    const auto base = reinterpret_cast<uintptr_t>(image.base);
    CHECK(AnonymousMappingPermissions(base) == "r--p");
    CHECK(AnonymousMappingPermissions(base + Page) == "r-xp");
    CHECK(AnonymousMappingPermissions(base + 2 * Page) == "rw-p");
    CHECK(snx_fresh_image_load(&linked, &image) == -1);
    uintptr_t relocated{};
    std::memcpy(&relocated, image.base + 2 * Page, sizeof(relocated));
    CHECK(relocated == base + Page);
    CHECK(image.base[3 * Page + 15] == 0);
    snx_fresh_image_unload(&image);
    CHECK(AnonymousMappingPermissions(base + Page).empty());
}

TEST_CASE("ScarletNX fresh deltas match cached dlopen dispatch and outlive retirement",
          "[scarletnx][inprocess]") {
    auto cache_path = (std::filesystem::temp_directory_path() / "scarletnx-loader-XXXXXX").string();
    const auto cleanup = [](const char* path) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    };
    const std::unique_ptr<const char, decltype(cleanup)> cache{mkdtemp(cache_path.data()), cleanup};
    REQUIRE(cache);
    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 1);
    snx_recompiler_set_hook_direct_calls(compiler.get(), 1);
    snx_recompiler_set_inprocess_delta_loader(compiler.get(), 1);
    REQUIRE(snx_recompiler_set_optimization(compiler.get(), '0') == 0);
    REQUIRE(snx_recompiler_set_cache(compiler.get(), cache.get()) == 0);

    using StageCounts = std::array<unsigned, SNX_PROFILE_STAGE_COUNT>;
    StageCounts calls{}, successes{};
    std::array counts{&calls, &successes};
    snx_recompiler_set_profile(
        compiler.get(),
        +[](void* context, snx_profile_stage stage, uint64_t, uint64_t, int success) {
            const auto& count = *static_cast<const std::array<StageCounts*, 2>*>(context);
            ++(*count[0])[stage];
            (*count[1])[stage] += success != 0;
        },
        &counts);

    constexpr u64 Entry = 0x10000;
    constexpr u64 Return = Entry + 0x1000;
    using Module = std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)>;
    const auto compile = [&](u32 value) {
        const std::array<u32, 2> code{0xd2800000 | (value << 5), 0xd65f03c0}; // MOVZ; RET
        const snx_code_range range{Entry, code.data(), sizeof(code)};
        Module module{snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, code.size(), nullptr,
                                         nullptr),
                      &snx_recompiled_free};
        INFO(snx_recompiler_error(compiler.get()));
        REQUIRE(module);
        return module;
    };
    auto first = compile(41);
    const auto second = compile(42);
    REQUIRE(calls[SNX_PROFILE_INPROCESS_LOAD] == 2);
    REQUIRE(successes[SNX_PROFILE_INPROCESS_LOAD] == 2);
    CHECK(successes[SNX_PROFILE_INPROCESS_MAP] == 2);
    CHECK(successes[SNX_PROFILE_INPROCESS_DESCRIPTOR] == 2);
    CHECK(calls[SNX_PROFILE_DLOPEN] == 0);
    CHECK(successes[SNX_PROFILE_CACHE_WRITE] == 2);
    const auto cached = compile(41);
    CHECK(calls[SNX_PROFILE_INPROCESS_LOAD] == 2);
    CHECK(successes[SNX_PROFILE_CACHE_LOOKUP] == 1);
    CHECK(successes[SNX_PROFILE_CACHE_IMAGE_LOAD] == 1);
    CHECK(successes[SNX_PROFILE_DLOPEN] == 1);

    const auto entry = [&](const Module& module) { return snx_recompiled_function(module.get(), Entry); };
    REQUIRE(entry(first) != nullptr);
    CHECK(entry(first) != entry(second));
    CHECK(entry(first) != entry(cached));
    const auto first_code = reinterpret_cast<uintptr_t>(entry(first));
    CHECK(AnonymousMappingPermissions(first_code) == "r-xp");
    CHECK(AnonymousMappingPermissions(reinterpret_cast<uintptr_t>(entry(cached))) == "file-backed");

    RtAPI runtime{};
    snx_runtime_defaults(&runtime);
    runtime.svc = +[](CPU*, u32) {};
    runtime.undef = +[](CPU* cpu, u64, u32) { cpu->x[0] = 0; };
    runtime.brk = runtime.undef;
    const auto check_value = [&](const Module& module, u64 expected) {
        u64 lo{}, hi{};
        snx_recompiled_range(module.get(), &lo, &hi);
        CHECK(lo == Entry);
        CHECK(hi == Entry + 2 * sizeof(u32));
        CPU cpu{};
        cpu.rt = &runtime;
        cpu.x[30] = Return;
        REQUIRE(snx_recompiled_bind(module.get(), &cpu) == 0);
        snx_recompiled_call(module.get(), &cpu, Entry);
        CHECK(cpu.x[0] == expected);
        CHECK(cpu.pc == Return);
    };
    check_value(first, 41);
    check_value(second, 42);
    check_value(cached, 41);
    CHECK(snx_recompiled_invalidate(first.get(), Entry, sizeof(u32), nullptr, nullptr) == 1);
    CHECK(entry(first) == nullptr);
    CHECK(AnonymousMappingPermissions(first_code) == "r-xp");
    check_value(second, 42);
    check_value(cached, 41);
    first.reset();
    CHECK(AnonymousMappingPermissions(first_code).empty());
}

TEST_CASE("ScarletNX fresh rejections never reach dlopen, dispatch or disk",
          "[scarletnx][inprocess]") {
    auto cache_path = (std::filesystem::temp_directory_path() / "scarletnx-reject-XXXXXX").string();
    const auto cleanup = [](const char* path) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    };
    const std::unique_ptr<const char, decltype(cleanup)> cache{mkdtemp(cache_path.data()), cleanup};
    REQUIRE(cache);
    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 1);
    snx_recompiler_set_inprocess_delta_loader(compiler.get(), 1);
    REQUIRE(snx_recompiler_set_cache(compiler.get(), cache.get()) == 0);

    using Outcomes = std::array<int, SNX_PROFILE_STAGE_COUNT>;
    Outcomes outcomes{};
    snx_recompiler_set_profile(
        compiler.get(),
        +[](void* context, snx_profile_stage stage, uint64_t, uint64_t, int ok) {
            (*static_cast<Outcomes*>(context))[stage] = ok ? 1 : -1;
        },
        &outcomes);

    constexpr u64 Entry = 0x10000;
    constexpr u32 Ret = 0xd65f03c0;
    const snx_code_range range{Entry, &Ret, sizeof(Ret)};
    for (const auto kind : {ImageFaultKind::InvalidDescriptor, ImageFaultKind::DeniedExec}) {
        ImageFault fault{kind};
        outcomes = {};
        active_image_fault = &fault;
        std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)> module{
            snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, 1, nullptr, nullptr),
            &snx_recompiled_free};
        active_image_fault = nullptr;
        const auto error = std::string_view{snx_recompiler_error(compiler.get())};
        const bool bad_descriptor = kind == ImageFaultKind::InvalidDescriptor;
        REQUIRE(fault.injected_at != 0);
        CHECK_FALSE(module);
        CHECK(error.find(bad_descriptor ? "invalid descriptor" : "mapping or protection") !=
              std::string_view::npos);
        CHECK(outcomes[SNX_PROFILE_INPROCESS_LOAD] == -1);
        CHECK(outcomes[SNX_PROFILE_INPROCESS_MAP] == (bad_descriptor ? 1 : -1));
        CHECK(outcomes[SNX_PROFILE_INPROCESS_DESCRIPTOR] == (bad_descriptor ? -1 : 0));
        CHECK(outcomes[SNX_PROFILE_DLOPEN] == 0);
        CHECK(outcomes[SNX_PROFILE_CACHE_WRITE] == 0);
        CHECK(AnonymousMappingPermissions(fault.injected_at).empty());
        for (const auto& file : std::filesystem::directory_iterator{cache.get()}) {
            CHECK(file.path().extension() == ".o"); // Only the independent LLVM runtime object.
        }
    }
}

TEST_CASE("ScarletNX rejects even a known cache hit without a verified W^X policy",
          "[scarletnx][inprocess]") {
    auto cache_path = (std::filesystem::temp_directory_path() / "scarletnx-policy-XXXXXX").string();
    const auto cleanup = [](const char* path) {
        std::error_code error;
        std::filesystem::remove_all(path, error);
    };
    const std::unique_ptr<const char, decltype(cleanup)> cache{mkdtemp(cache_path.data()), cleanup};
    REQUIRE(cache);
    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 1);
    REQUIRE(snx_recompiler_set_cache(compiler.get(), cache.get()) == 0);
    constexpr u64 Entry = 0x10000;
    constexpr u32 Ret = 0xd65f03c0;
    const snx_code_range range{Entry, &Ret, sizeof(Ret)};
    const auto compile = [&] {
        return std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)>{
            snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, 1, nullptr, nullptr),
            &snx_recompiled_free};
    };
    REQUIRE(compile());
    using Outcomes = std::array<int, SNX_PROFILE_STAGE_COUNT>;
    Outcomes outcomes{};
    snx_recompiler_set_profile(
        compiler.get(),
        +[](void* context, snx_profile_stage stage, uint64_t, uint64_t, int ok) {
            (*static_cast<Outcomes*>(context))[stage] = ok ? 1 : -1;
        },
        &outcomes);
    snx_recompiler_set_inprocess_delta_loader(compiler.get(), 1);
    REQUIRE(compile());
    CHECK(outcomes[SNX_PROFILE_CACHE_LOOKUP] == 1);
    CHECK(outcomes[SNX_PROFILE_DLOPEN] == 1);

    const pid_t child = fork();
    REQUIRE(child >= 0);
    if (child == 0) {
        // Deny the fresh loader's personality query in this child only.
        sock_filter filter[]{
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, SYS_personality, 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
        };
        sock_fprog program{static_cast<unsigned short>(std::size(filter)), filter};
        if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) || prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program)) {
            _exit(77);
        }
        outcomes = {};
        auto module = compile();
        const auto error = std::string_view{snx_recompiler_error(compiler.get())};
        const bool ok = !module && error.find("verified W^X personality") != std::string_view::npos &&
                        outcomes[SNX_PROFILE_CACHE_LOOKUP] == 0 &&
                        outcomes[SNX_PROFILE_DLOPEN] == 0 &&
                        outcomes[SNX_PROFILE_INPROCESS_LOAD] == 0 &&
                        outcomes[SNX_PROFILE_CACHE_WRITE] == 0;
        module.reset();
        compiler.reset();
        _exit(ok ? 0 : 2);
    }
    int status{};
    REQUIRE(waitpid(child, &status, 0) == child);
    REQUIRE(WIFEXITED(status));
    if (WEXITSTATUS(status) == 77) {
        SKIP("seccomp test filter unavailable");
    }
    CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("ScarletNX JIT deltas keep distinct code without retaining descriptors", "[scarletnx][fd]") {
    constexpr u64 Entry = 0x10000;
    constexpr u64 Return = Entry + 0x1000;
    constexpr u32 ModuleCount = 64;
    using Module = std::unique_ptr<snx_recompiled, decltype(&snx_recompiled_free)>;
    const auto count_fds = [] {
        return std::distance(std::filesystem::directory_iterator{"/proc/self/fd"},
                             std::filesystem::directory_iterator{});
    };

    std::unique_ptr<snx_recompiler, decltype(&snx_recompiler_free)> compiler{
        snx_recompiler_new(SNX_RECOMPILER_FLAG_LLVM), &snx_recompiler_free};
    REQUIRE(compiler);
    snx_recompiler_set_jobs(compiler.get(), 1);

    const auto initial_fds = count_fds();
    std::vector<Module> modules;
    modules.reserve(ModuleCount);
    for (u32 value = 1; value <= ModuleCount; ++value) {
        CAPTURE(value);
        const std::array<u32, 2> code{
            0xd2800000 | (value << 5), // MOVZ X0, #value
            0xd65f03c0,                // RET
        };
        const snx_code_range range{Entry, code.data(), sizeof(code)};
        auto& module = modules.emplace_back(
            snx_recompile_from(compiler.get(), &range, 1, &Entry, 1, code.size(), nullptr, nullptr),
            &snx_recompiled_free);
        INFO(snx_recompiler_error(compiler.get()));
        REQUIRE(module);
    }
    CHECK(count_fds() <= initial_fds + 2);

    RtAPI runtime{};
    snx_runtime_defaults(&runtime);
    runtime.svc = +[](CPU*, u32) {};
    runtime.undef = +[](CPU* cpu, u64, u32) { cpu->x[0] = 0; };
    runtime.brk = runtime.undef;
    for (u32 i = 0; i < ModuleCount; ++i) {
        CAPTURE(i);
        CPU cpu{};
        cpu.rt = &runtime;
        cpu.x[30] = Return;
        REQUIRE(snx_recompiled_bind(modules[i].get(), &cpu) == 0);
        snx_recompiled_call(modules[i].get(), &cpu, Entry);
        CHECK(cpu.x[0] == i + 1);
        CHECK(cpu.pc == Return);
    }
}
