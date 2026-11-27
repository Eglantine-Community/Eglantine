// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <string>

#include "shader_recompiler/frontend/ir/abstract_syntax_list.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/program_header.h"
#include "shader_recompiler/shader_info.h"
#include "shader_recompiler/stage.h"

namespace Shader::IR {

struct Program {
    AbstractSyntaxList syntax_list;
    BlockList blocks;
    BlockList post_order_blocks;
    Info info;
    Stage stage{};
    std::array<u32, 3> workgroup_size{};
    OutputTopology output_topology{};
    u32 output_vertices{};
    u32 invocations{};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    bool is_geometry_passthrough{};
    MeshInfo mesh{};

    // Ballot, vote and shuffle are local to lane zero in this compute workgroup.
    // Quad and FSWZ operations have their own lowering.
    [[nodiscard]] bool HasScalarWarp() const noexcept {
        return stage == Stage::Compute && workgroup_size == std::array<u32, 3>{1, 1, 1} &&
               !info.uses_quad_shuffles && !info.uses_fswzadd;
    }
};

[[nodiscard]] std::string DumpProgram(const Program& program);

} // namespace Shader::IR
