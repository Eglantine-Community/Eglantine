// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// Saturate, clamp and FMNMX with RuntimeInfo::nan_safe_min_max: NMin/NMax, which return the
// operand that isn't NaN as the guest GPU does (Ocarina's Lost Woods bloom blew up from NaN).

#include <unordered_map>
#include <catch2/catch_test_macros.hpp>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
namespace IR = Shader::IR;

// GLSL.std.450 instruction numbers
constexpr u32 FMin = 37;
constexpr u32 FMax = 40;
constexpr u32 FClamp = 43;
constexpr u32 NMin = 79;
constexpr u32 NMax = 80;

std::unordered_map<u32, u32> CountExtInsts(bool nan_safe) {
    Shader::ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Shader::Stage::Fragment;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    IR::IREmitter ir{block};
    const IR::F32 value{ir.Imm32(0.5f)};
    static_cast<void>(ir.FPSaturate(value));
    static_cast<void>(ir.FPClamp(value, ir.Imm32(-1.0f), ir.Imm32(2.0f)));
    static_cast<void>(ir.FPMin(value, ir.Imm32(0.25f)));
    static_cast<void>(ir.FPMax(value, ir.Imm32(0.75f)));
    Shader::Profile profile;
    profile.supported_spirv = 0x00010500;
    Shader::RuntimeInfo runtime_info;
    runtime_info.nan_safe_min_max = nan_safe;
    Shader::Backend::Bindings bindings;
    const auto code = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, bindings);
    std::unordered_map<u32, u32> counts;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        if ((code[offset] & 0xffffU) == 12) { // OpExtInst: type, result, set, instruction
            ++counts[code[offset + 4]];
        }
        offset += size;
    }
    return counts;
}

} // namespace

TEST_CASE("NaN-safe min/max emits NMin/NMax for saturate, clamp and FMNMX",
          "[video_core][spirv][nan_safe_min_max]") {
    const auto plain = CountExtInsts(false);
    CHECK(plain.contains(FClamp));
    CHECK(plain.contains(FMin));
    CHECK(plain.contains(FMax));
    CHECK_FALSE(plain.contains(NMin));
    CHECK_FALSE(plain.contains(NMax));

    const auto safe = CountExtInsts(true);
    CHECK_FALSE(safe.contains(FClamp));
    CHECK_FALSE(safe.contains(FMin));
    CHECK_FALSE(safe.contains(FMax));
    // Saturate and clamp take one NMax and one NMin each, plus the plain min and max.
    CHECK(safe.at(NMin) == 3);
    CHECK(safe.at(NMax) == 3);
}
