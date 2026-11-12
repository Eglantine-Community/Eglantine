// SPDX-FileCopyrightText: Copyright 2026 Dalia Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <limits>
#include <mutex>
#include <thread>
#include <vector>

#include "common/cityhash.h"
#include "common/fs/fs.h"
#include "common/fs/path_util.h"
#include "common/logging.h"
#include "common/thread_worker.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/exception.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "shader_recompiler/frontend/ampere/program_address_map.h"
#include "shader_recompiler/frontend/maxwell/control_flow.h"
#include "shader_recompiler/frontend/maxwell/translate_program.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/precompile/shader_precompiler.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline_library.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace VideoCore::Precompile {

namespace {

[[nodiscard]] Shader::Stage ToShaderStage(ScannedStage stage) noexcept {
    switch (stage) {
    case ScannedStage::Vertex:
        return Shader::Stage::VertexB;
    case ScannedStage::TessellationControl:
        return Shader::Stage::TessellationControl;
    case ScannedStage::TessellationEval:
        return Shader::Stage::TessellationEval;
    case ScannedStage::Geometry:
        return Shader::Stage::Geometry;
    case ScannedStage::Fragment:
        return Shader::Stage::Fragment;
    case ScannedStage::Compute:
        return Shader::Stage::Compute;
    case ScannedStage::Task:
        return Shader::Stage::Task;
    case ScannedStage::Mesh:
        return Shader::Stage::Mesh;
    }
    return Shader::Stage::VertexB;
}

constexpr u32 DefaultFallbackLocalMemorySize = 16384U;

[[nodiscard]] u32 ComputeSphLocalMemorySize(const Shader::ProgramHeader& sph) noexcept {
    const u64 clamped = (std::min<u64>)(sph.LocalMemorySize(), (std::numeric_limits<u32>::max)());
    return static_cast<u32>(clamped) + sph.common3.shader_local_memory_crs_size.Value();
}

void EnsureProgramMemorySizes(Shader::IR::Program& program, u32 max_compute_shared_memory,
                              bool has_explicit_local_size) {
    bool uses_shared = false;
    bool uses_local = false;
    for (const Shader::IR::Block* block : program.blocks) {
        for (const Shader::IR::Inst& inst : block->Instructions()) {
            const auto op = inst.GetOpcode();
            if ((op >= Shader::IR::Opcode::LoadSharedU8 &&
                 op <= Shader::IR::Opcode::WriteSharedU128) ||
                (op >= Shader::IR::Opcode::SharedAtomicIAdd32 &&
                 op <= Shader::IR::Opcode::SharedAtomicExchange32x2)) {
                uses_shared = true;
            } else if (op == Shader::IR::Opcode::LoadLocal ||
                       op == Shader::IR::Opcode::WriteLocal) {
                uses_local = true;
            }
            if (uses_shared && uses_local) {
                break;
            }
        }
        if (uses_shared && uses_local) {
            break;
        }
    }
    if (uses_shared && program.shared_memory_size == 0) {
        program.shared_memory_size =
            max_compute_shared_memory > 0 ? (std::min)(max_compute_shared_memory, 49152U) : 32768U;
    }
    if (uses_local) {
        if (program.local_memory_size == 0) {
            program.local_memory_size = DefaultFallbackLocalMemorySize;
        }
    } else if (!has_explicit_local_size) {
        program.local_memory_size = 0;
    }
}

} // Anonymous namespace

PrecompileEnvironment::PrecompileEnvironment(const ScannedShader& shader_)
    : shader{shader_},
      workgroup_size{(std::max)(shader_.local_size[0], 1U),
                     (std::max)(shader_.local_size[1], 1U),
                     (std::max)(shader_.local_size[2], 1U)},
      native_sm86{shader_.is_native_sm86} {
    stage = ToShaderStage(shader.stage);
    is_proprietary_driver = true;
}

PrecompileEnvironment::~PrecompileEnvironment() = default;

