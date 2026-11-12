// SPDX-FileCopyrightText: Copyright 2026 Dalia Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

#include "common/common_types.h"
#include "common/thread_worker.h"
#include "shader_recompiler/environment.h"
#include "shader_recompiler/frontend/ir/basic_block.h"
#include "shader_recompiler/frontend/ir/program.h"
#include "shader_recompiler/frontend/ir/value.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/host_translate_info.h"
#include "shader_recompiler/object_pool.h"
#include "shader_recompiler/profile.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/precompile/shader_container_scanner.h"

namespace Core {
class System;
}

namespace VideoCore::Precompile {

/// Self-contained Shader::Environment backed by bytes extracted from game shader containers.
class PrecompileEnvironment final : public Shader::Environment {
public:
    explicit PrecompileEnvironment(const ScannedShader& shader_);
    ~PrecompileEnvironment() override;

    /// Initializes the environment, lowering native SM86 binaries to Maxwell when required.
    [[nodiscard]] bool Initialize(std::string* out_error = nullptr);

    [[nodiscard]] bool IsValid() const noexcept {
        return valid;
    }

    [[nodiscard]] u64 ReadInstruction(u32 address) override;
    [[nodiscard]] u32 TranslateProgramAddress(u32 address) const override;
    [[nodiscard]] bool IsNativeProgram() const override;
    [[nodiscard]] u32 ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) override;
    [[nodiscard]] Shader::TextureType ReadTextureType(u32 handle) override;
    [[nodiscard]] Shader::TexturePixelFormat ReadTexturePixelFormat(u32 handle) override;
    [[nodiscard]] bool IsTexturePixelFormatInteger(u32 handle) override;
    [[nodiscard]] u32 ReadViewportTransformState() override;
    [[nodiscard]] u32 TextureBoundBuffer() const override;
    [[nodiscard]] u32 LocalMemorySize() const override;
    [[nodiscard]] u32 SharedMemorySize() const override;
    [[nodiscard]] std::array<u32, 3> WorkgroupSize() const override;
    [[nodiscard]] bool HasHLEMacroState() const override;
    [[nodiscard]] std::optional<Shader::ReplaceConstant> GetReplaceConstBuffer(u32 bank,
                                                                               u32 offset) override;
    void Dump(u64 pipeline_hash, u64 shader_hash) override;

private:
    const ScannedShader& shader;
    std::vector<u64> code_words;
    std::array<u32, 3> workgroup_size{1, 1, 1};
    u32 texture_bound{2};
    u32 local_memory_size{};
    u32 shared_memory_size{};
    bool native_sm86{};
    bool valid{};
};

struct PrecompilePools {
    void Release() {
        flow_block.ReleaseContents();
        block.ReleaseContents();
        inst.ReleaseContents();
    }

    Shader::ObjectPool<Shader::IR::Inst> inst{8192};
    Shader::ObjectPool<Shader::IR::Block> block{32};
    Shader::ObjectPool<Shader::Maxwell::Flow::Block> flow_block{32};
};

struct TranslatedShader {
    Shader::Stage stage{Shader::Stage::VertexB};
    Shader::Info info;
    std::vector<u32> spirv;
    u32 shared_memory_size{};
};

/// Translates a scanned container shader into IR and SPIR-V using isolated pools.
[[nodiscard]] bool TranslateScannedShader(PrecompilePools& pools, const ScannedShader& shader,
                                          const Shader::Profile& profile,
                                          const Shader::HostTranslateInfo& host_info,
                                          u32 max_compute_shared_memory,
                                          bool clamp_compute_shared_memory,
                                          TranslatedShader& out_result,
                                          std::string* out_error = nullptr);

} // namespace VideoCore::Precompile

namespace Vulkan {

class Device;
class GraphicsPipelineLibraryCache;
class RenderPassCache;

namespace vk {
class PipelineCache;
}

struct PrecompileContext {
    Core::System& system;
    const Device& device;
    RenderPassCache& render_pass_cache;
    vk::PipelineCache& vulkan_pipeline_cache;
    const std::filesystem::path& vulkan_pipeline_cache_filename;
    GraphicsPipelineLibraryCache& gpl_cache;
    Common::ThreadWorker& workers;
    const Shader::Profile& profile;
    const Shader::HostTranslateInfo& host_info;
    const std::filesystem::path& cache_dir;
    u64 title_id{};
    bool use_vulkan_pipeline_cache{};
    u32 cache_version{};
    std::stop_token stop_loading;
    const VideoCore::DiskResourceLoadCallback& callback;
};

struct PrecompileStats {
    bool skipped_by_manifest{};
    bool completed{};
    std::size_t scanned_total{};
    std::size_t translated_ok{};
    std::size_t compute_warmed{};
    std::size_t graphics_stages_warmed{};
    std::size_t skipped_unsupported{};
    std::size_t failed{};
    double elapsed_seconds{};
};

/// Scans patched game archives for shaders and pre-warms Vulkan ISA caches prior to launch.
[[nodiscard]] PrecompileStats PrecompileTitleShaders(const PrecompileContext& ctx);

} // namespace Vulkan

