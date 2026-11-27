// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <iterator>
#include <optional>
#include <vector>

#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/program.h"

namespace Shader::Backend::SPIRV {
namespace {

constexpr u32 GUEST_WARP_LANES = 32;

struct WarpScratch {
    Id words;
    Id pointer;
    u32 threads;

    Id At(EmitContext& ctx, Id lane, Id word) const {
        const Id index{ctx.OpIAdd(ctx.U32[1], lane,
                                 ctx.OpIMul(ctx.U32[1], word, ctx.Const(threads)))};
        if (ctx.uses_explicit_workgroup_layout) {
            return ctx.OpAccessChain(pointer, words, ctx.Const(1u), index);
        }
        return ctx.OpAccessChain(pointer, words, index);
    }
};

bool HasSingleWorkgroupPath(const IR::Program& program) {
    using Type = IR::AbstractSyntaxNode::Type;
    const auto& syntax = program.syntax_list;
    if (syntax.size() < 2 || syntax.back().type != Type::Return ||
        program.info.removed_conditional_barrier) {
        return false;
    }
    return std::all_of(syntax.begin(), std::prev(syntax.end()),
                       [](const auto& node) { return node.type == Type::Block; });
}

void VerifyWorkgroupWarpProgram(const IR::Program& program) {
    if (program.stage != Stage::Compute || !HasSingleWorkgroupPath(program)) {
        throw NotImplementedException("Guest warp scratch requires convergent straight-line compute");
    }
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::DemoteToHelperInvocation:
            case IR::Opcode::QuadBroadcast:
            case IR::Opcode::QuadSwap:
            case IR::Opcode::FSwizzleAdd:
                throw NotImplementedException("Unsupported collective in guest warp scratch mode");
            default:
                break;
            }
        }
    }
}

WarpScratch DefineWarpScratch(EmitContext& ctx, const IR::Program& program, u32 words_per_thread,
                              u32 budget, const char* name, bool requires_complete_warps) {
    const auto [x, y, z] = program.workgroup_size;
    const u32 max_threads = budget / (words_per_thread * sizeof(u32));
    if (x == 0 || y == 0 || z == 0 || x > max_threads / y / z) {
        throw NotImplementedException("Guest warp scratch exceeds the shared memory limit");
    }
    const u32 threads = x * y * z;
    if (requires_complete_warps && threads % GUEST_WARP_LANES != 0) {
        throw NotImplementedException("Workgroup HMMA requires complete guest warps");
    }
    const u64 alignment = ctx.uses_explicit_workgroup_layout ? 16 : 4;
    const u64 guest_bytes = (u64{program.shared_memory_size} + alignment - 1) & ~(alignment - 1);
    const u64 scratch_offset = ctx.uses_explicit_workgroup_layout ? (std::max)(guest_bytes, alignment) : 0;
    if ((std::max)(guest_bytes, scratch_offset) + u64{threads} * words_per_thread * sizeof(u32) > budget) {
        throw NotImplementedException("Guest warp scratch exceeds the shared memory limit");
    }
    const Id u32_type = ctx.U32[1];
    const Id array = ctx.TypeArray(u32_type, ctx.Const(threads * words_per_thread));
    Id type = array;
    const bool explicit_layout = ctx.uses_explicit_workgroup_layout;
    if (explicit_layout) {
        ctx.AddExtension("SPV_KHR_workgroup_memory_explicit_layout");
        ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayoutKHR);
        ctx.Decorate(array, spv::Decoration::ArrayStride, 4u);
        // An unused first member makes this type distinct from the guest shared blocks.
        type = ctx.TypeStruct(u32_type, array);
        ctx.Decorate(type, spv::Decoration::Block);
        ctx.MemberDecorate(type, 0, spv::Decoration::Offset, 0u);
        ctx.MemberDecorate(type, 1, spv::Decoration::Offset, static_cast<u32>(scratch_offset));
    }
    const Id words = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Workgroup, type),
                                          spv::StorageClass::Workgroup);
    if (explicit_layout) {
        ctx.Decorate(words, spv::Decoration::Aliased);
    }
    if (ctx.profile.supported_spirv >= 0x00010400) {
        ctx.interfaces.push_back(words);
    }
    ctx.Name(words, name);
    return {words, ctx.TypePointer(spv::StorageClass::Workgroup, u32_type), threads};
}

