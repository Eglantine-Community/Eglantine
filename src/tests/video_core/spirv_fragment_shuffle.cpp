// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <bit>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>
#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

struct ShuffleModule {
    std::vector<u32> code;
    std::array<u32, 4> results;
};

ShuffleModule EmitShuffles(Stage stage, u32 subgroup_size, bool fragment_fallback,
                           bool general_fallback = false, bool native_shuffle = true) {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = stage;
    program.workgroup_size = {64, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.uses_subgroup_invocation_id = true;
    program.info.uses_subgroup_shuffles = true;
    IR::IREmitter ir{block};
    const auto lane = ir.LaneId();
    const auto value = ir.IAdd(lane, ir.Imm32(100));
    const std::array<IR::U32, 4> results{
        ir.ShuffleIndex(value, ir.BitwiseXor(lane, ir.Imm32(1)), ir.Imm32(31), ir.Imm32(0)),
        ir.ShuffleUp(value, ir.Imm32(1), ir.Imm32(0), ir.Imm32(0)),
        ir.ShuffleDown(value, ir.Imm32(1), ir.Imm32(31), ir.Imm32(0)),
        ir.ShuffleButterfly(value, ir.Imm32(1), ir.Imm32(29), ir.Imm32(0)),
    };
    Profile profile;
    profile.supported_spirv = 0x00010500;
    profile.support_ballot = true;
    profile.support_shuffle = native_shuffle;
    profile.support_shuffle_relative = native_shuffle;
    profile.max_subgroup_size = subgroup_size;
    profile.warp_size_potentially_larger_than_guest = subgroup_size > 32;
    profile.has_broken_spirv_fragment_shuffle = fragment_fallback;
    profile.has_broken_spirv_subgroup_shuffle = general_fallback;
    auto code = Backend::SPIRV::EmitSPIRV(profile, program);
    if (const char* directory = std::getenv("EDEN_SHUFFLE_TEST_OUTPUT")) {
        std::filesystem::create_directories(directory);
        const auto name = std::to_string(static_cast<u32>(stage)) + "-" +
                          std::to_string(subgroup_size) + "-" +
                          std::to_string(fragment_fallback) + "-" +
                          std::to_string(general_fallback) + ".spv";
        std::ofstream file{std::filesystem::path{directory} / name, std::ios::binary};
        file.write(reinterpret_cast<const char*>(code.data()),
                   static_cast<std::streamsize>(code.size() * sizeof(u32)));
        REQUIRE(file.good());
    }
    return {std::move(code), {results[0].Inst()->Definition<u32>(),
                              results[1].Inst()->Definition<u32>(),
                              results[2].Inst()->Definition<u32>(),
                              results[3].Inst()->Definition<u32>()}};
}
}

TEST_CASE("Lane registers use only the capabilities needed by their stage",
          "[video_core][spirv][subgroup_capabilities]") {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = GENERATE(Stage::Compute, Stage::Fragment);
    const bool compute = program.stage == Stage::Compute;
    program.workgroup_size = {32, 1, 1};
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.uses_subgroup_invocation_id = true;
    program.info.uses_subgroup_mask = true;
    IR::IREmitter ir{block};
    static_cast<void>(ir.LaneId());
    static_cast<void>(ir.SubgroupGeMask());

    Profile profile;
    profile.supported_spirv = compute ? 0x00010000 : 0x00010500;
    const auto code = Backend::SPIRV::EmitSPIRV(profile, program);
    bool basic{};
    bool lane_id{};
    bool local_index{};
    for (size_t offset = 5; offset < code.size();) {
        const auto words = std::span{code}.subspan(offset);
        const u32 count = words[0] >> 16;
        REQUIRE(count > 0);
        REQUIRE(count <= words.size());
        const auto op = static_cast<spv::Op>(words[0] & 0xffff);
        if (op == spv::Op::OpCapability) {
            const auto capability = static_cast<spv::Capability>(words[1]);
            basic |= capability == spv::Capability::GroupNonUniform;
            CHECK(capability != spv::Capability::GroupNonUniformVote);
            CHECK(capability != spv::Capability::GroupNonUniformBallot);
            CHECK(capability != spv::Capability::GroupNonUniformShuffle);
            CHECK(capability != spv::Capability::GroupNonUniformShuffleRelative);
        }
        if (op == spv::Op::OpDecorate && words[2] == static_cast<u32>(spv::Decoration::BuiltIn)) {
            local_index |= words[3] == static_cast<u32>(spv::BuiltIn::LocalInvocationIndex);
            lane_id |= words[3] == static_cast<u32>(spv::BuiltIn::SubgroupLocalInvocationId);
        }
        offset += count;
    }
    CHECK(basic == !compute);
    CHECK(lane_id == !compute);
    CHECK(local_index == compute);
}

