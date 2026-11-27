// SPDX-FileCopyrightText: Copyright 2026 Vortex727 (FUKKO)
// SPDX-License-Identifier: GPL-3.0-or-later

// The GPU ASTC decoder of the NVN replay (nvn_astc_gpu.h) against what the guest's bytes mean:
// block-linear (Switch 2 GOB order) void-extent ASTC blocks, each with its own colour, over several
// layers and levels of a texture whose sides are not block multiples. Needs a GPU (SKIP without).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <memory>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/common_types.h"
#include "video_core/renderer_vulkan/nvn/nvn_astc_gpu.h"
#include "video_core/renderer_vulkan/nvn/nvn_sparse_layout.h"
#include "video_core/renderer_vulkan/nvn/nvn_texture_layout.h"
#include "video_core/textures/decoders.h"
#include "video_core/vulkan_common/vulkan_device.h"
#include "video_core/vulkan_common/vulkan_instance.h"
#include "video_core/vulkan_common/vulkan_library.h"
#include "video_core/vulkan_common/vulkan_memory_allocator.h"
#include "video_core/vulkan_common/vulkan_wrapper.h"
#include "video_core/vulkan_common/vma.h"

namespace {

using Vulkan::Nvn::AstcGpuDecoder;

struct HeadlessVulkan {
    std::shared_ptr<Common::DynamicLibrary> library;
    std::unique_ptr<Vulkan::vk::InstanceDispatch> dld;
    Vulkan::vk::Instance instance;
    std::unique_ptr<Vulkan::Device> device;
};

std::unique_ptr<HeadlessVulkan> MakeHeadlessVulkan() {
    try {
        auto result = std::make_unique<HeadlessVulkan>();
        result->library = Vulkan::OpenLibrary(nullptr);
        if (!result->library) {
            return nullptr;
        }
        result->dld = std::make_unique<Vulkan::vk::InstanceDispatch>();
        result->instance = Vulkan::CreateInstance(*result->library, *result->dld,
                                                  VK_API_VERSION_1_1,
                                                  Core::Frontend::WindowSystemType::Headless, false);
        const std::vector<VkPhysicalDevice> devices = result->instance.EnumeratePhysicalDevices();
        if (devices.empty()) {
            return nullptr;
        }
        result->device = std::make_unique<Vulkan::Device>(
            *result->instance, Vulkan::vk::PhysicalDevice(devices[0], *result->dld),
            VK_NULL_HANDLE, *result->dld);
        return result;
    } catch (const Vulkan::vk::Exception&) {
        return nullptr;
    } catch (const std::exception&) {
        return nullptr;
    }
}

/// A void-extent LDR block (spec 'Void-Extent Blocks') of one colour, 16 bits per channel.
std::array<u8, 16> VoidExtentBlock(const std::array<u16, 4>& rgba) {
    u64 low = 0x1FC;                  // block mode: void extent
    low |= u64{3} << 10;              // reserved bits are ones
    low |= u64{0x1FFF} << 12;         // no extent: every coordinate field is all ones
    low |= u64{0x1FFF} << 25;
    low |= u64{0x1FFF} << 38;
    low |= u64{0x1FFF} << 51;
    u64 high = 0;
    for (u32 i = 0; i < 4; ++i) {
        high |= u64{rgba[i]} << (16 * i);
    }
    std::array<u8, 16> block;
    std::memcpy(block.data(), &low, 8);
    std::memcpy(block.data() + 8, &high, 8);
    return block;
}

u8 ToUnorm8(u16 value) {
    return static_cast<u8>(std::lround(static_cast<double>(value) / 65535.0 * 255.0));
}

} // Anonymous namespace

