// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>

#include "common/common_types.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/profile.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Vulkan {

namespace {

bool SupportsWarpInstructions(const Shader::IR::Program& program, const Shader::Profile& native_profile,
                              u32 max_width) {
    // Ballot supports all four shuffles via constant broadcasts and also implements votes.
    if (native_profile.support_ballot) {
        return true;
    }
    const bool shuffle = native_profile.support_shuffle && !native_profile.has_broken_spirv_subgroup_shuffle;
    const bool relative = native_profile.support_shuffle_relative && !native_profile.has_broken_spirv_subgroup_shuffle;
    for (const auto* block : program.blocks) {
        for (const auto& inst : block->Instructions()) {
            using Shader::IR::Opcode;
            switch (inst.GetOpcode()) {
            case Opcode::SubgroupBallot:
                return false;
            case Opcode::VoteAll:
            case Opcode::VoteAny:
            case Opcode::VoteEqual:
                if (!native_profile.support_vote || max_width > GuestWarpSize) {
                    return false;
                }
                break;
            case Opcode::ShuffleUp:
            case Opcode::ShuffleDown:
                if (relative) {
                    break;
                }
                [[fallthrough]];
            case Opcode::ShuffleIndex:
            case Opcode::ShuffleButterfly:
                if (!shuffle) {
                    return false;
                }
                break;
            default:
                break;
            }
        }
    }
    return true;
}

} // namespace

bool ConfigureNativeComputeWarp(const Device& device, const Shader::IR::Program& program,
                                Shader::Profile& profile, std::array<u32, 3>& host_size) {
    const auto layout = device.GetComputeWarpLayout(program.workgroup_size);
    if (!layout || !SupportsWarpInstructions(program, profile, layout->max_width)) {
        return false;
    }
    host_size = layout->local_size;
    profile.compute_warp_local_id_from_subgroup = true;
    profile.compute_warp_flatten_workgroup = host_size != program.workgroup_size;
    profile.compute_warp_subgroup_size = layout->min_width == layout->max_width ? layout->min_width : 0;
    profile.max_subgroup_size = layout->max_width;
    profile.warp_size_potentially_larger_than_guest = layout->max_width > GuestWarpSize;
    return true;
}

vk::ShaderModule BuildShader(const Device& device, std::span<const u32> code) {
    return device.GetLogical().CreateShaderModule({
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = static_cast<u32>(code.size_bytes()),
        .pCode = code.data(),
    });
}

} // namespace Vulkan