// A cooperative matrix needs its whole subgroup: every row of the workgroup fills whole warps and
// the pipeline is created with full subgroups. Cooperative matrices also need the Vulkan memory
// model, which has no Coherent decoration, so a program that needs coherent memory keeps the
// emulation.
bool SupportsCooperativeHmma(const IR::Program& program) {
    return program.stage == Stage::Compute && program.workgroup_size[0] != 0 &&
           program.workgroup_size[0] % GUEST_WARP_LANES == 0 &&
           !program.info.requires_coherent_global_memory &&
           !program.info.requires_coherent_image_memory;
}

// Sirit lacks the two cooperative matrix instructions. They are emitted as an OpTypeStruct and an
// OpSelect, which have the same word layout, and PatchCooperativeHmma replaces the opcodes after
// assembly. The operands are constants that no real structure has as members.
CooperativeHmmaTypes DefineCooperativeHmmaTypes(Sirit::Module& module, Id u32_type,
                                                u32 spirv_version) {
    module.AddExtension("SPV_KHR_cooperative_matrix");
    module.AddCapability(spv::Capability::CooperativeMatrixKHR);
    module.AddCapability(spv::Capability::Float16);
    if (spirv_version < 0x00010500) {
        module.AddExtension("SPV_KHR_vulkan_memory_model");
    }
    module.AddCapability(spv::Capability::VulkanMemoryModel);
    module.AddCapability(spv::Capability::VulkanMemoryModelDeviceScope);
    module.SetMemoryModel(spv::AddressingModel::Logical, spv::MemoryModel::Vulkan);
    const Id float16{module.TypeFloat(16)};
    const auto constant{[&](u32 value) { return module.Constant(u32_type, value); }};
    const Id scope{constant(static_cast<u32>(spv::Scope::Subgroup))};
    const auto matrix{[&](u32 columns, spv::CooperativeMatrixUse use) {
        return module.TypeStruct(float16, scope, constant(16), constant(columns),
                                 constant(static_cast<u32>(use)));
    }};
    return CooperativeHmmaTypes{
        .float16 = float16,
        .float16x2 = module.TypeVector(float16, 2),
        .matrix_a = matrix(16, spv::CooperativeMatrixUse::MatrixAKHR),
        .matrix_b = matrix(8, spv::CooperativeMatrixUse::MatrixBKHR),
        .matrix_c = matrix(8, spv::CooperativeMatrixUse::MatrixAccumulatorKHR),
    };
}

// HMMA.16816.F16 on a cooperative matrix. Each lane already holds its share of the matrices in
// registers, so no data moves between lanes or through memory: the lane's halves become the
// matrix's local elements. Vulkan leaves their order to the driver; NVIDIA's is the guest's except
// that A's middle registers swap (see the self-test in vk_cooperative_hmma_test.cpp).
Id EmitCooperativeHmma(Sirit::Module& module, const CooperativeHmmaTypes& types, Id u32_type,
                       Id u32x2_type, Id a, Id b, Id c) {
    const auto insert_word{[&](Id matrix_type, Id matrix, Id vector, u32 word, u32 slot) {
        const Id halves{module.OpBitcast(types.float16x2,
                                         module.OpCompositeExtract(u32_type, vector, word))};
        for (u32 half = 0; half < 2; ++half) {
            matrix = module.OpCompositeInsert(
                matrix_type, module.OpCompositeExtract(types.float16, halves, half), matrix,
                slot * 2 + half);
        }
        return matrix;
    }};
    constexpr std::array<u32, 4> a_words{0, 2, 1, 3};
    Id matrix_a{module.OpUndef(types.matrix_a)};
    for (u32 slot = 0; slot < a_words.size(); ++slot) {
        matrix_a = insert_word(types.matrix_a, matrix_a, a, a_words[slot], slot);
    }
    Id matrix_b{module.OpUndef(types.matrix_b)};
    Id matrix_c{module.OpUndef(types.matrix_c)};
    for (u32 slot = 0; slot < 2; ++slot) {
        matrix_b = insert_word(types.matrix_b, matrix_b, b, slot, slot);
        matrix_c = insert_word(types.matrix_c, matrix_c, c, slot, slot);
    }
    // OpCooperativeMatrixMulAddKHR after PatchCooperativeHmma.
    const Id matrix_d{module.OpSelect(types.matrix_c, matrix_a, matrix_b, matrix_c)};
    const auto extract_word{[&](u32 word) {
        return module.OpBitcast(
            u32_type,
            module.OpCompositeConstruct(
                types.float16x2, module.OpCompositeExtract(types.float16, matrix_d, word * 2),
                module.OpCompositeExtract(types.float16, matrix_d, word * 2 + 1)));
    }};
    return module.OpCompositeConstruct(u32x2_type, extract_word(0), extract_word(1));
}

} // Anonymous namespace

