// SPDX-FileCopyrightText: Copyright 2026 Dalia Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cmath>
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
#include "video_core/renderer_vulkan/present/fsr3_frame_gen.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_state_tracker.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"

namespace {

struct ValidationCollector {
    std::mutex mutex;
    std::vector<std::string> issues;
};

VKAPI_ATTR VkBool32 VKAPI_CALL DebugUtilsCallback(
    VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT type,
    const VkDebugUtilsMessengerCallbackDataEXT* callback_data, void* user_data) {
    if ((severity & (VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                     VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)) == 0) {
        return VK_FALSE;
    }
    if ((type & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) == 0 || !callback_data ||
        !callback_data->pMessage) {
        return VK_FALSE;
    }
    auto* collector = static_cast<ValidationCollector*>(user_data);
    std::scoped_lock lk{collector->mutex};
    collector->issues.emplace_back(callback_data->pMessage);
    return VK_FALSE;
}

Vulkan::vk::Image CreateColorImage(const Vulkan::MemoryAllocator& allocator, u32 width, u32 height,
                                   VkFormat format, VkImageUsageFlags usage) {
    return allocator.CreateImage({
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent =
            {
                .width = width,
                .height = height,
                .depth = 1,
            },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    });
}

Vulkan::vk::ImageView CreateColorView(const Vulkan::Device& device, VkImage image,
                                      VkFormat format) {
    return device.GetLogical().CreateImageView({
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .components =
            {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
        .subresourceRange =
            {
                .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = 1,
            },
    });
}

void ClearToGeneral(Vulkan::Scheduler& scheduler, VkImage image, VkExtent2D extent,
                    std::array<f32, 4> rgba) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image, extent, rgba](Vulkan::vk::CommandBuffer cmdbuf) {
        const VkImageSubresourceRange range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        const VkImageMemoryBarrier to_dst{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                               {}, {}, {}, to_dst);
        const VkClearColorValue clear_val{
            .float32 = {rgba[0], rgba[1], rgba[2], rgba[3]},
        };
        cmdbuf.ClearColorImage(image, VK_IMAGE_LAYOUT_GENERAL, clear_val, range);
        const VkImageMemoryBarrier to_read{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.PipelineBarrier(
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, {}, {}, {},
            to_read);
        (void)extent;
    });
}

std::array<u8, 4> ReadCenterPixelRgba(Vulkan::Scheduler& scheduler, VkImage image, VkExtent2D extent,
                                      VkFormat format, Vulkan::vk::Buffer& staging_buffer) {
    scheduler.RequestOutsideRenderPassOperationContext();
    scheduler.Record([image, extent, dst_buf = *staging_buffer](Vulkan::vk::CommandBuffer cmdbuf) {
        const VkImageSubresourceRange range{
            .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        };
        const VkImageMemoryBarrier to_src{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_GENERAL,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                               VK_PIPELINE_STAGE_TRANSFER_BIT, {}, {}, {}, to_src);
        const VkBufferImageCopy region{
            .bufferOffset = 0,
            .bufferRowLength = 0,
            .bufferImageHeight = 0,
            .imageSubresource =
                {
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT,
                    .mipLevel = 0,
                    .baseArrayLayer = 0,
                    .layerCount = 1,
                },
            .imageOffset = {0, 0, 0},
            .imageExtent = {extent.width, extent.height, 1},
        };
        cmdbuf.CopyImageToBuffer(image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dst_buf, region);
        const VkImageMemoryBarrier back_to_general{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = range,
        };
        cmdbuf.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                               {}, {}, {}, back_to_general);
    });
    scheduler.Finish();
    if (!staging_buffer.IsHostCoherent()) {
        staging_buffer.Invalidate();
    }
    const auto mapped = staging_buffer.Mapped();
    const size_t x = extent.width / 2;
    const size_t y = extent.height / 2;
    const size_t idx = (y * extent.width + x) * 4;
    if (format == VK_FORMAT_B8G8R8A8_UNORM) {
        return {mapped[idx + 2], mapped[idx + 1], mapped[idx + 0], mapped[idx + 3]};
    }
    return {mapped[idx + 0], mapped[idx + 1], mapped[idx + 2], mapped[idx + 3]};
}

} // namespace

