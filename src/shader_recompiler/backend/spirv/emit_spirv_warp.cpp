// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {
namespace {
using spv::Capability;

Id SubgroupScope(EmitContext& ctx, Capability capability) {
    if (capability == Capability::GroupNonUniformBallot && !ctx.profile.support_ballot) {
        throw NotImplementedException("SPIR-V subgroup operation requires BALLOT support");
    }
    ctx.AddCapability(capability);
    return ctx.Const(static_cast<u32>(spv::Scope::Subgroup));
}

bool StageSupportsSubgroups(EmitContext& ctx) {
    return ctx.SupportsSubgroups();
}

Id GetThreadId(EmitContext& ctx) {
    if (!StageSupportsSubgroups(ctx)) {
        return ctx.u32_zero_value;
    }
    return ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
}

Id WarpExtract(EmitContext& ctx, Id value) {
    const Id thread_id{GetThreadId(ctx)};
    const Id local_index{ctx.OpShiftRightArithmetic(ctx.U32[1], thread_id, ctx.Const(5U))};
    if (ctx.profile.has_broken_spirv_subgroup_mask_vector_extract_dynamic) {
        const Id c0_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(0U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 0U), ctx.Const(0U))};
        const Id c1_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(1U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 1U), ctx.Const(0U))};
        const Id c2_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(2U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 2U), ctx.Const(0U))};
        const Id c3_sel{ctx.OpSelect(ctx.U32[1], ctx.OpIEqual(ctx.U1, local_index, ctx.Const(3U)),
                                     ctx.OpCompositeExtract(ctx.U32[1], value, 3U), ctx.Const(0U))};
        const Id c0_or_c1{ctx.OpBitwiseOr(ctx.U32[1], c0_sel, c1_sel)};
        const Id c2_or_c3{ctx.OpBitwiseOr(ctx.U32[1], c2_sel, c3_sel)};
        const Id c0_or_c1_or_c2_or_c3{ctx.OpBitwiseOr(ctx.U32[1], c0_or_c1, c2_or_c3)};
        return c0_or_c1_or_c2_or_c3;
    } else {
        return ctx.OpVectorExtractDynamic(ctx.U32[1], value, local_index);
    }
}

Id LoadMask(EmitContext& ctx, Id mask) {
    const Id value{ctx.OpLoad(ctx.U32[4], mask)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpCompositeExtract(ctx.U32[1], value, 0U);
    }
    return WarpExtract(ctx, value);
}

void SetInBoundsFlag(IR::Inst* inst, Id result) {
    IR::Inst* const in_bounds{inst->GetAssociatedPseudoOperation(IR::Opcode::GetInBoundsFromOp)};
    if (!in_bounds) {
        return;
    }
    in_bounds->SetDefinition(result);
    in_bounds->Invalidate();
}

Id ComputeMinThreadId(EmitContext& ctx, Id thread_id, Id segmentation_mask) {
    return ctx.OpBitwiseAnd(ctx.U32[1], thread_id, segmentation_mask);
}

Id ComputeMaxThreadId(EmitContext& ctx, Id min_thread_id, Id clamp, Id not_seg_mask) {
    return ctx.OpBitwiseOr(ctx.U32[1], min_thread_id,
                           ctx.OpBitwiseAnd(ctx.U32[1], clamp, not_seg_mask));
}

Id GetMaxThreadId(EmitContext& ctx, Id thread_id, Id clamp, Id segmentation_mask) {
    const Id not_seg_mask{ctx.OpNot(ctx.U32[1], segmentation_mask)};
    const Id min_thread_id{ComputeMinThreadId(ctx, thread_id, segmentation_mask)};
    return ComputeMaxThreadId(ctx, min_thread_id, clamp, not_seg_mask);
}