void DefineSoftwareMatrixFunctions(EmitContext& ctx, const IR::Program& program) {
    const u32 workgroup_budget = ctx.runtime_info.warp_scratch_memory_limit;
    const bool cooperative{workgroup_budget == 0 && ctx.profile.support_cooperative_hmma &&
                           SupportsCooperativeHmma(program)};
    std::array<bool, 2> needed{};
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::TensorMulAdd || inst.Flags<u32>() == 0) continue;
            if (ctx.HasScalarWarp()) {
                throw NotImplementedException("HMMA requires a complete guest warp");
            }
            const u32 depth = inst.Flags<u32>();
            if (depth != 8 && depth != 16) throw LogicError("Invalid software HMMA depth");
            if (depth == 16 && cooperative) {
                if (!ctx.cooperative_hmma) {
                    ctx.cooperative_hmma =
                        DefineCooperativeHmmaTypes(ctx, ctx.U32[1], ctx.profile.supported_spirv);
                }
                continue;
            }
            needed[depth == 16] = true;
        }
    }
    std::optional<WarpScratch> scratch;
    if (workgroup_budget != 0 && !ctx.HasScalarWarp()) {
        const bool matrix = needed[0] || needed[1];
        VerifyWorkgroupWarpProgram(program);
        if (matrix) {
            scratch = DefineWarpScratch(ctx, program, needed[1] ? 6 : 3, workgroup_budget,
                                        "hmma_scratch", true);
        }
        if (program.info.UsesWarpCollectives()) {
            const auto warp_scratch = scratch ? *scratch :
                DefineWarpScratch(ctx, program, 1, workgroup_budget, "guest_warp_scratch", false);
            ctx.workgroup_warp_scratch = warp_scratch.words;
            ctx.workgroup_warp_threads = warp_scratch.threads;
        }
    }
    for (u32 variant = 0; variant < 2; ++variant) {
        if (!needed[variant]) continue;
        if (!scratch) {
            ctx.AddCapability(spv::Capability::GroupNonUniform);
            ctx.AddCapability(spv::Capability::GroupNonUniformShuffle);
            if (!ctx.SupportsSubgroups() || !ctx.profile.support_shuffle ||
                ctx.profile.has_broken_spirv_subgroup_shuffle) {
                throw NotImplementedException("Software HMMA requires subgroup shuffle support");
            }
        }
        const Id u32ty = ctx.U32[1], f32ty = ctx.F32[1];
        const auto imm = [&](u32 v) { return ctx.Const(v); };
        const auto add = [&](Id a, Id b) { return ctx.OpIAdd(u32ty, a, b); };
        const auto and_ = [&](Id a, u32 b) { return ctx.OpBitwiseAnd(u32ty, a, imm(b)); };
        const auto unpack = [&](Id v) { return ctx.OpUnpackHalf2x16(ctx.F32[2], v); };
        const auto round_half = [&](Id value) {
            // Do not express the intermediate rounding as unpack(pack(value)): drivers may
            // eliminate that round trip. Round normal significands explicitly, ties to even;
            // half subnormals lie on the exactly representable 2^-24 grid.
            const Id bits = ctx.OpBitcast(u32ty, value);
            const Id magnitude = and_(bits, 0x7fffffffU);
            const Id tie = and_(ctx.OpShiftRightLogical(u32ty, magnitude, imm(13)), 1);
            const Id rounded = and_(add(add(magnitude, imm(0xfff)), tie), ~0x1fffU);
            const Id normal = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, rounded, imm(0x47800000)), imm(0x7f800000), rounded);
            const Id scaled = ctx.OpFMul(f32ty, ctx.OpBitcast(f32ty, magnitude), ctx.Const(16777216.0f));
            const Id subnormal = ctx.OpBitcast(u32ty,
                ctx.OpFMul(f32ty, ctx.OpRoundEven(f32ty, scaled), ctx.Const(0x1p-24f)));
            const Id finite = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x38800000)), normal, subnormal);
            const Id result = ctx.OpSelect(u32ty,
                ctx.OpUGreaterThanEqual(ctx.U1, magnitude, imm(0x7f800000)), magnitude, finite);
            return ctx.OpBitcast(f32ty, ctx.OpBitwiseOr(u32ty, result, and_(bits, 0x80000000)));
        };
        const Id type = ctx.TypeFunction(ctx.U32[2], ctx.U32[4], ctx.U32[2], ctx.U32[2]);
        const Id fn = ctx.OpFunction(ctx.U32[2], spv::FunctionControlMask::DontInline, type);
        ctx.software_matrix_functions[variant] = fn;
        const Id matrix_a = ctx.OpFunctionParameter(ctx.U32[4]);
        const Id matrix_b = ctx.OpFunctionParameter(ctx.U32[2]);
        const Id matrix_c = ctx.OpFunctionParameter(ctx.U32[2]);
        ctx.AddLabel();
        const Id counter = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, u32ty),
                                           spv::StorageClass::Function);
        std::array<Id, 4> accumulators;
        for (auto& acc : accumulators)
            acc = ctx.AddLocalVariable(ctx.TypePointer(spv::StorageClass::Function, f32ty),
                                 spv::StorageClass::Function);
        std::array<Id, 8> inputs;
        for (u32 i = 0; i < 4; ++i)
            inputs[i] = ctx.OpCompositeExtract(u32ty, matrix_a, i);
        for (u32 i = 0; i < 2; ++i) {
            inputs[4 + i] = ctx.OpCompositeExtract(u32ty, matrix_b, i);
            inputs[6 + i] = ctx.OpCompositeExtract(u32ty, matrix_c, i);
        }
        for (u32 row = 0; row < 2; ++row) {
            const Id c = unpack(inputs[6 + row]);
            for (u32 col = 0; col < 2; ++col)
                ctx.OpStore(accumulators[row * 2 + col], ctx.OpCompositeExtract(f32ty, c, col));
        }
        ctx.OpStore(counter, imm(0));
        const Id lane = ctx.OpLoad(u32ty, scratch ? ctx.local_invocation_index
                                                 : ctx.subgroup_local_invocation_id);
        if (scratch) {
            ctx.OpStore(scratch->At(ctx, lane, imm(0)), inputs[0]);
            ctx.OpStore(scratch->At(ctx, lane, imm(1)), inputs[1]);
            ctx.OpStore(scratch->At(ctx, lane, imm(2)), inputs[4]);
            if (variant == 1) {
                ctx.OpStore(scratch->At(ctx, lane, imm(3)), inputs[2]);
                ctx.OpStore(scratch->At(ctx, lane, imm(4)), inputs[3]);
                ctx.OpStore(scratch->At(ctx, lane, imm(5)), inputs[5]);
            }
            // Every invocation publishes its own fragment before any invocation reads it.
            EmitBarrier(ctx);
        }
        const Id warp_base = and_(lane, ~31U);
        const Id row_base = and_(lane, 28);
        const Id col_base = ctx.OpShiftLeftLogical(u32ty, and_(lane, 3), imm(3));
        const Id a_base = add(warp_base, row_base);
        const Id b_base = add(warp_base, col_base);
        // Unrolling a K=8 group lets its shuffles overlap, which latency-bound small dispatches
        // need; unrolling all of K=16 raises register pressure and slows large dispatches.
        const auto emit_group = [&](Id group, auto pick) {
            const Id first_word = scratch ? ctx.OpIMul(u32ty, group, imm(3)) : Id{};
            const auto read = [&](Id from, u32 word, Id low, Id high) {
                if (scratch) {
                    return ctx.OpLoad(u32ty, scratch->At(ctx, from, add(first_word, imm(word))));
                }
                return ctx.OpGroupNonUniformShuffle(u32ty, imm(3), pick(low, high), from);
            };
            for (u32 component = 0; component < 4; ++component) {
                std::array<Id, 2> bs;
                for (u32 col = 0; col < 2; ++col)
                    bs[col] = unpack(read(add(b_base, imm(component + col * 4)),
                                          2, inputs[4], inputs[5]));
                for (u32 row = 0; row < 2; ++row) {
                    const Id as = unpack(read(add(a_base, imm(component)), row,
                                              inputs[row], inputs[row + 2]));
                    for (u32 col = 0; col < 2; ++col) {
                        Id value = ctx.OpLoad(f32ty, accumulators[row * 2 + col]);
                        for (u32 half = 0; half < 2; ++half) {
                            value = ctx.OpFma(f32ty, ctx.OpCompositeExtract(f32ty, as, half),
                                              ctx.OpCompositeExtract(f32ty, bs[col], half), value);
                            ctx.Decorate(value, spv::Decoration::NoContraction);
                        }
                        ctx.OpStore(accumulators[row * 2 + col], value);
                    }
                }
            }
            // Match inline HMMA at each K=8 boundary. PTX leaves accumulation order and rounding unspecified.
            for (const Id accumulator : accumulators)
                ctx.OpStore(accumulator, round_half(ctx.OpLoad(f32ty, accumulator)));
        };
        if (variant == 0) {
            emit_group(imm(0), [](Id low, Id) { return low; });
        } else {
            const Id header = ctx.OpLabel(), body = ctx.OpLabel(), next = ctx.OpLabel(),
                     end = ctx.OpLabel();
            ctx.OpBranch(header);
            ctx.AddLabel(header);
            const Id group = ctx.OpLoad(u32ty, counter);
            const Id more = ctx.OpULessThan(ctx.U1, group, imm(2));
            ctx.OpLoopMerge(end, next, spv::LoopControlMask::DontUnroll);
            ctx.OpBranchConditional(more, body, end);
            ctx.AddLabel(body);
            const Id upper = ctx.OpIEqual(ctx.U1, group, imm(1));
            emit_group(group, [&](Id low, Id high) { return ctx.OpSelect(u32ty, upper, high, low); });
            ctx.OpBranch(next);
            ctx.AddLabel(next);
            ctx.OpStore(counter, add(group, imm(1)));
            ctx.OpBranch(header);
            ctx.AddLabel(end);
        }
        if (scratch) {
            // The next HMMA may reuse the same words as soon as this helper returns.
            EmitBarrier(ctx);
        }
        std::array<Id, 2> packed;
        for (u32 row = 0; row < 2; ++row)
            packed[row] = ctx.OpPackHalf2x16(u32ty, ctx.OpCompositeConstruct(ctx.F32[2],
                ctx.OpLoad(f32ty, accumulators[row * 2]), ctx.OpLoad(f32ty, accumulators[row * 2 + 1])));
        ctx.OpReturnValue(ctx.OpCompositeConstruct(ctx.U32[2], packed[0], packed[1]));
        ctx.OpFunctionEnd();
    }
}

