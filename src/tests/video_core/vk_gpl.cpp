// SPDX-FileCopyrightText: Copyright 2026 Dalia Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <array>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>

#include "common/common_types.h"
#include "common/scope_exit.h"
#include "common/settings.h"
#include "shader_recompiler/frontend/ampere/lower_to_maxwell.h"
#include "video_core/precompile/shader_container_scanner.h"
#include "video_core/precompile/shader_precompiler.h"
#include "video_core/renderer_vulkan/pipeline_helper.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline_library.h"
#include "video_core/renderer_vulkan/vk_render_pass_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/surface.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {

struct ValidationCollector {
    std::mutex mutex;
    std::vector<std::string> issues;
};

VKAPI_ATTR VkBool32 VKAPI_CALL TestDebugCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity,
    [[maybe_unused]] VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* data, void* user_data) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)) != 0 &&
        data && data->pMessage) {
        auto* collector = static_cast<ValidationCollector*>(user_data);
        std::scoped_lock lk{collector->mutex};
        collector->issues.emplace_back(data->pMessage);
    }
    return VK_FALSE;
}

std::vector<u8> MinimalSm86ExitBytes() {
    const Shader::Ampere::Instruction exit_inst{{0x0000794d, 0, 0x03800000, 0}};
    std::vector<u8> bytes(sizeof(exit_inst));
    std::memcpy(bytes.data(), &exit_inst, sizeof(exit_inst));
    return bytes;
}

} // Anonymous namespace