TEST_CASE("NVN ASTC GPU: block-linear blocks decode into the right texels of every level and layer",
          "[nvn][astc-gpu]") {
    using namespace Vulkan;
    auto vulkan = MakeHeadlessVulkan();
    if (!vulkan) {
        SKIP("no Vulkan device");
    }
    const Device& device = *vulkan->device;
    if (!AstcGpuDecoder::Supported(device)) {
        SKIP("the device cannot run the GPU ASTC decoder");
    }

    const bool sparse_tile = GENERATE(false, true);
    INFO("sparse tile " << sparse_tile);
    const s32 format = sparse_tile ? 0x80 : 0x79; // ASTC 8x8 or 4x4 UNORM
    Nvn::Storage atlas;
    std::optional<Nvn::SparseLayout> sparse_layout;
    Nvn::SparseRect tile;
    if (sparse_tile) {
        atlas.desc = {.target = static_cast<s32>(Nvn::TexTarget::Dim2D),
                      .format = format,
                      .flags = Nvn::TexFlag::Sparse,
                      .width = 733,
                      .height = 789,
                      .depth = 1,
                      .levels = 1};
        atlas.layout = Nvn::ComputeTextureLayout(atlas.desc);
        atlas.type = VK_IMAGE_TYPE_2D;
        atlas.samples = VK_SAMPLE_COUNT_1_BIT;
        atlas.extent = {atlas.desc.width, atlas.desc.height, 1};
        atlas.layers = atlas.levels = 1;
        sparse_layout = Nvn::BuildSparseLayout(atlas);
        REQUIRE(sparse_layout);
        tile = sparse_layout->TileRect({0, 0, 1, 1});
        REQUIRE(tile.width < sparse_layout->tile_width);
        REQUIRE(tile.height < sparse_layout->tile_height);
        CHECK(u64{sparse_layout->tile_width} * sparse_layout->tile_height * 4 == (1u << 20));
    }
    const u32 WIDTH = sparse_tile ? tile.width : 70;
    const u32 HEIGHT = sparse_tile ? tile.height : 45;
    const u32 LEVELS = sparse_tile ? 1 : 4;
    const u32 LAYERS = sparse_tile ? 1 : 3;
    Nvn::TextureLayoutDesc desc{
        .target = static_cast<s32>(Nvn::TexTarget::Dim2DArray),
        .format = format,
        .flags = 0,
        .samples = 0,
        .width = WIDTH,
        .height = HEIGHT,
        .depth = LAYERS,
        .levels = LEVELS,
        .stride = 0,
        .pkg_block_h = 0,
    };
    const Nvn::TextureLayout layout = Nvn::ComputeTextureLayout(desc);
    const Nvn::BlockInfo block = Nvn::FormatBlockInfo(format);
    REQUIRE(block.bytes == 16);
    REQUIRE(layout.layers == LAYERS);

    // Guest bytes: every block of every level and layer gets its own random colour, swizzled the
    // way the game stores it.
    std::mt19937 random(1234);
    std::vector<u8> guest(sparse_tile ? Nvn::SPARSE_TILE_BYTES : layout.size + 4096, 0);
    struct Expected {
        u32 level;
        u32 layer;
        u32 width;
        u32 height;
        std::vector<u8> texels; // RGBA8, tightly packed
    };
    std::vector<Expected> expected;
    std::vector<AstcGpuDecoder::Level> levels;
    for (u32 level = 0; level < LEVELS; ++level) {
        const u32 width = (std::max)(WIDTH >> level, 1u);
        const u32 height = (std::max)(HEIGHT >> level, 1u);
        const u32 columns = sparse_tile ? sparse_layout->shape.tile_cols
                                        : (width + block.width - 1) / block.width;
        const u32 rows = sparse_tile ? sparse_layout->shape.tile_rows
                                     : (height + block.height - 1) / block.height;
        u32 tile_h = 0;
        u32 tile_d = 0;
        Nvn::TextureLevelTiles(desc, level, tile_h, tile_d);
        if (sparse_tile) {
            tile_h = sparse_layout->shape.tile_h;
            tile_d = 0;
        }
        for (u32 layer = 0; layer < LAYERS; ++layer) {
            std::vector<u8> linear(size_t{columns} * rows * 16);
            Expected texels{level, layer, width, height,
                            std::vector<u8>(size_t{width} * height * 4)};
            for (u32 row = 0; row < rows; ++row) {
                for (u32 column = 0; column < columns; ++column) {
                    const std::array<u16, 4> color{static_cast<u16>(random()),
                                                   static_cast<u16>(random()),
                                                   static_cast<u16>(random()),
                                                   static_cast<u16>(random())};
                    const auto encoded = VoidExtentBlock(color);
                    std::memcpy(&linear[(size_t{row} * columns + column) * 16], encoded.data(), 16);
                    for (u32 y = row * block.height; y < (std::min)((row + 1) * block.height, height);
                         ++y) {
                        for (u32 x = column * block.width;
                             x < (std::min)((column + 1) * block.width, width); ++x) {
                            u8* texel = &texels.texels[(size_t{y} * width + x) * 4];
                            for (u32 c = 0; c < 4; ++c) {
                                texel[c] = ToUnorm8(color[c]);
                            }
                        }
                    }
                }
            }
            const u64 offset = u64{layer} * layout.layer_size + Nvn::TextureLevelOffset(desc, level);
            Tegra::Texture::SwizzleTexture(
                std::span<u8>{guest.data() + offset, guest.size() - static_cast<size_t>(offset)},
                linear, 16, columns, rows, 1, tile_h, tile_d, 0, true);
            expected.push_back(std::move(texels));
        }
        levels.push_back(sparse_tile ? Nvn::SparseAstcTileLevel(*sparse_layout, tile)
                                     : AstcGpuDecoder::Level{
            .level = level,
            .layers = LAYERS,
            .width = width,
            .height = height,
            .offset = Nvn::TextureLevelOffset(desc, level),
            .layer_stride = static_cast<u32>(layout.layer_size),
            .block_width = block.width,
            .block_height = block.height,
            .tile_height = tile_h,
            .tile_depth = tile_d,
        });
    }
    if (sparse_tile) {
        CHECK(AstcGpuDecoder::RequiredBytes(levels.front()) == Nvn::SPARSE_TILE_BYTES);
    }

    VmaAllocator vma = device.CreateThreadSafeAllocator();
    {
        MemoryAllocator allocator(device, vma);
        const AstcGpuDecoder decoder(device);
        for (const AstcGpuDecoder::Level& level : levels) {
            REQUIRE(level.offset % decoder.OffsetAlignment() == 0);
        }

        vk::Buffer input = allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = guest.size(),
                .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Upload);
        std::memcpy(input.Mapped().data(), guest.data(), guest.size());
        input.Flush();

        // An oversized scratch, with UNORM storage views over an sRGB image.
        vk::Image image = allocator.CreateImage(VkImageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT,
            .imageType = VK_IMAGE_TYPE_2D,
            .format = VK_FORMAT_A8B8G8R8_SRGB_PACK32,
            .extent = {WIDTH + 64, HEIGHT + 64, 1},
            .mipLevels = LEVELS,
            .arrayLayers = LAYERS,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .tiling = VK_IMAGE_TILING_OPTIMAL,
            .usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                     VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
            .queueFamilyIndexCount = 0,
            .pQueueFamilyIndices = nullptr,
            .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        });

        // One download buffer holds every (level, layer) region back to back.
        u64 download_size = 0;
        std::vector<VkBufferImageCopy> regions;
        for (const Expected& texels : expected) {
            regions.push_back({
                .bufferOffset = download_size,
                .bufferRowLength = 0,
                .bufferImageHeight = 0,
                .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, texels.level, texels.layer, 1},
                .imageOffset = {0, 0, 0},
                .imageExtent = {texels.width, texels.height, 1},
            });
            download_size += texels.texels.size();
        }
        const u64 guard_offset = download_size;
        for (const Expected& texels : expected) {
            const s32 x = static_cast<s32>(texels.width);
            const s32 y = static_cast<s32>(texels.height);
            const s32 bx = static_cast<s32>(block.width);
            const s32 by = static_cast<s32>(block.height);
            for (const VkOffset3D guard : {VkOffset3D{x, 0, 0},
                                           VkOffset3D{(x + bx - 1) / bx * bx, 0, 0},
                                           VkOffset3D{0, y, 0},
                                           VkOffset3D{0, (y + by - 1) / by * by, 0}}) {
                regions.push_back({
                    .bufferOffset = download_size,
                    .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, texels.level, texels.layer, 1},
                    .imageOffset = guard,
                    .imageExtent = {1, 1, 1},
                });
                download_size += 4;
            }
        }
        vk::Buffer download = allocator.CreateBuffer(
            VkBufferCreateInfo{
                .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
                .size = download_size,
                .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
                .queueFamilyIndexCount = 0,
                .pQueueFamilyIndices = nullptr,
            },
            MemoryUsage::Download);

        const vk::Device& logical = device.GetLogical();
        const std::array pool_sizes{
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, LEVELS},
            VkDescriptorPoolSize{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, LEVELS},
        };
        auto descriptors = logical.CreateDescriptorPool({
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
            .maxSets = LEVELS,
            .poolSizeCount = static_cast<u32>(pool_sizes.size()),
            .pPoolSizes = pool_sizes.data(),
        });
        const auto allocate_set = [&](VkDescriptorSetLayout layout) {
            const VkDescriptorSetAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = *descriptors,
                .descriptorSetCount = 1,
                .pSetLayouts = &layout,
            };
            VkDescriptorSet set = VK_NULL_HANDLE;
            vk::Check(device.GetDispatchLoader().vkAllocateDescriptorSets(*logical, &info, &set));
            return set;
        };
        vk::CommandPool pool = logical.CreateCommandPool({
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .queueFamilyIndex = device.GetGraphicsFamily(),
        });
        vk::CommandBuffers buffers = pool.Allocate(1);
        const VkCommandBuffer raw = buffers[0];
        vk::CommandBuffer cmd{raw, device.GetDispatchLoader()};
        cmd.Begin({
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        });
        const VkImageMemoryBarrier to_general{
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = 0,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_GENERAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = *image,
            .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0,
                                 VK_REMAINING_ARRAY_LAYERS},
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                            0, {}, {}, to_general);
        const VkClearColorValue guard_color{.float32 = {1.0f, 0.0f, 1.0f, 1.0f}};
        cmd.ClearColorImage(*image, VK_IMAGE_LAYOUT_GENERAL, guard_color, to_general.subresourceRange);
        const VkMemoryBarrier after_clear{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0,
                            after_clear);
        std::vector<vk::ImageView> views;
        const auto first_level = std::span{levels}.first(1);
        REQUIRE_FALSE(decoder.Record(cmd, *input, 0,
                                     AstcGpuDecoder::RequiredBytes(first_level.front()) - 16,
                                     *image, first_level, views, allocate_set));
        REQUIRE(views.empty());
        REQUIRE(decoder.Record(cmd, *input, 0, guest.size(), *image, levels, views, allocate_set));
        REQUIRE(views.size() == levels.size());
        const VkMemoryBarrier to_transfer{
            .sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        };
        cmd.PipelineBarrier(VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                            VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                            to_transfer, {}, {});
        cmd.CopyImageToBuffer(*image, VK_IMAGE_LAYOUT_GENERAL, *download, regions);
        cmd.End();
        vk::Fence fence = logical.CreateFence({
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        });
        const VkSubmitInfo submit{
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &raw,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
        REQUIRE(device.GetGraphicsQueue().Submit(submit, *fence) == VK_SUCCESS);
        REQUIRE(fence.Wait() == VK_SUCCESS);
        download.Invalidate();

        const u8* actual = download.Mapped().data();
        size_t wrong = 0;
        for (const Expected& texels : expected) {
            for (size_t i = 0; i < texels.texels.size(); ++i) {
                const int difference = std::abs(int(actual[i]) - int(texels.texels[i]));
                if (difference > 1) {
                    if (wrong < 8) {
                        WARN("level " << texels.level << " layer " << texels.layer << " texel "
                                      << i / 4 % texels.width << "," << i / 4 / texels.width
                                      << " channel " << i % 4 << ": gpu " << int(actual[i])
                                      << " expected " << int(texels.texels[i]));
                    }
                    ++wrong;
                }
            }
            actual += texels.texels.size();
        }
        REQUIRE(wrong == 0);
        constexpr std::array<u8, 4> guard_pixel{255, 0, 255, 255};
        for (u64 offset = guard_offset; offset < download_size; offset += guard_pixel.size()) {
            CHECK(std::equal(guard_pixel.begin(), guard_pixel.end(), download.Mapped().data() + offset));
        }
    }
    vmaDestroyAllocator(vma);
}