Id EmitTensorMulAdd(EmitContext& ctx, IR::Inst* inst, Id a, Id b, Id c) {
    const u32 depth = inst->Flags<u32>();
    if (depth != 8 && depth != 16) {
        throw NotImplementedException("Unsupported matrix multiply depth");
    }
    if (depth == 16 && ctx.cooperative_hmma) {
        return EmitCooperativeHmma(ctx, *ctx.cooperative_hmma, ctx.U32[1], ctx.U32[2], a, b, c);
    }
    if (ctx.workgroup_warp_payload.value) {
        // Bank zero is shared with warp publications. Finish all reads before the helper
        // overwrites it. Its final barrier makes the bank safe to reuse on return.
        EmitBarrier(ctx);
        ctx.workgroup_warp_payload = {};
    }
    return ctx.OpFunctionCall(ctx.U32[2], ctx.software_matrix_functions[depth == 16], a, b, c);
}

void PatchCooperativeHmma(std::vector<u32>& code, const CooperativeHmmaTypes& types) {
    for (size_t offset = 5; offset < code.size();) {
        const u32 word_count{code[offset] >> 16};
        if (word_count == 0) {
            return;
        }
        const auto opcode{static_cast<spv::Op>(code[offset] & 0xFFFFu)};
        std::optional<spv::Op> replacement;
        if (opcode == spv::Op::OpTypeStruct) {
            const u32 result{code[offset + 1]};
            if (result == types.matrix_a.value || result == types.matrix_b.value ||
                result == types.matrix_c.value) {
                replacement = spv::Op::OpTypeCooperativeMatrixKHR;
            }
        } else if (opcode == spv::Op::OpSelect && code[offset + 1] == types.matrix_c.value) {
            replacement = spv::Op::OpCooperativeMatrixMulAddKHR;
        }
        if (replacement) {
            code[offset] = (code[offset] & 0xFFFF0000u) | static_cast<u32>(*replacement);
        }
        offset += word_count;
    }
}