TEST_CASE("Fragment shuffle fallback preserves host subgroup partitions",
          "[video_core][spirv][fragment_shuffle]") {
    for (const u32 subgroup_size : {32U, 64U}) {
        const auto module = EmitShuffles(Stage::Fragment, subgroup_size, false, false, false);
        std::map<u32, std::vector<u32>> definitions;
        std::vector<u32> broadcast_lanes;
        u32 invocation_variable{};
        for (size_t offset = 5; offset < module.code.size();) {
            const u32 size = module.code[offset] >> 16;
            REQUIRE(size != 0);
            REQUIRE(offset + size <= module.code.size());
            const auto words = std::span{module.code}.subspan(offset, size);
            const auto op = static_cast<spv::Op>(words[0] & 0xffffU);
            CHECK(op != spv::Op::OpGroupNonUniformShuffle);
            CHECK(op != spv::Op::OpGroupNonUniformShuffleUp);
            CHECK(op != spv::Op::OpGroupNonUniformShuffleDown);
            CHECK(op != spv::Op::OpGroupNonUniformShuffleXor);
            bool has_result{}, has_type{};
            spv::HasResultAndType(op, &has_result, &has_type);
            if (has_result && has_type) {
                definitions.emplace(words[2], std::vector<u32>{words.begin(), words.end()});
            }
            if (op == spv::Op::OpGroupNonUniformBroadcast) {
                broadcast_lanes.push_back(words[5]);
            }
            if (op == spv::Op::OpDecorate &&
                words[2] == static_cast<u32>(spv::Decoration::BuiltIn) &&
                words[3] == static_cast<u32>(spv::BuiltIn::SubgroupLocalInvocationId)) {
                invocation_variable = words[1];
            }
            offset += size;
        }
        REQUIRE(broadcast_lanes.size() == module.results.size() * subgroup_size);
        REQUIRE(invocation_variable != 0);
        for (u32 index = 0; index < broadcast_lanes.size(); ++index) {
            CHECK(definitions.at(broadcast_lanes[index])[3] == index % subgroup_size);
        }
        const auto evaluate = [&](auto&& self, u32 id, u32 lane) -> u32 {
            const auto& words = definitions.at(id);
            const auto arg = [&](size_t index) { return self(self, words[index], lane); };
            switch (static_cast<spv::Op>(words[0] & 0xffffU)) {
            case spv::Op::OpConstant: return words[3];
            case spv::Op::OpLoad:
                REQUIRE(words[3] == invocation_variable);
                return lane;
            case spv::Op::OpIAdd: return arg(3) + arg(4);
            case spv::Op::OpISub: return arg(3) - arg(4);
            case spv::Op::OpBitwiseAnd: return arg(3) & arg(4);
            case spv::Op::OpBitwiseOr: return arg(3) | arg(4);
            case spv::Op::OpBitwiseXor: return arg(3) ^ arg(4);
            case spv::Op::OpNot: return ~arg(3);
            case spv::Op::OpShiftLeftLogical: return arg(3) << arg(4);
            case spv::Op::OpShiftRightLogical: return arg(3) >> arg(4);
            case spv::Op::OpIEqual: return arg(3) == arg(4);
            case spv::Op::OpSLessThanEqual:
                return std::bit_cast<s32>(arg(3)) <= std::bit_cast<s32>(arg(4));
            case spv::Op::OpSGreaterThanEqual:
                return std::bit_cast<s32>(arg(3)) >= std::bit_cast<s32>(arg(4));
            case spv::Op::OpSelect: return arg(3) ? arg(4) : arg(5);
            case spv::Op::OpGroupNonUniformBroadcast:
                CHECK(arg(5) / 32 == lane / 32);
                return self(self, words[4], arg(5));
            default: FAIL("Unexpected shuffle opcode " << (words[0] & 0xffffU));
            }
            return 0;
        };
        for (u32 lane = 0; lane < subgroup_size; ++lane) {
            CAPTURE(subgroup_size, lane);
            const u32 guest_lane = lane % 32;
            CHECK(evaluate(evaluate, module.results[0], lane) == 100 + (guest_lane ^ 1));
            CHECK(evaluate(evaluate, module.results[1], lane) ==
                  100 + (guest_lane == 0 ? 0 : guest_lane - 1));
            CHECK(evaluate(evaluate, module.results[2], lane) ==
                  100 + (guest_lane == 31 ? 31 : guest_lane + 1));
            const u32 butterfly_source = guest_lane ^ 1;
            CHECK(evaluate(evaluate, module.results[3], lane) ==
                  100 + (butterfly_source <= 29 ? butterfly_source : guest_lane));
        }
    }
}