bool PrecompileEnvironment::Initialize(std::string* out_error) {
    valid = false;
    code_words.clear();

    if (native_sm86) {
        if (shader.program_bytes.empty() ||
            (shader.program_bytes.size() % sizeof(Shader::Ampere::Instruction)) != 0) {
            if (out_error) {
                *out_error = "Invalid native SM86 instruction byte length";
            }
            return false;
        }
        const std::size_t count =
            shader.program_bytes.size() / sizeof(Shader::Ampere::Instruction);
        std::vector<Shader::Ampere::Instruction> native(count);
        std::memcpy(native.data(), shader.program_bytes.data(), shader.program_bytes.size());

        const auto lowered = Shader::Ampere::LowerToMaxwell(
            native,
            {.geometry = (stage == Shader::Stage::Geometry), .native_code_address = 0});
        if (!lowered) {
            if (out_error) {
                *out_error = lowered.error;
            }
            return false;
        }

        std::memset(&sph, 0, sizeof(sph));
        if (stage != Shader::Stage::Compute) {
            if (shader.sph_bytes) {
                std::memcpy(&sph, shader.sph_bytes->data(), sizeof(sph));
                local_memory_size = ComputeSphLocalMemorySize(sph);
            } else {
                sph.common0.sph_type.Assign(stage == Shader::Stage::Fragment ? 2U : 1U);
                sph.common0.version.Assign(3U);
                sph.common0.mrt_enable.Assign(stage == Shader::Stage::Fragment ? 1U : 0U);
                local_memory_size = DefaultFallbackLocalMemorySize;
            }
        } else {
            // Standalone NVN2 compute shaders lack QMD local/shared allocations.
            local_memory_size = DefaultFallbackLocalMemorySize;
        }

        if (lowered.required_texture_buffer != ~0U) {
            texture_bound = lowered.required_texture_buffer;
        } else {
            texture_bound = 2U;
        }

        code_words.resize(sizeof(sph) / sizeof(u64));
        std::memcpy(code_words.data(), &sph, sizeof(sph));
        for (const u64 inst : lowered.instructions) {
            if ((code_words.size() * sizeof(u64)) % 32 == 0) {
                code_words.push_back(0ULL);
            }
            code_words.push_back(inst);
        }
        code_words.push_back(0ULL);
        Shader::Ampere::AppendNativeAddressMap(code_words, lowered.source_instruction_offsets);
        start_address =
            stage == Shader::Stage::Compute ? static_cast<u32>(sizeof(sph)) : 0U;
    } else {
        if (shader.program_bytes.empty() || (shader.program_bytes.size() % sizeof(u64)) != 0) {
            if (out_error) {
                *out_error = "Invalid Maxwell instruction byte length";
            }
            return false;
        }
        code_words.resize(shader.program_bytes.size() / sizeof(u64));
        std::memcpy(code_words.data(), shader.program_bytes.data(), shader.program_bytes.size());

        std::memset(&sph, 0, sizeof(sph));
        if (stage != Shader::Stage::Compute) {
            if (shader.program_bytes.size() < sizeof(sph) + sizeof(u64)) {
                if (out_error) {
                    *out_error = "Maxwell graphics shader shorter than SPH + 1 instruction";
                }
                return false;
            }
            std::memcpy(&sph, code_words.data(), sizeof(sph));
            local_memory_size = ComputeSphLocalMemorySize(sph);
        } else {
            local_memory_size = DefaultFallbackLocalMemorySize;
        }
        texture_bound = 2U;
        start_address = 0U;
    }

    valid = true;
    return true;
}

u64 PrecompileEnvironment::ReadInstruction(u32 address) {
    const std::size_t idx = address / sizeof(u64);
    if ((address % sizeof(u64)) != 0 || idx >= code_words.size()) {
        throw Shader::RuntimeError("Precompile ReadInstruction out of bounds at {:#x}", address);
    }
    return code_words[idx];
}

u32 PrecompileEnvironment::TranslateProgramAddress(u32 address) const {
    if (const auto translated = Shader::Ampere::NativeProgramAddress(code_words, address)) {
        return *translated;
    }
    return address;
}

bool PrecompileEnvironment::IsNativeProgram() const {
    return native_sm86;
}