Id HostThreadId(EmitContext& ctx, Id thread_id) {
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return thread_id;
    }
    const Id partition_idx{ctx.OpShiftRightLogical(ctx.U32[1], GetThreadId(ctx), ctx.Const(5u))};
    const Id partition_base{ctx.OpShiftLeftLogical(ctx.U32[1], partition_idx, ctx.Const(5u))};
    return ctx.OpIAdd(ctx.U32[1], thread_id, partition_base);
}

Id GuestLane(EmitContext& ctx, Id index) {
    return ctx.OpBitwiseAnd(ctx.U32[1], index, ctx.Const(31U));
}

Id ScratchPointer(EmitContext& ctx, Id index) {
    const Id pointer{ctx.TypePointer(spv::StorageClass::Workgroup, ctx.U32[1])};
    if (ctx.uses_explicit_workgroup_layout) {
        return ctx.OpAccessChain(pointer, ctx.workgroup_warp_scratch, ctx.Const(1u), index);
    }
    return ctx.OpAccessChain(pointer, ctx.workgroup_warp_scratch, index);
}

Id LocalWarpBase(EmitContext& ctx) {
    const Id local{ctx.OpLoad(ctx.U32[1], ctx.local_invocation_index)};
    return ctx.OpBitwiseAnd(ctx.U32[1], local, ctx.Const(~31u));
}

Id ActiveWarpMask(EmitContext& ctx) {
    const u32 threads = ctx.workgroup_warp_threads;
    const u32 tail = threads % 32;
    if (tail == 0) {
        return ctx.Const(~0u);
    }
    const Id is_tail{ctx.OpIEqual(ctx.U1, LocalWarpBase(ctx), ctx.Const(threads - tail))};
    return ctx.OpSelect(ctx.U32[1], is_tail, ctx.Const((1u << tail) - 1), ctx.Const(~0u));
}

Id PublishWarpValue(EmitContext& ctx, Id value) {
    const Id local{ctx.OpLoad(ctx.U32[1], ctx.local_invocation_index)};
    // The setup pass proved a single straight-line path. Equal SSA IDs refer to the same
    // immutable value in every invocation until another payload overwrites the scratch.
    if (ctx.workgroup_warp_payload.value != value.value) {
        // Wait for readers before reusing the allocation. The first publication has no readers.
        if (ctx.workgroup_warp_payload.value) {
            EmitBarrier(ctx);
        }
        ctx.OpStore(ScratchPointer(ctx, local), value);
        EmitBarrier(ctx);
        ctx.workgroup_warp_payload = value;
    }
    return ctx.OpBitwiseAnd(ctx.U32[1], local, ctx.Const(~31u));
}

Id ReadWarpValue(EmitContext& ctx, Id warp_base, Id lane) {
    const Id local_id{ctx.OpBitwiseOr(ctx.U32[1], warp_base, GuestLane(ctx, lane))};
    if (ctx.workgroup_warp_threads % 32 == 0) {
        return ctx.OpLoad(ctx.U32[1], ScratchPointer(ctx, local_id));
    }
    const Id valid{ctx.OpULessThan(ctx.U1, local_id, ctx.Const(ctx.workgroup_warp_threads))};
    const Id safe_id{ctx.OpSelect(ctx.U32[1], valid, local_id, warp_base)};
    const Id word{ctx.OpLoad(ctx.U32[1], ScratchPointer(ctx, safe_id))};
    // Inactive lanes contribute zero to ballots. Shuffle from an inactive lane has no
    // guest-defined payload; returning zero keeps the load defined and preserves clamp flags.
    return ctx.OpSelect(ctx.U32[1], valid, word, ctx.u32_zero_value);
}

bool HasBrokenShuffle(EmitContext& ctx) {
    return ctx.profile.has_broken_spirv_subgroup_shuffle ||
           (ctx.profile.has_broken_spirv_fragment_shuffle && ctx.stage == Stage::Fragment);
}

bool HasNativeShuffle(EmitContext& ctx) {
    return ctx.profile.support_shuffle && !HasBrokenShuffle(ctx);
}

