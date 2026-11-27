// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/frontend/ir/modifiers.h"

namespace Shader::Backend::SPIRV {
namespace {
Id ExtractU16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U16, value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

Id ExtractS16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.S16, value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

Id ExtractU8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpUConvert(ctx.U8, value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}

Id ExtractS8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpSConvert(ctx.S8, value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}
} // Anonymous namespace

Id EmitConvertS16F16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS16F32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS16F64(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], ctx.OpConvertFToS(ctx.U16, value));
    } else {
        return ExtractS16(ctx, ctx.OpConvertFToS(ctx.U32[1], value));
    }
}

Id EmitConvertS32F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS32F32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        return ctx.OpBitcast(ctx.U32[1], ctx.OpConvertFToS(ctx.S32[1], value));
    } else {
        return ctx.OpConvertFToS(ctx.U32[1], value);
    }
}

Id EmitConvertS32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U32[1], value);
}

Id EmitConvertS64F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertS64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertS64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToS(ctx.U64, value);
}

Id EmitConvertU16F16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU16F32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU16F64(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], ctx.OpConvertFToU(ctx.U16, value));
    } else {
        return ExtractU16(ctx, ctx.OpConvertFToU(ctx.U32[1], value));
    }
}

Id EmitConvertU32F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU32F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU32F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U32[1], value);
}

Id EmitConvertU64F16(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64F32(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64F64(EmitContext& ctx, Id value) {
    return ctx.OpConvertFToU(ctx.U64, value);
}

Id EmitConvertU64U32(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U64, value);
}

Id EmitConvertU32U64(EmitContext& ctx, Id value) {
    return ctx.OpUConvert(ctx.U32[1], value);
}

Id Float32ToHalfBitsRTE(EmitContext& ctx, Id value, u32 components) {
    const Id integer{ctx.U32[components]};
    const Id boolean{components == 1 ? ctx.U1 : ctx.TypeVector(ctx.U1, 2)};
    const auto constant = [&](u32 x) {
        return components == 1 ? ctx.Const(x) : ctx.Const(x, x);
    };
    const Id one{constant(1U)};
    const Id bits{ctx.OpBitcast(integer, value)};
    const Id magnitude{ctx.OpBitwiseAnd(integer, bits, constant(0x7fffffffU))};
    const Id sign{ctx.OpBitwiseAnd(integer,
                                  ctx.OpShiftRightLogical(integer, bits, constant(16U)),
                                  constant(0x8000U))};

    const Id exponent{ctx.OpShiftRightLogical(integer, magnitude, constant(23U))};
    const Id significand{ctx.OpBitwiseOr(integer, ctx.OpBitwiseAnd(integer, magnitude, constant(0x7fffffU)),
                                        constant(0x800000U))};

    // Normals retain 11 significand bits. Subnormals shift further. At 25 bits even the
    // largest significand rounds to zero, including binary32 subnormal inputs.
    const Id min_exp{constant(101U)};
    const Id normal_exp{constant(113U)};
    const Id bounded_exp{ctx.profile.has_broken_spirv_clamp
                             ? ctx.OpUMin(integer, ctx.OpUMax(integer, exponent, min_exp), normal_exp)
                             : ctx.OpUClamp(integer, exponent, min_exp, normal_exp)};
    const Id shift{ctx.OpISub(integer, constant(126U), bounded_exp)};
    const Id half_ulp{ctx.OpShiftLeftLogical(integer, one, ctx.OpISub(integer, shift, one))};
    const Id odd{ctx.OpBitwiseAnd(integer, ctx.OpShiftRightLogical(integer, significand, shift), one)};
    const Id bias{ctx.OpIAdd(integer, ctx.OpISub(integer, half_ulp, one), odd)};
    const Id rounded{ctx.OpShiftRightLogical(integer, ctx.OpIAdd(integer, significand, bias), shift)};

    // The leading bit of a rounded normal supplies one unit of the binary16 exponent.
    const Id excess{ctx.OpISub(integer, ctx.OpUMax(integer, exponent, normal_exp), normal_exp)};
    const Id biased{ctx.OpIAdd(integer, ctx.OpShiftLeftLogical(integer, excess, constant(10U)), rounded)};
    const Id saturated{ctx.OpUMin(integer, biased, constant(0x7c00U))};
    const Id unsigned_half{ctx.OpSelect(integer,
        ctx.OpUGreaterThan(boolean, magnitude, constant(0x7f800000U)), constant(0x7e00U), saturated)};
    return ctx.OpBitwiseOr(integer, sign, unsigned_half);
}

Id EmitConvertF16F32(EmitContext& ctx, IR::Inst* inst, Id value) {
    if (inst->Flags<IR::FpControl>().rounding != IR::FpRounding::RN) {
        return ctx.OpFConvert(ctx.F16[1], value);
    }
    if (ctx.UsesNativeFloat16RTE()) {
        // Every float32 subnormal rounds to half zero. Pass signed zero for these inputs
        // so float32 denorm flushing cannot change the result without a float32 mode.
        const Id integer{ctx.U32[1]};
        const Id raw{ctx.OpBitcast(integer, value)};
        const Id magnitude{ctx.OpBitwiseAnd(integer, raw, ctx.Const(0x7fffffffU))};
        const Id small{ctx.OpULessThan(ctx.U1, magnitude, ctx.Const(0x00800000U))};
        const Id signed_zero{ctx.OpBitwiseAnd(integer, raw, ctx.Const(0x80000000U))};
        const Id input{ctx.OpSelect(integer, small, signed_zero, raw)};
        return ctx.OpFConvert(ctx.F16[1], ctx.OpBitcast(ctx.F32[1], input));
    }
    // Vulkan's implicit conversion rounding does not guarantee RN. A two-half bitcast
    // materializes the exact value without requiring the optional Int16 capability.
    const Id bits{Float32ToHalfBitsRTE(ctx, value, 1)};
    const Id halves{ctx.OpBitcast(ctx.F16[2], bits)};
    return ctx.OpCompositeExtract(ctx.F16[1], halves, 0);
}

Id EmitConvertF32F16(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F32[1], value);
}