u32 PrecompileEnvironment::ReadCbufValue(u32 cbuf_index, u32 cbuf_offset) {
    if (cbuf_index == 1 && shader.c1_bytes.size() >= sizeof(u32) &&
        cbuf_offset <= shader.c1_bytes.size() - sizeof(u32)) {
        u32 value = 0;
        std::memcpy(&value, shader.c1_bytes.data() + cbuf_offset, sizeof(u32));
        return value;
    }
    return 0;
}

Shader::TextureType PrecompileEnvironment::ReadTextureType(u32) {
    return Shader::TextureType::Color2D;
}

Shader::TexturePixelFormat PrecompileEnvironment::ReadTexturePixelFormat(u32) {
    return Shader::TexturePixelFormat::A8B8G8R8_UNORM;
}

bool PrecompileEnvironment::IsTexturePixelFormatInteger(u32) {
    return false;
}

u32 PrecompileEnvironment::ReadViewportTransformState() {
    return 1;
}

u32 PrecompileEnvironment::TextureBoundBuffer() const {
    return texture_bound;
}

u32 PrecompileEnvironment::LocalMemorySize() const {
    return local_memory_size;
}

u32 PrecompileEnvironment::SharedMemorySize() const {
    return shared_memory_size;
}

std::array<u32, 3> PrecompileEnvironment::WorkgroupSize() const {
    return workgroup_size;
}

bool PrecompileEnvironment::HasHLEMacroState() const {
    return false;
}

std::optional<Shader::ReplaceConstant> PrecompileEnvironment::GetReplaceConstBuffer(u32, u32) {
    return std::nullopt;
}

void PrecompileEnvironment::Dump(u64, u64) {}

bool TranslateScannedShader(PrecompilePools& pools, const ScannedShader& shader,
                            const Shader::Profile& profile,
                            const Shader::HostTranslateInfo& host_info,
                            u32 max_compute_shared_memory, bool clamp_compute_shared_memory,
                            TranslatedShader& out_result, std::string* out_error) {
    pools.Release();
    PrecompileEnvironment env{shader};
    if (!env.Initialize(out_error)) {
        return false;
    }

    try {
        const Shader::Stage stage = env.ShaderStage();
        const bool has_explicit_local_size =
            stage != Shader::Stage::Compute && (!shader.is_native_sm86 || shader.sph_bytes);
        out_result.stage = stage;

        if (stage == Shader::Stage::Compute) {
            Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, env.StartAddress()};
            auto program{Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg,
                                                           host_info)};
            EnsureProgramMemorySizes(program, max_compute_shared_memory, has_explicit_local_size);
            if (clamp_compute_shared_memory &&
                program.shared_memory_size > max_compute_shared_memory) {
                program.shared_memory_size = max_compute_shared_memory;
            }
            out_result.shared_memory_size = program.shared_memory_size;
            out_result.spirv = Shader::Backend::SPIRV::EmitSPIRV(profile, program);
            out_result.info = std::move(program.info);
            return !out_result.spirv.empty();
        }

        const u32 cfg_offset =
            static_cast<u32>(env.StartAddress() + sizeof(Shader::ProgramHeader));
        Shader::Maxwell::Flow::CFG cfg{env, pools.flow_block, cfg_offset, false};
        auto program{Shader::Maxwell::TranslateProgram(pools.inst, pools.block, env, cfg,
                                                       host_info)};
        EnsureProgramMemorySizes(program, max_compute_shared_memory, has_explicit_local_size);
        Shader::RuntimeInfo runtime_info{};
        if (stage == Shader::Stage::Fragment) {
            runtime_info.previous_stage_stores.mask.set();
        }
        Shader::Maxwell::ConvertLegacyToGeneric(program, runtime_info);
        Shader::Backend::Bindings binding{};
        out_result.spirv =
            Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
        out_result.info = std::move(program.info);
        return !out_result.spirv.empty();
    } catch (const std::exception& e) {
        if (out_error) {
            *out_error = e.what();
        }
        return false;
    }
}

} // namespace VideoCore::Precompile