Id ShuffleAbsolute(EmitContext& ctx, Id value, Id src_thread_id) {
    if (HasNativeShuffle(ctx)) {
        return ctx.OpGroupNonUniformShuffle(ctx.U32[1],
                                            SubgroupScope(ctx, Capability::GroupNonUniformShuffle),
                                            value, src_thread_id);
    }
    const Id scope{SubgroupScope(ctx, Capability::GroupNonUniformBallot)};
    const u32 lanes{ctx.profile.max_subgroup_size};
    if (lanes == 0 || lanes > 128) {
        throw NotImplementedException("Invalid subgroup lane limit {} for shader shuffle", lanes);
    }
    Id result{ctx.u32_zero_value};
    for (u32 lane = 0; lane < lanes; ++lane) {
        const Id read{ctx.BroadcastSubgroupU32(scope, value, lane)};
        const Id matches{ctx.OpIEqual(ctx.U1, src_thread_id, ctx.Const(lane))};
        result = ctx.OpSelect(ctx.U32[1], matches, read, result);
    }
    return result;
}

Id ReadScalarWarp(EmitContext& ctx, Id value, Id lane) {
    // An inactive source has no defined guest payload. Choosing zero never reads another lane.
    const Id is_self{ctx.OpIEqual(ctx.U1, lane, ctx.u32_zero_value)};
    return ctx.OpSelect(ctx.U32[1], is_self, value, ctx.u32_zero_value);
}

Id ShuffleRelative(EmitContext& ctx, Id value, Id delta, Id src_thread_id, spv::Op op) {
    if (ctx.HasScalarWarp()) {
        return ReadScalarWarp(ctx, value, src_thread_id);
    }
    if (ctx.workgroup_warp_scratch.value) {
        return ReadWarpValue(ctx, PublishWarpValue(ctx, value), src_thread_id);
    }
    if (!ctx.profile.support_shuffle_relative || HasBrokenShuffle(ctx)) {
        return ShuffleAbsolute(ctx, value, HostThreadId(ctx, src_thread_id));
    }
    const Id result{ctx.OpGroupNonUniformShuffleXor(
        ctx.U32[1], SubgroupScope(ctx, Capability::GroupNonUniformShuffleRelative), value, delta)};
    ctx.opcode_rewrites.emplace_back(result.value, op);
    return result;
}

Id BroadcastLane(EmitContext& ctx, Id value, u32 lane) {
    if (!ctx.profile.support_ballot) {
        return ShuffleAbsolute(ctx, value, HostThreadId(ctx, ctx.Const(lane)));
    }
    const Id scope{SubgroupScope(ctx, Capability::GroupNonUniformBallot)};
    Id result{ctx.BroadcastSubgroupU32(scope, value, lane)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return result;
    }
    const Id partition_idx{ctx.OpShiftRightLogical(ctx.U32[1], GetThreadId(ctx), ctx.Const(5u))};
    for (u32 base = 32; base < ctx.profile.max_subgroup_size; base += 32) {
        const Id read{ctx.BroadcastSubgroupU32(scope, value, base + lane)};
        const Id matches{ctx.OpIEqual(ctx.U1, partition_idx, ctx.Const(base >> 5))};
        result = ctx.OpSelect(ctx.U32[1], matches, read, result);
    }
    return result;
}
void ValidateQuadShuffles(const EmitContext& ctx) {
    if (!ctx.SupportsSubgroups()) {
        throw NotImplementedException("SPIR-V quad shuffle requires subgroup support in the shader stage");
    }
    // Ordinary subgroup operations may treat fragment helper invocations as inactive.
    if (ctx.stage == Stage::Fragment && !ctx.SupportsQuadShuffles()) {
        throw NotImplementedException("SPIR-V fragment quad shuffle requires native quad support");
    }
}
} // Anonymous namespace

