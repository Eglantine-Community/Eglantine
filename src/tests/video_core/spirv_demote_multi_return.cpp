// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <catch2/catch_test_macros.hpp>
#define SPV_ENABLE_UTILITY_CODE
#include <spirv/unified1/spirv.hpp11>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/ir/ir_emitter.h"

namespace {
using namespace Shader;

struct DemoteCounts {
    size_t demotes{};
    size_t kills{};
    size_t returns{};
    bool declares_demote_capability{};
};

DemoteCounts EmitAndCount(bool has_broken_demote_reorder, bool with_side_effect_write) {
    ObjectPool<IR::Inst> inst_pool;
    ObjectPool<IR::Block> block_pool;
    IR::Block* const entry{block_pool.Create(inst_pool)};
    IR::Block* const demote_block{block_pool.Create(inst_pool)};
    IR::Block* const after_demote{block_pool.Create(inst_pool)};
    IR::Block* const early_return{block_pool.Create(inst_pool)};
    IR::Block* const main_return{block_pool.Create(inst_pool)};

    IR::IREmitter entry_ir{*entry};
    entry_ir.Prologue();
    const IR::U1 cond{entry_ir.IEqual(entry_ir.LaneId(), entry_ir.Imm32(0U))};

    IR::IREmitter{*demote_block}.DemoteToHelperInvocation();

    IR::IREmitter early_ir{*early_return};
    early_ir.SetFragColor(0, 0, early_ir.Imm32(1.0f));
    early_ir.Epilogue();

    IR::IREmitter main_ir{*main_return};
    main_ir.SetFragColor(0, 0, main_ir.Imm32(0.5f));
    main_ir.Epilogue();

    IR::Program program;
    program.stage = Stage::Fragment;
    program.blocks = {entry, demote_block, after_demote, early_return, main_return};
    program.post_order_blocks = {main_return, early_return, after_demote, demote_block, entry};

    auto& asl = program.syntax_list;
    asl.push_back({{.block = entry}, IR::AbstractSyntaxNode::Type::Block});
    {
        IR::AbstractSyntaxNode node{};
        node.type = IR::AbstractSyntaxNode::Type::If;
        node.data.if_node = {.cond = cond, .body = demote_block, .merge = after_demote};
        asl.push_back(node);
    }
    asl.push_back({{.block = demote_block}, IR::AbstractSyntaxNode::Type::Block});
    {
        IR::AbstractSyntaxNode node{};
        node.type = IR::AbstractSyntaxNode::Type::EndIf;
        node.data.end_if = {.merge = after_demote};
        asl.push_back(node);
    }
    asl.push_back({{.block = after_demote}, IR::AbstractSyntaxNode::Type::Block});
    {
        IR::AbstractSyntaxNode node{};
        node.type = IR::AbstractSyntaxNode::Type::If;
        node.data.if_node = {.cond = cond, .body = early_return, .merge = main_return};
        asl.push_back(node);
    }
    asl.push_back({{.block = early_return}, IR::AbstractSyntaxNode::Type::Block});
    asl.push_back({{}, IR::AbstractSyntaxNode::Type::Return});
    {
        IR::AbstractSyntaxNode node{};
        node.type = IR::AbstractSyntaxNode::Type::EndIf;
        node.data.end_if = {.merge = main_return};
        asl.push_back(node);
    }
    asl.push_back({{.block = main_return}, IR::AbstractSyntaxNode::Type::Block});
    asl.push_back({{}, IR::AbstractSyntaxNode::Type::Return});

    program.info.uses_subgroup_invocation_id = true;
    program.info.uses_demote_to_helper_invocation = true;
    program.info.stores_frag_color[0] = true;
    if (with_side_effect_write) {
        program.info.stores_global_memory = true;
    }

    Profile profile{};
    profile.supported_spirv = 0x00010500;
    profile.support_vote = true;
    profile.support_demote_to_helper_invocation = true;
    profile.has_broken_spirv_demote_reorder = has_broken_demote_reorder;

    const auto code = Backend::SPIRV::EmitSPIRV(profile, program);
    DemoteCounts counts;
    for (size_t offset = 5; offset < code.size();) {
        const u32 size = code[offset] >> 16;
        REQUIRE(size != 0);
        REQUIRE(offset + size <= code.size());
        const auto op = static_cast<spv::Op>(code[offset] & 0xffffU);
        counts.demotes += op == spv::Op::OpDemoteToHelperInvocationEXT;
        counts.kills += op == spv::Op::OpKill;
        counts.returns += op == spv::Op::OpReturn;
        if (op == spv::Op::OpCapability &&
            code[offset + 1] == static_cast<u32>(spv::Capability::DemoteToHelperInvocation)) {
            counts.declares_demote_capability = true;
        }
        offset += size;
    }
    return counts;
}
} // namespace

TEST_CASE("Deferred demote workaround guards every return with OpKill",
          "[video_core][spirv][demote_multi_return]") {
    const auto standard = EmitAndCount(false, false);
    CHECK(standard.demotes == 1);
    CHECK(standard.kills == 0);
    CHECK(standard.returns == 2);
    CHECK(standard.declares_demote_capability);

    const auto workaround_pure = EmitAndCount(true, false);
    CHECK(workaround_pure.demotes == 0);
    CHECK(workaround_pure.kills == 2);
    CHECK(workaround_pure.returns == 2);
    CHECK_FALSE(workaround_pure.declares_demote_capability);

    const auto workaround_side_effect = EmitAndCount(true, true);
    CHECK(workaround_side_effect.demotes == 1);
    CHECK(workaround_side_effect.kills == 2);
    CHECK(workaround_side_effect.returns == 2);
    CHECK(workaround_side_effect.declares_demote_capability);
}