Id EmitConvertF32F64(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F32[1], value);
}

Id EmitConvertF64F32(EmitContext& ctx, Id value) {
    return ctx.OpFConvert(ctx.F64[1], value);
}

Id EmitConvertF16S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], ExtractS8(ctx, value));
}

Id EmitConvertF16S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], ExtractS16(ctx, value));
}

Id EmitConvertF16S32(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], value);
}

Id EmitConvertF16S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F16[1], value);
}

Id EmitConvertF16U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], ExtractU8(ctx, value));
}

Id EmitConvertF16U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], ExtractU16(ctx, value));
}

Id EmitConvertF16U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], value);
}

Id EmitConvertF16U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F16[1], value);
}

Id EmitConvertF32S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS8(ctx, value));
}

Id EmitConvertF32S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], ExtractS16(ctx, value));
}

Id EmitConvertF32S32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        value = ctx.OpBitcast(ctx.S32[1], value);
    }
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F32[1], value);
}

Id EmitConvertF32U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU8(ctx, value));
}

Id EmitConvertF32U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], ExtractU16(ctx, value));
}

Id EmitConvertF32U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF32U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F32[1], value);
}

Id EmitConvertF64S8(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS8(ctx, value));
}

Id EmitConvertF64S16(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], ExtractS16(ctx, value));
}

Id EmitConvertF64S32(EmitContext& ctx, Id value) {
    if (ctx.profile.has_broken_signed_operations) {
        value = ctx.OpBitcast(ctx.S32[1], value);
    }
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64S64(EmitContext& ctx, Id value) {
    return ctx.OpConvertSToF(ctx.F64[1], value);
}

Id EmitConvertF64U8(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU8(ctx, value));
}

Id EmitConvertF64U16(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], ExtractU16(ctx, value));
}

Id EmitConvertF64U32(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

Id EmitConvertF64U64(EmitContext& ctx, Id value) {
    return ctx.OpConvertUToF(ctx.F64[1], value);
}

Id EmitConvertU16U32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U16, value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

Id EmitConvertU32U16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpUConvert(ctx.U32[1], value);
    } else {
        return ExtractU16(ctx, value);
    }
}

Id EmitConvertU8U32(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpUConvert(ctx.U8, value);
    } else {
        return ExtractU8(ctx, value);
    }
}

Id EmitConvertU32U8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpUConvert(ctx.U32[1], value);
    } else {
        return ctx.OpBitFieldUExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}

// in signed
Id EmitConvertS32S8(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int8) {
        return ctx.OpSConvert(ctx.U32[1], value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(8u));
    }
}

Id EmitConvertS32S16(EmitContext& ctx, Id value) {
    if (ctx.profile.support_int16) {
        return ctx.OpSConvert(ctx.U32[1], value);
    } else {
        return ctx.OpBitFieldSExtract(ctx.U32[1], value, ctx.u32_zero_value, ctx.Const(16u));
    }
}

} // namespace Shader::Backend::SPIRV