Id EmitLaneId(EmitContext& ctx) {
    if (ctx.HasScalarWarp()) {
        return ctx.u32_zero_value;
    }
    if (ctx.local_invocation_index.value) {
        const Id index{ctx.OpLoad(ctx.U32[1], ctx.local_invocation_index)};
        return GuestLane(ctx, index);
    }
    const Id id{GetThreadId(ctx)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return id;
    }
    return ctx.OpBitwiseAnd(ctx.U32[1], id, ctx.Const(31U));
}

Id EmitVoteAll(EmitContext& ctx, Id pred) {
    if (ctx.workgroup_warp_scratch.value) {
        return ctx.OpIEqual(ctx.U1, EmitSubgroupBallot(ctx, pred), ActiveWarpMask(ctx));
    }
    if (ctx.HasScalarWarp() || !StageSupportsSubgroups(ctx)) {
        return pred;
    }
    if (!ctx.profile.warp_size_potentially_larger_than_guest && ctx.profile.support_vote) {
        return ctx.OpGroupNonUniformAll(ctx.U1, SubgroupScope(ctx, Capability::GroupNonUniformVote),
                                        pred);
    }
    const Id failed{EmitSubgroupBallot(ctx, ctx.OpLogicalNot(ctx.U1, pred))};
    return ctx.OpIEqual(ctx.U1, failed, ctx.u32_zero_value);
}

Id EmitVoteAny(EmitContext& ctx, Id pred) {
    if (ctx.workgroup_warp_scratch.value) {
        return ctx.OpINotEqual(ctx.U1, EmitSubgroupBallot(ctx, pred), ctx.u32_zero_value);
    }
    if (ctx.HasScalarWarp() || !StageSupportsSubgroups(ctx)) {
        return pred;
    }
    if (!ctx.profile.warp_size_potentially_larger_than_guest && ctx.profile.support_vote) {
        return ctx.OpGroupNonUniformAny(ctx.U1, SubgroupScope(ctx, Capability::GroupNonUniformVote),
                                        pred);
    }
    return ctx.OpINotEqual(ctx.U1, EmitSubgroupBallot(ctx, pred), ctx.u32_zero_value);
}

Id EmitVoteEqual(EmitContext& ctx, Id pred) {
    if (ctx.workgroup_warp_scratch.value) {
        const Id mask{EmitSubgroupBallot(ctx, pred)};
        return ctx.OpLogicalOr(ctx.U1, ctx.OpIEqual(ctx.U1, mask, ctx.u32_zero_value),
                              ctx.OpIEqual(ctx.U1, mask, ActiveWarpMask(ctx)));
    }
    if (ctx.HasScalarWarp() || !StageSupportsSubgroups(ctx)) {
        return ctx.true_value;
    }
    if (!ctx.profile.warp_size_potentially_larger_than_guest && ctx.profile.support_vote) {
        return ctx.OpGroupNonUniformAllEqual(
            ctx.U1, SubgroupScope(ctx, Capability::GroupNonUniformVote), pred);
    }
    const Id ballot{EmitSubgroupBallot(ctx, pred)};
    const Id active{EmitSubgroupBallot(ctx, ctx.true_value)};
    return ctx.OpLogicalOr(ctx.U1, ctx.OpIEqual(ctx.U1, ballot, ctx.u32_zero_value),
                           ctx.OpIEqual(ctx.U1, ballot, active));
}