TEST_CASE("FSR3 FrameGen interpolates history and validates cleanly on swapchain formats",
          "[video_core][frame_gen][vulkan]") {
#ifdef _WIN32
    if (!std::getenv("VK_LAYER_PATH")) {
        const std::filesystem::path sdk_bin{"C:\\VulkanSDK\\1.4.363.1\\Bin"};
        if (std::filesystem::exists(sdk_bin / "VkLayer_khronos_validation.dll")) {
            _putenv_s("VK_LAYER_PATH", "C:\\VulkanSDK\\1.4.363.1\\Bin");
        }
    }
#endif

    Vulkan::vk::InstanceDispatch dld{};
    auto library = Vulkan::OpenLibrary();
    if (!library) {
        SKIP("Vulkan loader dynamic library is unavailable");
    }

    bool validation_enabled = true;
    Vulkan::vk::Instance instance;
    try {
        instance = Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1,
                                          Core::Frontend::WindowSystemType::Headless, true);
    } catch (const std::exception&) {
        validation_enabled = false;
        try {
            instance = Vulkan::CreateInstance(*library, dld, VK_API_VERSION_1_1,
                                              Core::Frontend::WindowSystemType::Headless, false);
        } catch (const std::exception& e) {
            SKIP(std::string("Unable to create Vulkan instance: ") + e.what());
        }
    }

    ValidationCollector collector;
    Vulkan::vk::DebugUtilsMessenger messenger;
    if (validation_enabled && dld.vkCreateDebugUtilsMessengerEXT) {
        messenger = instance.CreateDebugUtilsMessenger({
            .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
            .pNext = nullptr,
            .flags = 0,
            .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT |
                               VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT,
            .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT,
            .pfnUserCallback = DebugUtilsCallback,
            .pUserData = &collector,
        });
    }

    const auto physical_devices = instance.EnumeratePhysicalDevices();
    if (physical_devices.empty()) {
        SKIP("No Vulkan physical devices available");
    }

    Vulkan::vk::PhysicalDevice physical{physical_devices[0], dld};
    Vulkan::Device device{*instance, physical, VK_NULL_HANDLE, dld};
    Vulkan::MemoryAllocator memory_allocator{device};
    Vulkan::StateTracker state_tracker;
    Vulkan::Scheduler scheduler{device, state_tracker};

    constexpr VkExtent2D extent{.width = 64, .height = 64};
    constexpr VkDeviceSize readback_bytes = extent.width * extent.height * 4;
    const VkImageUsageFlags image_usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                          VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                          VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    for (const VkFormat format : {VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_R8G8B8A8_UNORM}) {
        INFO("Testing format " << static_cast<u32>(format));

        Vulkan::vk::Buffer download_buffer = memory_allocator.CreateBuffer(
            {
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = readback_bytes,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            Vulkan::MemoryUsage::Download);

        auto frame_a = CreateColorImage(memory_allocator, extent.width, extent.height, format,
                                        image_usage);
        auto frame_b = CreateColorImage(memory_allocator, extent.width, extent.height, format,
                                        image_usage);
        auto frame_c = CreateColorImage(memory_allocator, extent.width, extent.height, format,
                                        image_usage);
        auto destination = CreateColorImage(memory_allocator, extent.width, extent.height, format,
                                            image_usage);

        auto view_a = CreateColorView(device, *frame_a, format);
        auto view_b = CreateColorView(device, *frame_b, format);
        auto view_c = CreateColorView(device, *frame_c, format);
        auto view_dst = CreateColorView(device, *destination, format);

        ClearToGeneral(scheduler, *frame_a, extent, {20.0f / 255.0f, 40.0f / 255.0f, 220.0f / 255.0f, 1.0f});
        ClearToGeneral(scheduler, *frame_b, extent, {220.0f / 255.0f, 200.0f / 255.0f, 40.0f / 255.0f, 1.0f});
        ClearToGeneral(scheduler, *frame_c, extent, {100.0f / 255.0f, 100.0f / 255.0f, 100.0f / 255.0f, 1.0f});

        Vulkan::Fsr3FrameGen fsr3{device, memory_allocator, extent, format};
        CHECK_FALSE(fsr3.IsReady());

        // Frame 0 (previous = A), Frame 1 (current = B) -> midpoint blend should be ~(120, 120, 130)
        fsr3.Process(device, scheduler, *frame_a, *view_a, 0);
        CHECK_FALSE(fsr3.IsReady());
        fsr3.Process(device, scheduler, *frame_b, *view_b, 1);
        REQUIRE(fsr3.IsReady());
        fsr3.Generate(device, scheduler, *destination, *view_dst, 0.5f, 1.0f);

        const auto mid_ab = ReadCenterPixelRgba(scheduler, *destination, extent, format,
                                                download_buffer);
        INFO("mid_ab RGBA = (" << int(mid_ab[0]) << ", " << int(mid_ab[1]) << ", "
                               << int(mid_ab[2]) << ", " << int(mid_ab[3]) << ")");
        CHECK(std::abs(static_cast<int>(mid_ab[0]) - 120) <= 6);
        CHECK(std::abs(static_cast<int>(mid_ab[1]) - 120) <= 6);
        CHECK(std::abs(static_cast<int>(mid_ab[2]) - 130) <= 6);

        // Frame 2 (previous = B, current = C) -> test ping-pong swap + descriptor cache eviction
        fsr3.Process(device, scheduler, *frame_c, *view_c, 2);
        for (int iter = 0; iter < 18; ++iter) {
            fsr3.Generate(device, scheduler, *destination, *view_dst, 0.25f, 1.0f);
        }
        // blend_factor = 0.25 between B(220,200,40) and C(100,100,100) => ~(190, 175, 55)
        const auto quarter_bc = ReadCenterPixelRgba(scheduler, *destination, extent, format,
                                                    download_buffer);
        INFO("quarter_bc RGBA = (" << int(quarter_bc[0]) << ", " << int(quarter_bc[1]) << ", "
                                   << int(quarter_bc[2]) << ", " << int(quarter_bc[3]) << ")");
        CHECK(std::abs(static_cast<int>(quarter_bc[0]) - 190) <= 6);
        CHECK(std::abs(static_cast<int>(quarter_bc[1]) - 175) <= 6);
        CHECK(std::abs(static_cast<int>(quarter_bc[2]) - 55) <= 6);

        scheduler.Finish();
    }

    std::scoped_lock lk{collector.mutex};
    for (const auto& issue : collector.issues) {
        UNSCOPED_INFO("Vulkan validation: " << issue);
    }
    CHECK(collector.issues.empty());
}
