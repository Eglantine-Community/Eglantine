// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

void EmitBitCastU16F16(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBitCastU32F32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.U32[1], value);
}

void EmitBitCastU64F64(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

void EmitBitCastF16U16(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitBitCastF32U32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.F32[1], value);
}

void EmitBitCastF64U64(EmitContext&) {
    throw NotImplementedException("SPIR-V Instruction");
}

Id EmitPackUint2x32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.U64, value);
}

Id EmitUnpackUint2x32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.U32[2], value);
}

Id EmitPackFloat2x16(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.U32[1], value);
}

Id EmitUnpackFloat2x16(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.F16[2], value);
}

Id EmitPackHalf2x16(EmitContext& ctx, Id value) {
    const Id u32_type{ctx.U32[1]};
    const Id halves{Float32ToHalfBitsRTE(ctx, value, 2)};
    const Id lo{ctx.OpCompositeExtract(u32_type, halves, 0)};
    const Id hi{ctx.OpCompositeExtract(u32_type, halves, 1)};
    return ctx.OpBitwiseOr(u32_type, lo,
                           ctx.OpShiftLeftLogical(u32_type, hi, ctx.Const(16U)));
}

Id EmitUnpackHalf2x16(EmitContext& ctx, Id value) {
    return ctx.OpUnpackHalf2x16(ctx.F32[2], value);
}

Id EmitPackDouble2x32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.F64[1], value);
}

Id EmitUnpackDouble2x32(EmitContext& ctx, Id value) {
    return ctx.OpBitcast(ctx.U32[2], value);
}

} // namespace Shader::Backend::SPIRV