Id EmitSubgroupBallot(EmitContext& ctx, Id pred) {
    if (ctx.workgroup_warp_scratch.value) {
        if (pred.value == ctx.true_value.value) {
            return ActiveWarpMask(ctx);
        }
        if (pred.value == ctx.false_value.value) {
            return ctx.u32_zero_value;
        }
        if (pred.value == ctx.workgroup_warp_ballot_predicate.value) {
            return ctx.workgroup_warp_ballot;
        }
        const Id lane{EmitLaneId(ctx)};
        const Id bit{ctx.OpShiftLeftLogical(ctx.U32[1], ctx.Const(1u), lane)};
        const Id value{ctx.OpSelect(ctx.U32[1], pred, bit, ctx.u32_zero_value)};
        const Id warp_base{PublishWarpValue(ctx, value)};
        Id mask{ctx.u32_zero_value};
        for (u32 i = 0; i < 32; ++i) {
            mask = ctx.OpBitwiseOr(ctx.U32[1], mask, ReadWarpValue(ctx, warp_base, ctx.Const(i)));
        }
        ctx.workgroup_warp_ballot_predicate = pred;
        ctx.workgroup_warp_ballot = mask;
        return mask;
    }
    if (ctx.HasScalarWarp() || !StageSupportsSubgroups(ctx)) {
        return ctx.OpSelect(ctx.U32[1], pred, ctx.Const(1u), ctx.u32_zero_value);
    }
    const Id ballot{ctx.OpGroupNonUniformBallot(
        ctx.U32[4], SubgroupScope(ctx, Capability::GroupNonUniformBallot), pred)};
    if (!ctx.profile.warp_size_potentially_larger_than_guest) {
        return ctx.OpCompositeExtract(ctx.U32[1], ballot, 0U);
    }
    return WarpExtract(ctx, ballot);
}

Id EmitSubgroupEqMask(EmitContext& ctx) {
    if (ctx.subgroup_mask_eq.value) {
        return LoadMask(ctx, ctx.subgroup_mask_eq);
    }
    return ctx.OpShiftLeftLogical(ctx.U32[1], ctx.Const(1u), EmitLaneId(ctx));
}

Id EmitSubgroupLtMask(EmitContext& ctx) {
    if (ctx.subgroup_mask_lt.value) {
        return LoadMask(ctx, ctx.subgroup_mask_lt);
    }
    return ctx.OpISub(ctx.U32[1], EmitSubgroupEqMask(ctx), ctx.Const(1u));
}

Id EmitSubgroupLeMask(EmitContext& ctx) {
    if (ctx.subgroup_mask_le.value) {
        return LoadMask(ctx, ctx.subgroup_mask_le);
    }
    const Id greater{ctx.OpShiftLeftLogical(ctx.U32[1], ctx.Const(~1u), EmitLaneId(ctx))};
    return ctx.OpNot(ctx.U32[1], greater);
}

Id EmitSubgroupGtMask(EmitContext& ctx) {
    if (ctx.subgroup_mask_gt.value) {
        return LoadMask(ctx, ctx.subgroup_mask_gt);
    }
    if (!ctx.HasScalarWarp() && !ctx.local_invocation_index.value && !ctx.SupportsSubgroups()) {
        return ctx.u32_zero_value;
    }
    return ctx.OpShiftLeftLogical(ctx.U32[1], ctx.Const(~1u), EmitLaneId(ctx));
}

Id EmitSubgroupGeMask(EmitContext& ctx) {
    if (ctx.subgroup_mask_ge.value) {
        return LoadMask(ctx, ctx.subgroup_mask_ge);
    }
    if (!ctx.HasScalarWarp() && !ctx.local_invocation_index.value && !ctx.SupportsSubgroups()) {
        return ctx.Const(1u);
    }
    return ctx.OpShiftLeftLogical(ctx.U32[1], ctx.Const(~0u), EmitLaneId(ctx));
}