std::vector<u32> EmitCooperativeHmmaTest(u32 spirv_version) {
    Sirit::Module module{spirv_version};
    module.AddCapability(spv::Capability::Shader);
    module.AddCapability(spv::Capability::GroupNonUniform);

    const Id void_type{module.TypeVoid()};
    const Id u32_type{module.TypeUInt(32)};
    const Id u32x2_type{module.TypeVector(u32_type, 2)};
    const Id u32x4_type{module.TypeVector(u32_type, 4)};
    const CooperativeHmmaTypes types{DefineCooperativeHmmaTypes(module, u32_type, spirv_version)};

    const Id words_type{module.TypeRuntimeArray(u32_type)};
    module.Decorate(words_type, spv::Decoration::ArrayStride, 4U);
    const Id block_type{module.TypeStruct(words_type)};
    module.Decorate(block_type, spv::Decoration::Block);
    module.MemberDecorate(block_type, 0U, spv::Decoration::Offset, 0U);
    const Id buffer{module.AddGlobalVariable(
        module.TypePointer(spv::StorageClass::StorageBuffer, block_type),
        spv::StorageClass::StorageBuffer)};
    module.Decorate(buffer, spv::Decoration::DescriptorSet, 0U);
    module.Decorate(buffer, spv::Decoration::Binding, 0U);
    const Id lane_variable{module.AddGlobalVariable(
        module.TypePointer(spv::StorageClass::Input, u32_type), spv::StorageClass::Input)};
    module.Decorate(lane_variable, spv::Decoration::BuiltIn,
                    spv::BuiltIn::SubgroupLocalInvocationId);
    const Id word_pointer{module.TypePointer(spv::StorageClass::StorageBuffer, u32_type)};
    const Id zero{module.Constant(u32_type, 0U)};

    const Id main{module.OpFunction(void_type, spv::FunctionControlMask::MaskNone,
                                    module.TypeFunction(void_type))};
    module.AddLabel();
    const Id base{module.OpIMul(u32_type, module.OpLoad(u32_type, lane_variable),
                                module.Constant(u32_type, COOPERATIVE_HMMA_TEST_LANE_WORDS))};
    const auto word_at{[&](u32 index) {
        return module.OpAccessChain(word_pointer, buffer, zero,
                                    module.OpIAdd(u32_type, base,
                                                  module.Constant(u32_type, index)));
    }};
    const auto load{[&](u32 index) { return module.OpLoad(u32_type, word_at(index)); }};
    const Id a{module.OpCompositeConstruct(u32x4_type, load(0), load(1), load(2), load(3))};
    const Id b{module.OpCompositeConstruct(u32x2_type, load(4), load(5))};
    const Id c{module.OpCompositeConstruct(u32x2_type, load(6), load(7))};
    const Id d{EmitCooperativeHmma(module, types, u32_type, u32x2_type, a, b, c)};
    module.OpStore(word_at(8), module.OpCompositeExtract(u32_type, d, 0U));
    module.OpStore(word_at(9), module.OpCompositeExtract(u32_type, d, 1U));
    module.OpReturn();
    module.OpFunctionEnd();

    // Since SPIR-V 1.4 the interface lists every global the entry point uses.
    std::vector<Id> interfaces{lane_variable};
    if (spirv_version >= 0x00010400U) {
        interfaces.push_back(buffer);
    }
    module.AddEntryPoint(spv::ExecutionModel::GLCompute, main, "main", interfaces);
    module.AddExecutionMode(main, spv::ExecutionMode::LocalSize, GUEST_WARP_LANES, 1U, 1U);

    std::vector<u32> code{module.Assemble()};
    PatchCooperativeHmma(code, types);
    return code;
}

} // namespace Shader::Backend::SPIRV