namespace Vulkan {

namespace {

constexpr u32 ManifestMagic = 0x504C4144U; // "DALP"
constexpr u32 ManifestVersion = 3U;
constexpr u32 GuestWarpSize = 32U;
constexpr std::array<char, 8> VulkanCacheMagic{'y', 'u', 'z', 'u', 'v', 'k', 'c', 'h'};

struct ManifestRecord {
    u32 magic{ManifestMagic};
    u32 version{ManifestVersion};
    u64 title_id{};
    u64 content_fingerprint{};
    u64 device_fingerprint{};
    u32 cache_version{};
    u32 reserved{};
};
static_assert(sizeof(ManifestRecord) == 40);

[[nodiscard]] u64 ComputeDeviceFingerprint(const Device& device, bool gpl_supported,
                                           bool fast_linking, bool use_vk_pipeline_cache) {
    struct DeviceFpData {
        u32 vendor_id{};
        u32 device_id{};
        u32 driver_version{};
        u32 driver_id{};
        std::array<u8, VK_UUID_SIZE> cache_uuid{};
        u8 gpl{};
        u8 fast_link{};
        u8 vk_cache{};
        u8 pad{};
    } data{};

    const VkPhysicalDeviceProperties props = device.GetPhysical().GetProperties();
    data.vendor_id = props.vendorID;
    data.device_id = props.deviceID;
    data.driver_version = props.driverVersion;
    data.driver_id = static_cast<u32>(device.GetDriverID());
    std::memcpy(data.cache_uuid.data(), props.pipelineCacheUUID, VK_UUID_SIZE);
    data.gpl = gpl_supported ? 1U : 0U;
    data.fast_link = fast_linking ? 1U : 0U;
    data.vk_cache = use_vk_pipeline_cache ? 1U : 0U;
    return Common::CityHash64(reinterpret_cast<const char*>(&data), sizeof(data));
}

[[nodiscard]] bool ReadManifest(const std::filesystem::path& path, ManifestRecord& out_record) {
    std::ifstream file(path, std::ios::binary);
    if (!file.is_open()) {
        return false;
    }
    file.read(reinterpret_cast<char*>(&out_record), sizeof(out_record));
    return static_cast<bool>(file) && out_record.magic == ManifestMagic &&
           out_record.version == ManifestVersion;
}

void WriteManifestAtomic(const std::filesystem::path& path, const ManifestRecord& record) {
    auto temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return;
        }
        file.write(reinterpret_cast<const char*>(&record), sizeof(record));
        if (!file) {
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, path, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
    }
}

void SerializePipelineCacheAtomic(const std::filesystem::path& filename,
                                  const vk::PipelineCache& cache, u32 cache_version) {
    if (filename.empty() || !cache) {
        return;
    }
    size_t cache_size = 0;
    cache.Read(&cache_size, nullptr);
    std::vector<char> cache_data(cache_size);
    if (cache_size > 0) {
        cache.Read(&cache_size, cache_data.data());
    }
    auto temp = filename;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file.is_open()) {
            return;
        }
        file.write(VulkanCacheMagic.data(), VulkanCacheMagic.size());
        file.write(reinterpret_cast<const char*>(&cache_version), sizeof(cache_version));
        if (!cache_data.empty()) {
            file.write(cache_data.data(), static_cast<std::streamsize>(cache_data.size()));
        }
        if (!file) {
            return;
        }
    }
    std::error_code ec;
    std::filesystem::rename(temp, filename, ec);
    if (ec) {
        std::filesystem::remove(temp, ec);
    }
}

[[nodiscard]] bool HasUsableVulkanPipelineCacheFile(const std::filesystem::path& path) {
    if (path.empty()) {
        return false;
    }
    std::error_code ec;
    const auto sz = std::filesystem::file_size(path, ec);
    return !ec && sz > (VulkanCacheMagic.size() + sizeof(u32));
}

} // Anonymous namespace