Id EmitShuffleIndex(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                    Id segmentation_mask) {
    const Id not_seg_mask{ctx.OpNot(ctx.U32[1], segmentation_mask)};
    const Id thread_id{EmitLaneId(ctx)};
    const Id min_thread_id{ComputeMinThreadId(ctx, thread_id, segmentation_mask)};
    const Id max_thread_id{ComputeMaxThreadId(ctx, min_thread_id, clamp, not_seg_mask)};

    const Id lhs{ctx.OpBitwiseAnd(ctx.U32[1], GuestLane(ctx, index), not_seg_mask)};
    const Id src_thread_id{ctx.OpBitwiseOr(ctx.U32[1], lhs, min_thread_id)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    SetInBoundsFlag(inst, in_range);
    if (ctx.workgroup_warp_scratch.value) {
        const Id warp_base{PublishWarpValue(ctx, value)};
        const Id shuffled{ReadWarpValue(ctx, warp_base, src_thread_id)};
        return ctx.OpSelect(ctx.U32[1], in_range, shuffled, value);
    }
    if (ctx.HasScalarWarp()) {
        return ctx.OpSelect(ctx.U32[1], in_range, ReadScalarWarp(ctx, value, src_thread_id), value);
    }
    if (!StageSupportsSubgroups(ctx)) {
        return value;
    }
    const IR::Value lane{inst->Arg(1).Resolve()};
    const IR::Value segment{inst->Arg(3).Resolve()};
    if (lane.IsImmediate() && segment.IsImmediate() && segment.U32() == 0) {
        return ctx.OpSelect(ctx.U32[1], in_range, BroadcastLane(ctx, value, lane.U32() & 31),
                            value);
    }
    const Id shuffled{ShuffleAbsolute(ctx, value, HostThreadId(ctx, src_thread_id))};
    return ctx.OpSelect(ctx.U32[1], in_range, shuffled, value);
}

Id EmitShuffleUp(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                 Id segmentation_mask) {
    if (!ctx.HasScalarWarp() && !ctx.workgroup_warp_scratch.value &&
        !StageSupportsSubgroups(ctx)) {
        SetInBoundsFlag(inst, ctx.false_value);
        return value;
    }
    const Id delta{GuestLane(ctx, index)};
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    const Id src_thread_id{ctx.OpISub(ctx.U32[1], thread_id, delta)};
    const Id in_range{ctx.OpSGreaterThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    SetInBoundsFlag(inst, in_range);
    const Id shuffled{ShuffleRelative(ctx, value, delta, src_thread_id,
                                      spv::Op::OpGroupNonUniformShuffleUp)};
    return ctx.OpSelect(ctx.U32[1], in_range, shuffled, value);
}

Id EmitShuffleDown(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                   Id segmentation_mask) {
    if (!ctx.HasScalarWarp() && !ctx.workgroup_warp_scratch.value &&
        !StageSupportsSubgroups(ctx)) {
        SetInBoundsFlag(inst, ctx.false_value);
        return value;
    }
    const Id delta{GuestLane(ctx, index)};
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    const Id src_thread_id{ctx.OpIAdd(ctx.U32[1], thread_id, delta)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    SetInBoundsFlag(inst, in_range);
    const Id shuffled{ShuffleRelative(ctx, value, delta, src_thread_id,
                                      spv::Op::OpGroupNonUniformShuffleDown)};
    return ctx.OpSelect(ctx.U32[1], in_range, shuffled, value);
}

Id EmitShuffleButterfly(EmitContext& ctx, IR::Inst* inst, Id value, Id index, Id clamp,
                        Id segmentation_mask) {
    if (!ctx.HasScalarWarp() && !ctx.workgroup_warp_scratch.value &&
        !StageSupportsSubgroups(ctx)) {
        SetInBoundsFlag(inst, ctx.false_value);
        return value;
    }
    const Id mask{GuestLane(ctx, index)};
    const Id thread_id{EmitLaneId(ctx)};
    const Id max_thread_id{GetMaxThreadId(ctx, thread_id, clamp, segmentation_mask)};
    const Id src_thread_id{ctx.OpBitwiseXor(ctx.U32[1], thread_id, mask)};
    const Id in_range{ctx.OpSLessThanEqual(ctx.U1, src_thread_id, max_thread_id)};

    SetInBoundsFlag(inst, in_range);
    if (ctx.workgroup_warp_scratch.value) {
        const Id shuffled{ReadWarpValue(ctx, PublishWarpValue(ctx, value), src_thread_id)};
        return ctx.OpSelect(ctx.U32[1], in_range, shuffled, value);
    }
    if (ctx.HasScalarWarp()) {
        return ctx.OpSelect(ctx.U32[1], in_range, ReadScalarWarp(ctx, value, src_thread_id), value);
    }
    const Id shuffled{
        HasNativeShuffle(ctx)
            ? ctx.OpGroupNonUniformShuffleXor(
                  ctx.U32[1], SubgroupScope(ctx, Capability::GroupNonUniformShuffle), value, mask)
            : ShuffleAbsolute(ctx, value, HostThreadId(ctx, src_thread_id))};
    return ctx.OpSelect(ctx.U32[1], in_range, shuffled, value);
}

Id EmitQuadBroadcast(EmitContext& ctx, Id value, Id lane) {
    ValidateQuadShuffles(ctx);
    if (ctx.SupportsQuadShuffles()) {
        return ctx.OpGroupNonUniformQuadBroadcast(
            ctx.U32[1], SubgroupScope(ctx, Capability::GroupNonUniformQuad), value, lane);
    }
    const Id base{ctx.OpBitwiseAnd(ctx.U32[1], GetThreadId(ctx), ctx.Const(~3u))};
    const Id local_lane{ctx.OpBitwiseAnd(ctx.U32[1], lane, ctx.Const(3u))};
    const Id src_thread_id{ctx.OpBitwiseOr(ctx.U32[1], base, local_lane)};
    return ShuffleAbsolute(ctx, value, src_thread_id);
}

Id EmitQuadSwap(EmitContext& ctx, Id value, Id direction) {
    ValidateQuadShuffles(ctx);
    if (ctx.SupportsQuadShuffles()) {
        const Id result{ctx.OpGroupNonUniformQuadBroadcast(
            ctx.U32[1], SubgroupScope(ctx, Capability::GroupNonUniformQuad), value, direction)};
        ctx.opcode_rewrites.emplace_back(result.value, spv::Op::OpGroupNonUniformQuadSwap);
        return result;
    }
    const Id xor_mask{ctx.OpIAdd(ctx.U32[1], direction, ctx.Const(1u))};
    if (HasNativeShuffle(ctx)) {
        return ctx.OpGroupNonUniformShuffleXor(
            ctx.U32[1], SubgroupScope(ctx, Capability::GroupNonUniformShuffle), value, xor_mask);
    }
    const Id lane{ctx.OpBitwiseXor(ctx.U32[1], GetThreadId(ctx), xor_mask)};
    return ShuffleAbsolute(ctx, value, lane);
}

Id EmitFSwizzleAdd(EmitContext& ctx, Id op_a, Id op_b, Id swizzle) {
    const Id three{ctx.Const(3U)};
    Id mask{GetThreadId(ctx)};
    mask = ctx.OpBitwiseAnd(ctx.U32[1], mask, three);
    mask = ctx.OpShiftLeftLogical(ctx.U32[1], mask, ctx.Const(1U));
    mask = ctx.OpShiftRightLogical(ctx.U32[1], swizzle, mask);
    mask = ctx.OpBitwiseAnd(ctx.U32[1], mask, three);

    const Id modifier_a{ctx.OpVectorExtractDynamic(ctx.F32[1], ctx.fswzadd_lut_a, mask)};
    const Id modifier_b{ctx.OpVectorExtractDynamic(ctx.F32[1], ctx.fswzadd_lut_b, mask)};

    const Id result_a{ctx.OpFMul(ctx.F32[1], op_a, modifier_a)};
    const Id result_b{ctx.OpFMul(ctx.F32[1], op_b, modifier_b)};
    return ctx.OpFAdd(ctx.F32[1], result_a, result_b);
}

Id EmitDPdxFine(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdxFine(ctx.F32[1], op_a);
}

Id EmitDPdyFine(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdyFine(ctx.F32[1], op_a);
}

Id EmitDPdxCoarse(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdxCoarse(ctx.F32[1], op_a);
}

Id EmitDPdyCoarse(EmitContext& ctx, Id op_a) {
    return ctx.OpDPdyCoarse(ctx.F32[1], op_a);
}

} // namespace Shader::Backend::SPIRV