TEST_CASE("Fragment workaround leaves other stages on native shuffles",
          "[video_core][spirv][fragment_shuffle]") {
    for (const auto stage : {Stage::Fragment, Stage::Compute, Stage::VertexB}) {
        for (const bool fragment_fallback : {false, true}) {
            for (const bool general_fallback : {false, true}) {
                const auto module = EmitShuffles(stage, 64, fragment_fallback, general_fallback);
                size_t broadcasts{}, shuffles{};
                std::set<spv::Capability> capabilities;
                for (size_t offset = 5; offset < module.code.size();) {
                    const u32 size = module.code[offset] >> 16;
                    REQUIRE(size != 0);
                    const auto op = static_cast<spv::Op>(module.code[offset] & 0xffffU);
                    if (op == spv::Op::OpCapability) {
                        capabilities.insert(static_cast<spv::Capability>(module.code[offset + 1]));
                    }
                    broadcasts += op == spv::Op::OpGroupNonUniformBroadcast;
                    shuffles += op == spv::Op::OpGroupNonUniformShuffle ||
                                op == spv::Op::OpGroupNonUniformShuffleUp ||
                                op == spv::Op::OpGroupNonUniformShuffleDown ||
                                op == spv::Op::OpGroupNonUniformShuffleXor;
                    offset += size;
                }
                const bool fallback = general_fallback ||
                                      (fragment_fallback && stage == Stage::Fragment);
                CHECK(broadcasts == (fallback ? module.results.size() * 64 : 0u));
                CHECK(shuffles == (fallback ? 0u : module.results.size()));
                CHECK(capabilities.contains(spv::Capability::GroupNonUniformBallot) == fallback);
                CHECK(capabilities.contains(spv::Capability::GroupNonUniformShuffle) == !fallback);
                CHECK(capabilities.contains(spv::Capability::GroupNonUniformShuffleRelative) ==
                      !fallback);
            }
        }
    }
}

