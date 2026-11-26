// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>

#include "shader_recompiler/frontend/ampere/translate/impl/impl.h"
#include "shader_recompiler/frontend/maxwell/translate/impl/common_funcs.h"

namespace Shader::Ampere {
namespace {

[[nodiscard]] IR::U32 SafeShiftLeft32(IR::IREmitter& ir, const IR::U32& base, const IR::U32& shift,
                                      bool wrap) {
    if (wrap) {
        return ir.ShiftLeftLogical(base, ir.BitwiseAnd(shift, ir.Imm32(31U)));
    }
    const IR::U1 is_safe = ir.ILessThan(shift, ir.Imm32(32U), false);
    return IR::U32{ir.Select(is_safe, ir.ShiftLeftLogical(base, shift), ir.Imm32(0U))};
}

[[nodiscard]] IR::U32 SafeShiftRight32(IR::IREmitter& ir, const IR::U32& base, const IR::U32& shift,
                                       bool is_signed, bool wrap) {
    const IR::U32 safe_shift = wrap ? ir.BitwiseAnd(shift, ir.Imm32(31U)) : shift;
    const IR::U32 shifted = is_signed ? IR::U32{ir.ShiftRightArithmetic(base, safe_shift)}
                                      : IR::U32{ir.ShiftRightLogical(base, safe_shift)};
    if (wrap) {
        return shifted;
    }
    const IR::U1 is_safe = ir.ILessThan(shift, ir.Imm32(32U), false);
    const IR::U32 clamped =
        is_signed ? IR::U32{ir.ShiftRightArithmetic(base, ir.Imm32(31U))} : ir.Imm32(0U);
    return IR::U32{ir.Select(is_safe, shifted, clamped)};
}

} // Anonymous namespace

void TranslatorVisitor::LOP3(const Instruction& insn) {
    const u32 opcode = insn.Bits(0, 12);
    const bool is_uniform = opcode == 0x292 || opcode == 0x892;
    const u32 dest = insn.Bits(16, 8);
    const u32 lut = insn.Bits(72, 8);
    const u32 pred_target = insn.Bits(81, 3);

    const IR::U32 a = is_uniform ? UR(insn.Bits(24, 8)) : R(insn.Bits(24, 8));
    const IR::U32 c = is_uniform ? UR(insn.Bits(64, 8)) : R(insn.Bits(64, 8));
    IR::U32 b;
    if (opcode == 0x812 || opcode == 0x892) {
        b = ir.Imm32(insn.words[1]);
    } else if (opcode == 0x292) {
        b = UR(insn.Bits(32, 8));
    } else {
        b = AluSourceU32(insn, 1);
    }

    const IR::U32 res = Maxwell::ApplyLUT(ir, a, b, c, lut);
    if (is_uniform) {
        UR(dest, res);
        SetUP(pred_target, ir.INotEqual(res, ir.Imm32(0U)));
    } else {
        R(dest, res);
        SetP(pred_target, ir.INotEqual(res, ir.Imm32(0U)));
    }
}

void TranslatorVisitor::SHF(const Instruction& insn) {
    const u32 opcode = insn.Bits(0, 12);
    const bool is_uniform = opcode == 0x299 || opcode == 0x499 || opcode == 0x899;
    const u32 zero_reg = is_uniform ? 63U : 255U;
    const u32 dest = insn.Bits(16, 8);
    const auto read_reg = [&](u32 reg) { return is_uniform ? UR(reg) : R(reg); };
    const auto write_reg = [&](u32 reg, const IR::U32& val) {
        is_uniform ? UR(reg, val) : R(reg, val);
    };

    const bool wrap = insn.Bits(75, 1) != 0;
    if (opcode == 0x419 || opcode == 0x499) {
        const IR::U32 shift = read_reg(insn.Bits(64, 8));
        write_reg(dest, SafeShiftLeft32(ir, ir.Imm32(insn.words[1]), shift, wrap));
        return;
    }

    const bool is_imm = opcode == 0x819 || opcode == 0x899;
    const u32 low_reg = insn.Bits(24, 8);
    const u32 high_reg = insn.Bits(64, 8);
    const bool right = insn.Bits(76, 1) != 0;
    const bool high_result = insn.Bits(80, 1) != 0;
    const u32 data_type = insn.Bits(73, 2); // 1=U64, 2=S64, 3=U32
    const bool is_signed = data_type == 2U;

    const IR::U32 low_val = read_reg(low_reg);
    const IR::U32 high_val = read_reg(high_reg);
    const IR::U32 shift = is_imm            ? ir.Imm32(insn.words[1])
                          : opcode == 0xa19 ? ReadCbuf32(insn)
                          : opcode == 0xc19 ? UR(insn.Bits(32, 8))
                                            : read_reg(insn.Bits(32, 8));

    // Fast 32-bit single-word paths when the unused half is RZ/URZ
    if (data_type == 3U && !high_result && high_reg == zero_reg) {
        write_reg(dest, right ? SafeShiftRight32(ir, low_val, shift, false, wrap)
                              : SafeShiftLeft32(ir, low_val, shift, wrap));
        return;
    }
    if ((data_type == 3U || is_signed) && high_result && right && low_reg == zero_reg) {
        write_reg(dest, SafeShiftRight32(ir, high_val, shift, is_signed, wrap));
        return;
    }
    if (!high_result && right && high_reg == zero_reg) {
        write_reg(dest, SafeShiftRight32(ir, low_val, shift, false, wrap));
        return;
    }

    // General 64-bit funnel shift over (high:low)
    const IR::U64 packed = ir.PackUint2x32(ir.CompositeConstruct(low_val, high_val));
    const IR::U32 max_shift = data_type == 3U ? ir.Imm32(32U) : ir.Imm32(63U);
    const IR::U32 safe_shift =
        wrap ? ir.BitwiseAnd(shift, ir.ISub(max_shift, ir.Imm32(1U))) : ir.UMin(shift, max_shift);
    const IR::U64 shifted = !right      ? ir.ShiftLeftLogical(packed, safe_shift)
                            : is_signed ? ir.ShiftRightArithmetic(packed, safe_shift)
                                        : ir.ShiftRightLogical(packed, safe_shift);
    const IR::Value unpacked = ir.UnpackUint2x32(shifted);
    write_reg(dest, IR::U32{ir.CompositeExtract(unpacked, high_result ? 1U : 0U)});
}

void TranslatorVisitor::SGXT(const Instruction& insn) {
    const u32 dest = insn.Bits(16, 8);
    const u32 count = insn.words[1];
    const bool is_signed = insn.Bits(73, 1) != 0;
    const IR::U32 src = R(insn.Bits(24, 8));
    if (count == 0U) {
        R(dest, ir.Imm32(0U));
    } else if (count >= 32U) {
        R(dest, src);
    } else {
        R(dest, ir.BitFieldExtract(src, ir.Imm32(0U), ir.Imm32(count), is_signed));
    }
}

void TranslatorVisitor::PRMT(const Instruction& insn) {
    const bool is_uniform = insn.Bits(7, 1) != 0;
    const u32 dest = insn.Bits(16, 8);
    const u32 form = insn.Bits(9, 3);
    const u32 mode = insn.Bits(72, 3);
    const IR::U32 a = is_uniform ? UR(insn.Bits(24, 8)) : R(insn.Bits(24, 8));
    const IR::U32 c = !is_uniform ? AluSourceU32(insn, 2)
                      : form == 2 ? ir.Imm32(insn.words[1])
                                  : UR(insn.Bits(64, 8));
    const u32 control_field = form == 2 ? 64U : 32U;
    IR::U32 control = !is_uniform ? AluSourceU32(insn, 1)
                      : form == 4 ? ir.Imm32(insn.words[1])
                                  : UR(insn.Bits(control_field, 8));
    if (mode != 0U) {
        // PTX prmt selector table, packed in destination-byte order (low nibble first).
        constexpr std::array<std::array<u32, 4>, 6> modes{{
            {0x3210, 0x4321, 0x5432, 0x6543}, // F4E
            {0x5670, 0x6701, 0x7012, 0x0123}, // B4E
            {0x0000, 0x1111, 0x2222, 0x3333}, // RC8
            {0x3210, 0x3211, 0x3222, 0x3333}, // ECL
            {0x0000, 0x1110, 0x2210, 0x3210}, // ECR
            {0x1010, 0x3232, 0x1010, 0x3232}, // RC16
        }};
        const auto& selections = modes[mode - 1];
        if (control.IsImmediate()) {
            control = ir.Imm32(selections[control.U32() & 3U]);
        } else {
            const IR::U32 selector = ir.BitwiseAnd(control, ir.Imm32(3U));
            const IR::U32 low{ir.Select(ir.IEqual(selector, ir.Imm32(0U)), ir.Imm32(selections[0]),
                                        ir.Imm32(selections[1]))};
            const IR::U32 high{ir.Select(ir.IEqual(selector, ir.Imm32(2U)), ir.Imm32(selections[2]),
                                         ir.Imm32(selections[3]))};
            control = IR::U32{ir.Select(ir.ILessThan(selector, ir.Imm32(2U), false), low, high)};
        }
    }
    const auto write_dst = [&](const IR::U32& val) { is_uniform ? UR(dest, val) : R(dest, val); };

    if (!control.IsImmediate()) {
        IR::U32 result = ir.Imm32(0U);
        for (u32 i = 0; i < 4U; ++i) {
            const IR::U32 nib = ir.BitFieldExtract(control, ir.Imm32(i * 4U), ir.Imm32(4U), false);
            IR::U32 source = a;
            if (mode <= 2U) {
                const IR::U1 from_high =
                    ir.INotEqual(ir.BitwiseAnd(nib, ir.Imm32(4U)), ir.Imm32(0U));
                source = IR::U32{ir.Select(from_high, c, a)};
            }
            const IR::U32 pos =
                ir.ShiftLeftLogical(ir.BitwiseAnd(nib, ir.Imm32(3U)), ir.Imm32(3U));
            IR::U32 byte = ir.BitFieldExtract(source, pos, ir.Imm32(8U), false);
            if (mode == 0U) {
                const IR::U1 spread = ir.INotEqual(ir.BitwiseAnd(nib, ir.Imm32(8U)), ir.Imm32(0U));
                const IR::U32 sign = ir.BitFieldExtract(byte, ir.Imm32(7U), ir.Imm32(1U), true);
                byte = IR::U32{ir.Select(spread, sign, byte)};
            }
            result = ir.BitFieldInsert(result, byte, ir.Imm32(i * 8U), ir.Imm32(8U));
        }
        write_dst(result);
        return;
    }

    const u32 sel = control.U32() & 0xffffU;
    if (sel == 0x5410U) {
        write_dst(ir.BitFieldInsert(a, c, ir.Imm32(16U), ir.Imm32(16U)));
        return;
    }
    if (sel == 0x5432U) {
        write_dst(ir.BitFieldInsert(ir.ShiftRightLogical(a, ir.Imm32(16U)), c, ir.Imm32(16U),
                                    ir.Imm32(16U)));
        return;
    }
    if (sel == 0x7610U) {
        write_dst(ir.BitFieldInsert(c, a, ir.Imm32(0U), ir.Imm32(16U)));
        return;
    }
    if (sel == 0x7632U) {
        write_dst(ir.BitFieldInsert(c, ir.ShiftRightLogical(a, ir.Imm32(16U)), ir.Imm32(0U),
                                    ir.Imm32(16U)));
        return;
    }

    IR::U32 result = ir.Imm32(0U);
    for (u32 i = 0; i < 4U; ++i) {
        const u32 nib = (sel >> (i * 4U)) & 0xfU;
        const IR::U32 source = (nib & 4U) != 0 ? c : a;
        const u32 pos = (nib & 3U) * 8U;
        const IR::U32 byte_val =
            (nib & 8U) != 0
                ? ir.BitFieldExtract(source, ir.Imm32(pos + 7U), ir.Imm32(1U), true)
                : ir.BitFieldExtract(source, ir.Imm32(pos), ir.Imm32(8U), false);
        result = ir.BitFieldInsert(result, byte_val, ir.Imm32(i * 8U), ir.Imm32(8U));
    }
    write_dst(result);
}

void TranslatorVisitor::FLO(const Instruction& insn) {
    const u32 opcode = insn.Bits(0, 12);
    const bool is_uniform = opcode == 0x2bd;
    const u32 dest = insn.Bits(16, 8);
    IR::U32 src = is_uniform ? UR(insn.Bits(32, 8)) : AluSourceU32(insn, 1);
    if (!is_uniform && insn.Bits(63, 1)) {
        src = ir.BitwiseNot(src);
    }
    const bool is_signed = !is_uniform && insn.Bits(73, 1);
    IR::U32 res = is_signed ? ir.FindSMsb(src) : ir.FindUMsb(src);
    if (insn.Bits(74, 1)) {
        const IR::U1 not_found = ir.IEqual(res, ir.Imm32(-1));
        res = IR::U32{ir.Select(not_found, res, ir.BitwiseXor(res, ir.Imm32(31U)))};
    }
    is_uniform ? UR(dest, res) : R(dest, res);
}

void TranslatorVisitor::POPC(const Instruction& insn) {
    const u32 opcode = insn.Bits(0, 12);
    const bool is_uniform = opcode == 0x2bf;
    const u32 dest = insn.Bits(16, 8);
    IR::U32 src = is_uniform ? UR(insn.Bits(32, 8)) : AluSourceU32(insn, 1);
    if (insn.Bits(63, 1)) {
        src = ir.BitwiseNot(src);
    }
    const IR::U32 count = ir.BitCount(src);
    is_uniform ? UR(dest, count) : R(dest, count);
}

void TranslatorVisitor::BREV(const Instruction& insn) {
    const u32 opcode = insn.Bits(0, 12);
    const bool is_uniform = opcode == 0x2be;
    const u32 dest = insn.Bits(16, 8);
    const IR::U32 src = is_uniform ? UR(insn.Bits(32, 8)) : R(insn.words[1]);
    const IR::U32 rev = ir.BitReverse(src);
    is_uniform ? UR(dest, rev) : R(dest, rev);
}

} // namespace Shader::Ampere
