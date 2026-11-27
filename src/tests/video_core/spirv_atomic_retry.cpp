// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

std::vector<std::vector<u32>> Instructions(const std::vector<u32>& code, spv::Op opcode) {
    std::vector<std::vector<u32>> result;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        if ((code[offset] & 0xffffU) == static_cast<u32>(opcode)) {
            const auto words = std::span{code}.subspan(offset + 1, size - 1);
            result.emplace_back(words.begin(), words.end());
        }
        offset += size;
    }
    return result;
}

std::vector<u32> EmitAtomicHelpers(bool native_half, bool explicit_shared) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {256, 1, 1};
    program.shared_memory_size = 16;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    auto& info = program.info;
    info.storage_buffers_descriptors = {{0, 0, 1, true}, {0, 16, 1, true}};
    IR::IREmitter ir{block};
    block.AppendNewInst(IR::Opcode::WriteStorageU16, {ir.Imm32(0), ir.Imm32(2), ir.Imm32(12)});
    block.AppendNewInst(IR::Opcode::WriteStorageS8, {ir.Imm32(1), ir.Imm32(3), ir.Imm32(45)});
    block.AppendNewInst(IR::Opcode::StorageAtomicInc32,
                        {ir.Imm32(0), ir.Imm32(4), ir.Imm32(9)});
    block.AppendNewInst(IR::Opcode::StorageAtomicInc32,
                        {ir.Imm32(1), ir.Imm32(4), ir.Imm32(9)});
    block.AppendNewInst(IR::Opcode::StorageAtomicInc32,
                        {ir.Imm32(1), ir.Imm32(8), ir.Imm32(9)});
    block.AppendNewInst(IR::Opcode::StorageAtomicDec32,
                        {ir.Imm32(1), ir.Imm32(4), ir.Imm32(9)});
    block.AppendNewInst(IR::Opcode::StorageAtomicAddF32,
                        {ir.Imm32(0), ir.Imm32(8), ir.Imm32(1.0f)});
    const auto a = ir.Imm32(1.0f);
    const auto b = ir.Imm32(2.0f);
    const auto pair = native_half
                          ? ir.CompositeConstruct(ir.FPConvert(16, a), ir.FPConvert(16, b))
                          : ir.CompositeConstruct(a, b);
    const std::array vector_ops =
        native_half ? std::array{IR::Opcode::StorageAtomicAddF16x2,
                                 IR::Opcode::StorageAtomicMinF16x2,
                                 IR::Opcode::StorageAtomicMaxF16x2}
                    : std::array{IR::Opcode::StorageAtomicAddF32x2,
                                 IR::Opcode::StorageAtomicMinF32x2,
                                 IR::Opcode::StorageAtomicMaxF32x2};
    for (size_t i = 0; i < vector_ops.size(); ++i) {
        block.AppendNewInst(vector_ops[i], {ir.Imm32(1), ir.Imm32(12 + static_cast<u32>(i) * 4),
                                            pair});
    }
    info.used_storage_buffer_types = IR::Type::U32;
    info.uses_shared_increment = true;
    info.uses_shared_decrement = true;
    info.uses_fp16 = native_half;
    info.uses_int8 = true;
    info.uses_int16 = true;
    Profile profile;
    profile.supported_spirv = 0x00010500;
    profile.support_explicit_workgroup_layout = explicit_shared;
    profile.support_workgroup_layout_8bit_access = explicit_shared;
    profile.support_workgroup_layout_16bit_access = explicit_shared;
    auto code = Backend::SPIRV::EmitSPIRV(profile, program);
    if (const char* directory = std::getenv("EDEN_ATOMIC_TEST_OUTPUT")) {
        std::filesystem::create_directories(directory);
        const auto name = std::string{native_half ? "native-half" : "packed-half"} +
                          (explicit_shared ? "-explicit" : "-implicit") + ".spv";
        std::ofstream file{std::filesystem::path{directory} / name, std::ios::binary};
        file.write(reinterpret_cast<const char*>(code.data()),
                   static_cast<std::streamsize>(code.size() * sizeof(u32)));
        REQUIRE(file.good());
    }
    return code;
}
}

