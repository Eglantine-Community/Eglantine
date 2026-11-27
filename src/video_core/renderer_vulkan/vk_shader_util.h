// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <span>

#include "common/common_types.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace Shader {
struct Profile;
namespace IR {
struct Program;
}
}

namespace Vulkan {

class Device;

/// Configure native compute collectives only when the subgroup layout and operations are sufficient.
bool ConfigureNativeComputeWarp(const Device& device, const Shader::IR::Program& program,
                                Shader::Profile& profile, std::array<u32, 3>& host_size);

vk::ShaderModule BuildShader(const Device& device, std::span<const u32> code);

} // namespace Vulkan