TEST_CASE("Compute broadcasts are reused only before divergence or a barrier",
          "[video_core][spirv][subgroup_capabilities]") {
    const bool memory_barrier = GENERATE(false, true);
    const auto barrier_op = memory_barrier ? spv::Op::OpMemoryBarrier : spv::Op::OpControlBarrier;
    constexpr u32 subgroup_size = 32;
    ObjectPool<IR::Inst> pool;
    IR::Block entry{pool}, branch{pool}, merge{pool};
    IR::Program program;
    program.stage = Stage::Compute;
    program.workgroup_size = {subgroup_size, 1, 1};
    program.blocks = {&entry, &branch, &merge};
    program.post_order_blocks = {&merge, &branch, &entry};
    program.info.uses_subgroup_invocation_id = true;
    program.info.uses_subgroup_shuffles = true;

    IR::IREmitter ir{entry};
    const auto lane = ir.LaneId();
    const auto value = ir.IAdd(lane, ir.Imm32(100));
    const auto shuffles = [&](IR::IREmitter& emitter) {
        for (const u32 mask : {2U, 4U}) {
            const auto source = emitter.BitwiseXor(lane, emitter.Imm32(mask));
            static_cast<void>(
                emitter.ShuffleIndex(value, source, emitter.Imm32(31), emitter.Imm32(0)));
        }
    };
    shuffles(ir);
    if (memory_barrier) {
        ir.WorkgroupMemoryBarrier();
    } else {
        ir.Barrier();
    }
    shuffles(ir);
    const auto odd = ir.INotEqual(ir.BitwiseAnd(lane, ir.Imm32(1)), ir.Imm32(0));
    IR::IREmitter inside{branch};
    shuffles(inside);
    program.syntax_list = {
        {{.block = &entry}, IR::AbstractSyntaxNode::Type::Block},
        {{.if_node = {odd, &branch, &merge}}, IR::AbstractSyntaxNode::Type::If},
        {{.block = &branch}, IR::AbstractSyntaxNode::Type::Block},
        {{.end_if = {&merge}}, IR::AbstractSyntaxNode::Type::EndIf},
        {{.block = &merge}, IR::AbstractSyntaxNode::Type::Block},
        {{}, IR::AbstractSyntaxNode::Type::Return},
    };

    Profile profile;
    profile.supported_spirv = 0x00010300;
    profile.support_ballot = true;
    profile.max_subgroup_size = subgroup_size;
    const auto code = Backend::SPIRV::EmitSPIRV(profile, program);

    std::array<size_t, 4> broadcasts{};
    size_t region{};
    for (size_t offset = 5; offset < code.size();) {
        const u32 count = code[offset] >> 16;
        REQUIRE(count > 0);
        REQUIRE(count <= code.size() - offset);
        const auto op = static_cast<spv::Op>(code[offset] & 0xffff);
        if (op == barrier_op || op == spv::Op::OpBranchConditional || op == spv::Op::OpBranch) {
            ++region;
        }
        if (op == spv::Op::OpGroupNonUniformBroadcast) {
            REQUIRE(region < broadcasts.size());
            ++broadcasts[region];
        }
        offset += count;
    }

    REQUIRE(region == 3);
    CHECK(broadcasts[0] == subgroup_size);
    CHECK(broadcasts[1] == 2 * subgroup_size);
    CHECK(broadcasts[2] == 2 * subgroup_size);
    CHECK(broadcasts[3] == 0);
}

TEST_CASE("Quad shuffles reject stages without an equivalent lowering",
          "[video_core][spirv][subgroup_capabilities]") {
    ObjectPool<IR::Inst> pool;
    IR::Block block{pool};
    IR::Program program;
    program.stage = Stage::Fragment;
    program.blocks = {&block};
    program.post_order_blocks = {&block};
    program.syntax_list = {{{.block = &block}, IR::AbstractSyntaxNode::Type::Block},
                           {{}, IR::AbstractSyntaxNode::Type::Return}};
    program.info.uses_quad_shuffles = true;
    IR::IREmitter ir{block};
    static_cast<void>(ir.QuadSwap(ir.Imm32(30), ir.Imm32(0)));

    Profile profile;
    profile.supported_spirv = 0x00010300;
    profile.support_shuffle = true;
    profile.supported_subgroup_stages = 0;
    CHECK_THROWS_AS(Backend::SPIRV::EmitSPIRV(profile, program), Shader::NotImplementedException);

    profile.supported_subgroup_stages = 1U << static_cast<u32>(Stage::Fragment);
    CHECK_THROWS_AS(Backend::SPIRV::EmitSPIRV(profile, program), Shader::NotImplementedException);

    profile.support_quad_shuffles = true;
    CHECK_NOTHROW(Backend::SPIRV::EmitSPIRV(profile, program));

    RuntimeInfo runtime{};
    runtime.mesh_as_compute = true;
    profile.supported_subgroup_stages = 1U << static_cast<u32>(Stage::Compute);
    Backend::Bindings bindings{};
    CHECK_THROWS_AS(Backend::SPIRV::EmitSPIRV(profile, runtime, program, bindings),
                    Shader::NotImplementedException);

    profile.supported_spirv = 0x00010000;
    CHECK_THROWS_AS(Backend::SPIRV::EmitSPIRV(profile, program), Shader::NotImplementedException);
}