TEST_CASE("SPIR-V CAS retries use the exchange result as the next expected word",
          "[video_core][spirv][atomic_retry]") {
    for (const bool native_half : {true, false}) {
        for (const bool explicit_shared : {false, true}) {
            CAPTURE(native_half, explicit_shared);
            const auto code = EmitAtomicHelpers(native_half, explicit_shared);
            const auto exchanges = Instructions(code, spv::Op::OpAtomicCompareExchange);
            const auto loads = Instructions(code, spv::Op::OpLoad);
            const auto atomic_loads = Instructions(code, spv::Op::OpAtomicLoad);
            const auto stores = Instructions(code, spv::Op::OpStore);
            const auto variables = Instructions(code, spv::Op::OpVariable);
            const auto integers = Instructions(code, spv::Op::OpTypeInt);
            const auto comparisons = Instructions(code, spv::Op::OpIEqual);
            const auto return_values = Instructions(code, spv::Op::OpReturnValue);
            const auto access_chains = Instructions(code, spv::Op::OpAccessChain);
            const auto capabilities = Instructions(code, spv::Op::OpCapability);
            const auto pointers = Instructions(code, spv::Op::OpTypePointer);
            const auto parameters = Instructions(code, spv::Op::OpFunctionParameter);
            CHECK(std::ranges::none_of(capabilities, [](const auto& cap) {
                return cap[0] == static_cast<u32>(spv::Capability::VariablePointersStorageBuffer) ||
                       cap[0] == static_cast<u32>(spv::Capability::VariablePointers);
            }));
            for (const auto& parameter : parameters) {
                CHECK(std::ranges::none_of(pointers, [&](const auto& pointer) {
                    return pointer[0] == parameter[0];
                }));
            }
            // Seven storage atomic loops, two storage subword loops and two or four shared loops.
            REQUIRE(exchanges.size() == (explicit_shared ? 11u : 13u));
            size_t integer_storage_returns{};
            for (const auto& exchange : exchanges) {
                REQUIRE(exchange.size() == 8);
                const u32 type = exchange[0];
                const u32 result = exchange[1];
                const u32 memory = exchange[2];
                const u32 expected = exchange[7];
                CHECK(std::ranges::find(integers, std::vector<u32>{type, 32, 0}) != integers.end());
                const auto expected_load = std::ranges::find_if(
                    loads, [=](const auto& load) { return load[1] == expected; });
                REQUIRE(expected_load != loads.end());
                CHECK((*expected_load)[0] == type);
                const u32 retry = (*expected_load)[2];
                CHECK(retry != memory);
                const auto variable = std::ranges::find_if(
                    variables, [=](const auto& var) { return var[1] == retry; });
                REQUIRE(variable != variables.end());
                CHECK((*variable)[2] == static_cast<u32>(spv::StorageClass::Function));
                REQUIRE(std::ranges::count_if(stores, [=](const auto& store) {
                    return store[0] == retry;
                }) == 2);
                CHECK(std::ranges::find(stores, std::vector<u32>{retry, result}) != stores.end());
                const auto initial = std::ranges::find_if(
                    atomic_loads, [=](const auto& load) { return load[2] == memory; });
                REQUIRE(initial != atomic_loads.end());
                CHECK((*initial)[3] == exchange[3]);
                CHECK((*initial)[4] == exchange[4]);
                CHECK((*initial)[4] == exchange[5]);
                CHECK((*initial)[0] == type);
                const auto chain = std::ranges::find_if(access_chains, [=](const auto& access) {
                    return access[1] == memory;
                });
                REQUIRE(chain != access_chains.end());
                const auto base = std::ranges::find_if(variables, [&](const auto& var) {
                    return var[1] == (*chain)[2];
                });
                REQUIRE(base != variables.end());
                if ((*base)[2] == static_cast<u32>(spv::StorageClass::StorageBuffer) &&
                    std::ranges::find(return_values, std::vector<u32>{expected}) !=
                        return_values.end()) {
                    ++integer_storage_returns;
                }
                CHECK(std::ranges::count_if(loads, [=](const auto& load) {
                    return load[2] == memory;
                }) == 0);
                CHECK(std::ranges::find(stores, std::vector<u32>{retry, (*initial)[1]}) != stores.end());
                CHECK(std::ranges::any_of(comparisons, [=](const auto& comparison) {
                    return (comparison[2] == result && comparison[3] == expected) ||
                           (comparison[3] == result && comparison[2] == expected);
                }));
            }
            CHECK(integer_storage_returns == 3);
        }
    }
}