TEST_CASE("Vulkan GraphicsPipelineLibrary builds, caches, prewarms, and validates cleanly",
          "[video_core][gpl][vulkan]") {
#ifdef _WIN32
    if (!std::getenv("VK_LAYER_PATH")) {
        const std::filesystem::path sdk_bin{"C:\\VulkanSDK\\1.4.363.1\\Bin"};
        if (std::filesystem::exists(sdk_bin / "VkLayer_khronos_validation.dll")) {
            _putenv_s("VK_LAYER_PATH", "C:\\VulkanSDK\\1.4.363.1\\Bin");
        }
    }
#endif

    const auto prev_gpl_setting = Settings::values.use_graphics_pipeline_library.GetValue();
    Settings::values.use_graphics_pipeline_library.SetValue(true);
    SCOPE_EXIT {
        Settings::values.use_graphics_pipeline_library.SetValue(prev_gpl_setting);
    };

    Vulkan::vk::InstanceDispatch dld{};
    auto library = Vulkan::OpenLibrary();
    if (!library) {
        SKIP("Vulkan loader dynamic library is unavailable");
    }

    Vulkan::vk::Instance instance;
    try {
        instance = Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1,
                                          Core::Frontend::WindowSystemType::Headless, true);
    } catch (const std::exception&) {
        try {
            instance = Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1,
                                              Core::Frontend::WindowSystemType::Headless, false);
        } catch (const std::exception& e) {
            SKIP(std::string("Unable to create Vulkan instance: ") + e.what());
        }
    }

    ValidationCollector collector;
    VkDebugUtilsMessengerEXT raw_messenger{VK_NULL_HANDLE};
    if (dld.vkCreateDebugUtilsMessengerEXT) {
        const VkDebugUtilsMessengerCreateInfoEXT msg_ci{
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT,
            .pfnUserCallback = &TestDebugCallback,
            .pUserData = &collector,
        };
        dld.vkCreateDebugUtilsMessengerEXT(*instance, &msg_ci, nullptr, &raw_messenger);
    }
    SCOPE_EXIT {
        if (raw_messenger != VK_NULL_HANDLE && dld.vkDestroyDebugUtilsMessengerEXT) {
            dld.vkDestroyDebugUtilsMessengerEXT(*instance, raw_messenger, nullptr);
        }
    };

    const auto physical_devices = instance.EnumeratePhysicalDevices();
    if (physical_devices.empty()) {
        SKIP("No Vulkan physical device present");
    }

    const Vulkan::vk::PhysicalDevice physical{physical_devices[0], dld};
    Vulkan::Device device(*instance, physical, VK_NULL_HANDLE, dld);

    const VkPipelineCacheCreateInfo cache_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .initialDataSize = 0,
        .pInitialData = nullptr,
    };
    Vulkan::vk::PipelineCache pipeline_cache = device.GetLogical().CreatePipelineCache(cache_ci);
    Vulkan::GraphicsPipelineLibraryCache gpl_cache{device, pipeline_cache};

    if (!device.IsExtGraphicsPipelineLibrarySupported()) {
        CHECK_FALSE(gpl_cache.IsSupported());
        Vulkan::GraphicsPipelineLibraryInput dummy{};
        CHECK_FALSE(static_cast<bool>(gpl_cache.BuildLinkedPipeline(dummy)));
        std::scoped_lock lk{collector.mutex};
        INFO("Validation messages count: " << collector.issues.size());
        CHECK(collector.issues.empty());
        return;
    }

    REQUIRE(gpl_cache.IsSupported());

    // Translate real minimal SM86 Vertex + Fragment shaders to SPIR-V via Dalia compiler.
    VideoCore::Precompile::PrecompilePools pools;
    const Shader::Profile profile{.supported_spirv = device.SupportedSpirvVersion()};
    const Shader::HostTranslateInfo host_info{};

    VideoCore::Precompile::ScannedShader scanned_vs{
        .key = 0x1001ULL,
        .program_bytes = MinimalSm86ExitBytes(),
        .stage = VideoCore::Precompile::ScannedStage::Vertex,
        .is_native_sm86 = true,
    };
    VideoCore::Precompile::ScannedShader scanned_fs{
        .key = 0x1002ULL,
        .program_bytes = MinimalSm86ExitBytes(),
        .stage = VideoCore::Precompile::ScannedStage::Fragment,
        .is_native_sm86 = true,
    };

    VideoCore::Precompile::TranslatedShader translated_vs{};
    VideoCore::Precompile::TranslatedShader translated_fs{};
    std::string err;
    REQUIRE(VideoCore::Precompile::TranslateScannedShader(pools, scanned_vs, profile, host_info,
                                                          49152, false, translated_vs, &err));
    REQUIRE(VideoCore::Precompile::TranslateScannedShader(pools, scanned_fs, profile, host_info,
                                                          49152, false, translated_fs, &err));

    Vulkan::vk::ShaderModule vs_mod = Vulkan::BuildShader(device, translated_vs.spirv);
    Vulkan::vk::ShaderModule fs_mod = Vulkan::BuildShader(device, translated_fs.spirv);

    Vulkan::RenderPassCache rp_cache{device};
    Vulkan::RenderPassKey color_rp_key{};
    color_rp_key.color_formats.fill(VideoCore::Surface::PixelFormat::Invalid);
    color_rp_key.color_formats[0] = VideoCore::Surface::PixelFormat::A8B8G8R8_UNORM;
    color_rp_key.depth_format = VideoCore::Surface::PixelFormat::D24_UNORM_S8_UINT;
    color_rp_key.samples = VK_SAMPLE_COUNT_1_BIT;
    const VkRenderPass color_rp = rp_cache.Get(color_rp_key);
    REQUIRE(color_rp != VK_NULL_HANDLE);

    Vulkan::RenderPassKey depth_rp_key{};
    depth_rp_key.color_formats.fill(VideoCore::Surface::PixelFormat::Invalid);
    depth_rp_key.depth_format = VideoCore::Surface::PixelFormat::D24_UNORM_S8_UINT;
    depth_rp_key.samples = VK_SAMPLE_COUNT_1_BIT;
    const VkRenderPass depth_rp = rp_cache.Get(depth_rp_key);
    REQUIRE(depth_rp != VK_NULL_HANDLE);

    const std::array<u64, 6> hashes{0ULL, 0x1001ULL, 0ULL, 0ULL, 0ULL, 0x1002ULL};
    auto bundle = gpl_cache.GetOrCreateLayout(hashes, 42ULL, [&] {
        Vulkan::DescriptorLayoutBuilder builder{device};
        builder.Add(translated_vs.info, VK_SHADER_STAGE_VERTEX_BIT);
        builder.Add(translated_fs.info, VK_SHADER_STAGE_FRAGMENT_BIT);
        auto b = std::make_shared<Vulkan::SharedPipelineLayoutBundle>();
        b->uses_push_descriptor = builder.CanUsePushDescriptor();
        b->descriptor_set_layout =
            builder.CreateDescriptorSetLayout(b->uses_push_descriptor, false);
        b->pipeline_layout = builder.CreatePipelineLayout(*b->descriptor_set_layout);
        return b;
    });
    REQUIRE(bundle != nullptr);

    const std::array<VkPipelineShaderStageCreateInfo, 2> stages{{
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = *vs_mod,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = *fs_mod,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
    }};

    VkPipelineVertexInputStateCreateInfo vi_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
    };
    VkPipelineInputAssemblyStateCreateInfo ia_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineTessellationStateCreateInfo tess_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO,
    };
    VkPipelineViewportStateCreateInfo vp_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rast_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineDepthStencilStateCreateInfo ds_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthCompareOp = VK_COMPARE_OP_ALWAYS,
    };
    VkPipelineColorBlendAttachmentState cb_att{
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                          VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo cb_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &cb_att,
    };
    const std::array<VkDynamicState, 2> dyn_states{
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };

    Vulkan::GraphicsPipelineLibraryInput input{
        .unique_hashes = hashes,
        .layout_bundle = bundle,
        .render_pass = color_rp,
        .shader_stages = stages,
        .vertex_input_ci = &vi_ci,
        .input_assembly_ci = &ia_ci,
        .tessellation_ci = &tess_ci,
        .viewport_ci = &vp_ci,
        .rasterization_ci = &rast_ci,
        .multisample_ci = &ms_ci,
        .depth_stencil_ci = &ds_ci,
        .color_blend_ci = &cb_ci,
        .dynamic_states = dyn_states,
    };

    // 1. 3-part library creation + linking
    Vulkan::vk::Pipeline linked_a = gpl_cache.BuildLinkedPipeline(input);
    REQUIRE(static_cast<bool>(linked_a));
    auto stats = gpl_cache.Stats();
    CHECK(stats.linked_pipelines == 1);
    CHECK(stats.vertex_input_libraries == 1);
    CHECK(stats.shader_libraries == 1);
    CHECK(stats.fragment_output_libraries == 1);
    CHECK(stats.fallback_monolithic == 0);

    // 2. Changed color blend mask reuses VertexInput & Shader libraries
    cb_att.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    Vulkan::vk::Pipeline linked_b = gpl_cache.BuildLinkedPipeline(input);
    REQUIRE(static_cast<bool>(linked_b));
    stats = gpl_cache.Stats();
    CHECK(stats.linked_pipelines == 2);
    CHECK(stats.vertex_input_hits >= 1);
    CHECK(stats.shader_hits >= 1);
    CHECK(stats.fragment_output_libraries == 2);

    // 3. Depth-only pipeline (Vertex stage only, no fragment stage)
    const std::array<u64, 6> depth_hashes{0ULL, 0x1001ULL, 0ULL, 0ULL, 0ULL, 0ULL};
    auto depth_bundle = gpl_cache.GetOrCreateLayout(depth_hashes, 1ULL, [&] {
        Vulkan::DescriptorLayoutBuilder builder{device};
        builder.Add(translated_vs.info, VK_SHADER_STAGE_VERTEX_BIT);
        auto b = std::make_shared<Vulkan::SharedPipelineLayoutBundle>();
        b->uses_push_descriptor = builder.CanUsePushDescriptor();
        b->descriptor_set_layout =
            builder.CreateDescriptorSetLayout(b->uses_push_descriptor, false);
        b->pipeline_layout = builder.CreatePipelineLayout(*b->descriptor_set_layout);
        return b;
    });
    VkPipelineColorBlendStateCreateInfo depth_cb_ci{
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 0,
        .pAttachments = nullptr,
    };
    Vulkan::GraphicsPipelineLibraryInput depth_input = input;
    depth_input.unique_hashes = depth_hashes;
    depth_input.layout_bundle = depth_bundle;
    depth_input.render_pass = depth_rp;
    depth_input.shader_stages = std::span{stages}.first(1);
    depth_input.color_blend_ci = &depth_cb_ci;
    Vulkan::vk::Pipeline depth_linked = gpl_cache.BuildLinkedPipeline(depth_input);
    REQUIRE(static_cast<bool>(depth_linked));

    // 4. Standalone PrewarmShaderStage (Vertex + Fragment)
    {
        Vulkan::DescriptorLayoutBuilder vs_b{device};
        vs_b.Add(translated_vs.info, VK_SHADER_STAGE_VERTEX_BIT);
        auto vs_dsl = vs_b.CreateDescriptorSetLayout(vs_b.CanUsePushDescriptor(), false);
        auto vs_pl = vs_b.CreatePipelineLayout(*vs_dsl);
        CHECK(gpl_cache.PrewarmShaderStage(VK_SHADER_STAGE_VERTEX_BIT, *vs_mod, *vs_pl, color_rp));

        Vulkan::DescriptorLayoutBuilder fs_b{device};
        fs_b.Add(translated_fs.info, VK_SHADER_STAGE_FRAGMENT_BIT);
        auto fs_dsl = fs_b.CreateDescriptorSetLayout(fs_b.CanUsePushDescriptor(), false);
        auto fs_pl = fs_b.CreatePipelineLayout(*fs_dsl);
        CHECK(gpl_cache.PrewarmShaderStage(VK_SHADER_STAGE_FRAGMENT_BIT, *fs_mod, *fs_pl,
                                             color_rp));
    }

    // 5. Destroy temporary pipeline handles and verify layout bundle survives cleanly.
    linked_a.reset();
    linked_b.reset();
    depth_linked.reset();
    bundle.reset();
    CHECK(gpl_cache.Stats().layout_bundles >= 2);

    std::scoped_lock lk{collector.mutex};
    for (const auto& issue : collector.issues) {
        UNSCOPED_INFO("Vulkan validation: " << issue);
    }
    CHECK(collector.issues.empty());
}