PrecompileStats PrecompileTitleShaders(const PrecompileContext& ctx) {
    using VideoCore::Precompile::PrecompilePools;
    using VideoCore::Precompile::ScannedShader;
    using VideoCore::Precompile::ScannedStage;
    using VideoCore::Precompile::ScanOptions;
    using VideoCore::Precompile::ScanStats;
    using VideoCore::Precompile::ScanTitleContainers;
    using VideoCore::Precompile::TranslateScannedShader;
    using VideoCore::Precompile::TranslatedShader;

    PrecompileStats stats{};
    if (ctx.title_id == 0 || ctx.stop_loading.stop_requested()) {
        return stats;
    }

    const auto start_time = std::chrono::steady_clock::now();
    const bool gpl_supported = ctx.gpl_cache.IsSupported();
    const bool gpl_fast_linking = ctx.gpl_cache.IsFastLinkingSupported();
    const u64 device_fp = ComputeDeviceFingerprint(ctx.device, gpl_supported, gpl_fast_linking,
                                                   ctx.use_vulkan_pipeline_cache);
    const u64 content_fp = VideoCore::Precompile::ComputeTitleContentFingerprint(ctx.system,
                                                                                 ctx.title_id);
    const auto manifest_path = ctx.cache_dir / "precompile_manifest.bin";

    ManifestRecord existing{};
    const bool vk_cache_ok =
        !ctx.use_vulkan_pipeline_cache ||
        HasUsableVulkanPipelineCacheFile(ctx.vulkan_pipeline_cache_filename);
    if (vk_cache_ok && ReadManifest(manifest_path, existing) &&
        existing.title_id == ctx.title_id &&
        existing.content_fingerprint == content_fp &&
        existing.device_fingerprint == device_fp &&
        existing.cache_version == ctx.cache_version) {
        stats.skipped_by_manifest = true;
        stats.completed = true;
        LOG_INFO(Render_Vulkan,
                 "Precompile skipped by up-to-date manifest for title {:016X}", ctx.title_id);
        return stats;
    }

    ScanOptions scan_opts{};
    scan_opts.should_stop = [&ctx] { return ctx.stop_loading.stop_requested(); };

    ScanStats scan_stats{};
    u64 scanned_fp = content_fp;
    auto scanned =
        ScanTitleContainers(ctx.system, ctx.title_id, scan_opts, &scan_stats, &scanned_fp);
    stats.scanned_total = scanned.size();
    if (ctx.stop_loading.stop_requested() || scanned.empty()) {
        stats.completed = !ctx.stop_loading.stop_requested();
        if (stats.completed) {
            ManifestRecord rec{
                .title_id = ctx.title_id,
                .content_fingerprint = scanned_fp,
                .device_fingerprint = device_fp,
                .cache_version = ctx.cache_version,
            };
            WriteManifestAtomic(manifest_path, rec);
        }
        return stats;
    }

    // Filter out stages that cannot be compiled into a standalone stage/pipeline cleanly here.
    std::vector<ScannedShader> compilable;
    compilable.reserve(scanned.size());
    for (auto& s : scanned) {
        switch (s.stage) {
        case ScannedStage::Vertex:
        case ScannedStage::Fragment:
        case ScannedStage::Compute:
            compilable.push_back(std::move(s));
            break;
        default:
            ++stats.skipped_unsupported;
            break;
        }
    }
    std::vector<ScannedShader>{}.swap(scanned);
    if (compilable.empty()) {
        stats.completed = true;
        return stats;
    }

    RenderPassKey compat_rp_key{};
    compat_rp_key.color_formats.fill(VideoCore::Surface::PixelFormat::Invalid);
    compat_rp_key.color_formats[0] = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    compat_rp_key.depth_format = VideoCore::Surface::PixelFormat::D24_UNORM_S8_UINT;
    compat_rp_key.samples = VK_SAMPLE_COUNT_1_BIT;
    const VkRenderPass compatible_rp = ctx.render_pass_cache.Get(compat_rp_key);

    const VkDriverIdKHR driver_id = ctx.device.GetDriverID();
    const bool clamp_shared_mem = driver_id == VK_DRIVER_ID_QUALCOMM_PROPRIETARY ||
                                  driver_id == VK_DRIVER_ID_ARM_PROPRIETARY ||
                                  driver_id == VK_DRIVER_ID_MOLTENVK;
    const u32 max_shared_mem = ctx.device.GetMaxComputeSharedMemorySize();

    std::atomic<std::size_t> completed_count{0};
    std::atomic<std::size_t> translated_ok{0};
    std::atomic<std::size_t> compute_warmed{0};
    std::atomic<std::size_t> graphics_warmed{0};
    std::atomic<std::size_t> failed_count{0};
    std::mutex callback_mutex;

    const std::size_t total = compilable.size();
    const std::size_t hw_threads = std::max<std::size_t>(std::thread::hardware_concurrency(), 1ULL);
    const std::size_t worker_count =
        ctx.device.HasBrokenParallelShaderCompiling()
            ? 1ULL
            : std::clamp<std::size_t>(hw_threads, 4ULL, 16ULL);
    const double scan_elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    LOG_INFO(Render_Vulkan,
             "Precompile for {:016X}: scanned {} shaders ({} compilable, {} unsupported) in "
             "{:.2f}s across {} workers...",
             ctx.title_id, stats.scanned_total, total, stats.skipped_unsupported, scan_elapsed,
             worker_count);

    ctx.callback(VideoCore::LoadCallbackStage::Prepare, 0, total);

    // Use a dedicated normal-priority worker pool so background EcoQoS throttling on
    // PipelineCache::workers does not stall startup precompilation on hybrid/AMD CPUs.
    Common::ThreadWorker precompile_workers(worker_count, "VkPrecompile");
    constexpr std::size_t BatchSize = 64;
    for (std::size_t start = 0; start < total; start += BatchSize) {
        const std::size_t end = (std::min)(start + BatchSize, total);
        precompile_workers.QueueWork([&, start, end]() {
            PrecompilePools pools;
            for (std::size_t i = start; i < end; ++i) {
                if (ctx.stop_loading.stop_requested()) {
                    break;
                }

                ScannedShader shader = std::move(compilable[i]);
                TranslatedShader translated{};
                std::string error_msg;
                const bool ok = TranslateScannedShader(
                    pools, shader, ctx.profile, ctx.host_info, max_shared_mem, clamp_shared_mem,
                    translated, &error_msg);
                shader = {};
                if (!ok) {
                    failed_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    translated_ok.fetch_add(1, std::memory_order_relaxed);
                    try {
                        vk::ShaderModule spv_module = BuildShader(ctx.device, translated.spirv);
                        std::vector<u32>{}.swap(translated.spirv);
                        if (translated.stage == Shader::Stage::Compute) {
                            DescriptorLayoutBuilder builder{ctx.device};
                            builder.Add(translated.info, VK_SHADER_STAGE_COMPUTE_BIT);
                            const bool use_push = builder.CanUsePushDescriptor();
                            const vk::DescriptorSetLayout dsl =
                                builder.CreateDescriptorSetLayout(use_push, false);
                            const vk::PipelineLayout layout = builder.CreatePipelineLayout(*dsl);

                            const bool uses_warp =
                                translated.info.UsesWarpOps() &&
                                ctx.device.IsExtSubgroupSizeControlSupported() &&
                                ctx.device.IsGuestWarpSizeSupported(VK_SHADER_STAGE_COMPUTE_BIT);
                            const VkPipelineShaderStageRequiredSubgroupSizeCreateInfoEXT
                                subgroup_ci{
                                    .sType =
                                        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO_EXT,
                                    .pNext = nullptr,
                                    .requiredSubgroupSize = Vulkan::GuestWarpSize,
                                };
                            const VkComputePipelineCreateInfo compute_ci{
                                .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
                                .pNext = nullptr,
                                .flags = 0,
                                .stage =
                                    {
                                        .sType =
                                            VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                                        .pNext = uses_warp ? &subgroup_ci : nullptr,
                                        .flags = 0,
                                        .stage = VK_SHADER_STAGE_COMPUTE_BIT,
                                        .module = *spv_module,
                                        .pName = "main",
                                        .pSpecializationInfo = nullptr,
                                    },
                                .layout = *layout,
                                .basePipelineHandle = VK_NULL_HANDLE,
                                .basePipelineIndex = 0,
                            };
                            if (ctx.device.GetLogical().TryCreateComputePipeline(
                                    compute_ci, *ctx.vulkan_pipeline_cache)) {
                                compute_warmed.fetch_add(1, std::memory_order_relaxed);
                            }
                        // On AMD proprietary drivers, compiling tens of thousands of standalone GPL
                        // stage pipelines up front blocks boot for >10 min on large NUPs while
                        // fast linking already keeps runtime and cached pipeline loads fast.
                        } else if (gpl_supported && compatible_rp != VK_NULL_HANDLE &&
                                   driver_id != VK_DRIVER_ID_AMD_PROPRIETARY) {
                            const VkShaderStageFlagBits vk_stage =
                                translated.stage == Shader::Stage::Fragment
                                    ? VK_SHADER_STAGE_FRAGMENT_BIT
                                    : VK_SHADER_STAGE_VERTEX_BIT;
                            DescriptorLayoutBuilder builder{ctx.device};
                            builder.Add(translated.info, vk_stage);
                            const bool use_push = builder.CanUsePushDescriptor();
                            const vk::DescriptorSetLayout dsl =
                                builder.CreateDescriptorSetLayout(use_push, false);
                            const vk::PipelineLayout layout = builder.CreatePipelineLayout(*dsl);
                            const bool req_subgroup =
                                translated.info.UsesWarpOps() &&
                                ctx.device.IsExtSubgroupSizeControlSupported() &&
                                ctx.device.IsGuestWarpSizeSupported(vk_stage);
                            if (ctx.gpl_cache.PrewarmShaderStage(vk_stage, *spv_module, *layout,
                                                                   compatible_rp, req_subgroup,
                                                                   Vulkan::GuestWarpSize)) {
                                graphics_warmed.fetch_add(1, std::memory_order_relaxed);
                            }
                        }
                    } catch (const std::exception&) {
                        failed_count.fetch_add(1, std::memory_order_relaxed);
                    }
                }

                const std::size_t done = completed_count.fetch_add(1, std::memory_order_acq_rel) + 1;
                if ((done & 7U) == 0 || done == total) {
                    std::scoped_lock lk{callback_mutex};
                    ctx.callback(VideoCore::LoadCallbackStage::Build, done, total);
                    if ((done % 2000U) == 0 && done < total) {
                        const double elapsed =
                            std::chrono::duration<double>(std::chrono::steady_clock::now() -
                                                          start_time)
                                .count();
                        LOG_INFO(Render_Vulkan, "Precompile for {:016X}: {}/{} ({:.1f}s)...",
                                 ctx.title_id, done, total, elapsed);
                    }
                }
            }
        });
    }

    precompile_workers.WaitForRequests(ctx.stop_loading);
    if (ctx.stop_loading.stop_requested()) {
        precompile_workers.ClearRequests();
    }
    std::vector<ScannedShader>{}.swap(compilable);

    stats.translated_ok = translated_ok.load(std::memory_order_relaxed);
    stats.compute_warmed = compute_warmed.load(std::memory_order_relaxed);
    stats.graphics_stages_warmed = graphics_warmed.load(std::memory_order_relaxed);
    stats.failed = failed_count.load(std::memory_order_relaxed);
    stats.elapsed_seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    stats.completed = !ctx.stop_loading.stop_requested();

    if (stats.completed && (stats.translated_ok > 0 || total == 0)) {
        if (ctx.use_vulkan_pipeline_cache) {
            SerializePipelineCacheAtomic(ctx.vulkan_pipeline_cache_filename,
                                         ctx.vulkan_pipeline_cache, ctx.cache_version);
        }
        ManifestRecord rec{
            .title_id = ctx.title_id,
            .content_fingerprint = scanned_fp,
            .device_fingerprint = device_fp,
            .cache_version = ctx.cache_version,
        };
        WriteManifestAtomic(manifest_path, rec);
    }

    LOG_INFO(Render_Vulkan,
             "Precompile for {:016X}: scanned={}, translated={}, compute_warmed={}, "
             "gfx_gpl_warmed={}, unsupported={}, failed={} in {:.2f}s (sarc={}, bnsh={}, "
             "sharcb={}, ndsr={}, nso={}, zstd_dicts={})",
             ctx.title_id, stats.scanned_total, stats.translated_ok, stats.compute_warmed,
             stats.graphics_stages_warmed, stats.skipped_unsupported, stats.failed,
             stats.elapsed_seconds, scan_stats.sarc_archives, scan_stats.bnsh_containers,
             scan_stats.sharcb_containers, scan_stats.ndsr_containers, scan_stats.nso_modules,
             scan_stats.zstd_dicts_loaded);

    return stats;
}

} // namespace Vulkan

