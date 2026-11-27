// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"

#include "../maxwell-disas/file_environment.h"

int SpirvShaderRecompilerImpl(int argc, char *argv[]) {
    if (argc != 2) {
        printf("usage: %s [input file] [-n]\n"
            "RAW SPIRV CODE WILL BE SENT TO STDOUT!\n", argv[0]);
        return EXIT_FAILURE;
    }

    size_t cfg_offset = 0;

    Shader::ObjectPool<Shader::IR::Inst> inst_pool;
    Shader::ObjectPool<Shader::IR::Block> block_pool;
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> cfg_blocks;
    FileEnvironment env;

    FILE *fp = fopen(argv[1], "rb");
    if (fp != NULL) {
        struct stat st;
        fstat(fileno(fp), &st);
        auto const words = (st.st_size / sizeof(u64));
        env.code.resize(words + 1);
        fread(env.code.data(), sizeof(u64), words, fp);
        fclose(fp);
    }

    env.read_highest = env.read_lowest + env.code.size() * sizeof(u64);

    Shader::Maxwell::Flow::CFG cfg(env, cfg_blocks, cfg_offset);

    Shader::HostTranslateInfo host_info;
    host_info.support_float64 = true;
    host_info.support_float16 = true;
    host_info.support_int64 = true;
    host_info.needs_demote_reorder = true;
    host_info.support_snorm_render_buffer = true;
    host_info.support_viewport_index_layer = true;
    host_info.support_geometry_shader_passthrough = true;
    host_info.support_conditional_barrier = true;
    host_info.min_ssbo_alignment = 0;
    host_info.ApplyDescriptorLimitPolicy();
    auto program = Shader::Maxwell::TranslateProgram(inst_pool, block_pool, env, cfg, host_info);

    // The dump tool targets a reference Vulkan 1.1 device with 32 lanes and full subgroup
    // operations in the original Maxwell shader stages.
    Shader::Profile profile{};
    profile.supported_spirv = 0x00010300;
    profile.unified_descriptor_binding = true;
    profile.support_vote = true;
    profile.support_ballot = true;
    profile.support_shuffle = true;
    profile.support_shuffle_relative = true;
    profile.support_quad_shuffles = true;
    profile.support_quad_shuffles_all_stages = true;
    profile.max_subgroup_size = 32;
    const auto spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
    const size_t written = std::fwrite(spirv.data(), sizeof(spirv.front()), spirv.size(), stdout);
    return written == spirv.size() && std::fflush(stdout) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
