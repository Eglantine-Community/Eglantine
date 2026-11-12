// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include "common/scope_exit.h"
#include "common/settings.h"
#include "core/core.h"
#include "core/device_memory.h"
#include "core/hle/kernel/k_page_table.h"
#include "core/hle/kernel/k_process.h"
#include "core/memory.h"
#include "video_core/host1x/gpu_device_memory_manager.h"
#include "video_core/memory_manager.h"
#include "video_core/texture_cache/image_info.h"
#include "video_core/texture_cache/util.h"
#include "video_core/textures/decoders.h"

TEST_CASE("Direct row uploads match whole-image bytes across mip and layer boundaries",
          "[texture_row_upload]") {
    using VideoCore::Surface::PixelFormat;
    const auto format =
        GENERATE(PixelFormat::R8_UNORM, PixelFormat::R16_UNORM, PixelFormat::A8B8G8R8_UNORM,
                 PixelFormat::R32G32_FLOAT, PixelFormat::R32G32B32A32_FLOAT,
                 PixelFormat::BC1_RGBA_UNORM, PixelFormat::BC3_UNORM);
    const bool native = GENERATE(false, true);
    const auto block_height = GENERATE(0U, 2U, 4U);
    const auto spacing = GENERATE(0U, 1U);
    const bool partial = GENERATE(false, true);
    CAPTURE(format, native, block_height, spacing, partial);

    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT { Settings::values.use_multi_core.SetValue(old_multi_core); };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);

    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.format = format;
    info.size = {258, 137, 1};
    info.resources = {5, 3};
    info.block.height = block_height;
    info.tile_width_spacing = spacing;
    info.layer_stride = VideoCommon::CalculateLayerStride(info);

    const size_t guest_bytes = VideoCommon::CalculateGuestSizeInBytes(info);
    const size_t mapped_bytes = Common::AlignUp(guest_bytes, 0x10000ULL);
    constexpr u64 phys = 0x80000, cpu_guest = 0x200000;
    constexpr GPUVAddr address = 0x1000000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, mapped_bytes, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{cpu_guest}, mapped_bytes,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager device_manager(system.DeviceMemory());
    device_manager.Map(0x100000, cpu_guest, mapped_bytes, device_manager.RegisterProcess(&cpu));
    Tegra::MemoryManager gpu_memory(system, device_manager);
    if (native) {
        gpu_memory.EnableNativeTiling();
    }
    gpu_memory.Map(address, 0x100000, mapped_bytes);

    std::vector<u8> guest(guest_bytes);
    u32 random = 0x9128ebf;
    for (auto& byte : guest) {
        random ^= random << 13;
        random ^= random >> 17;
        random ^= random << 5;
        byte = static_cast<u8>(random);
    }
    gpu_memory.WriteBlockUnsafe(address, guest.data(), guest.size());

    std::vector<u8> whole(VideoCommon::CalculateUnswizzledSizeBytes(info));
    const auto reference = VideoCommon::UnswizzleImage(gpu_memory, address, info, guest, whole);
    const u64 middle = guest.size() / 2;
    const std::vector<std::pair<u64, u64>> ranges =
        partial ? std::vector<std::pair<u64, u64>>{{511, 1025},
                                                   {middle - 1, middle + 513},
                                                   {guest.size() - 517, guest.size()}}
                : std::vector<std::pair<u64, u64>>{{0, guest.size()}};

    const auto plan = VideoCommon::PlanUnswizzleImageRows(address, info, ranges);
    REQUIRE(plan.layout_valid);
    REQUIRE_FALSE(plan.copies.empty());
    std::vector<u8> storage(plan.total_host_bytes, 0xa5);
    VideoCommon::ExecuteUnswizzlePlan(gpu_memory, plan, storage);

    const VideoCommon::Extent2D block{VideoCore::Surface::DefaultBlockWidth(format),
                                      VideoCore::Surface::DefaultBlockHeight(format)};
    const auto bpp = VideoCore::Surface::BytesPerBlock(format);
    size_t end = 0;
    for (const auto& copy : plan.copies) {
        REQUIRE(copy.buffer_offset == end);
        end += copy.buffer_size;
        REQUIRE(end <= storage.size());
        const auto& ref = reference[copy.image_subresource.base_level];
        const auto row_bytes = ((copy.image_extent.width + block.width - 1) / block.width) * bpp;
        const auto rows = (copy.image_extent.height + block.height - 1) / block.height;
        const auto host_pitch = (copy.buffer_row_length / block.width) * bpp;
        const auto ref_pitch = ((ref.buffer_row_length + block.width - 1) / block.width) * bpp;
        const auto ref_rows = (ref.buffer_image_height + block.height - 1) / block.height;
        const auto ref_offset =
            ref.buffer_offset +
            static_cast<size_t>(copy.image_subresource.base_layer) * ref_pitch * ref_rows +
            static_cast<size_t>(copy.image_offset.y / static_cast<s32>(block.height)) * ref_pitch +
            static_cast<size_t>(copy.image_offset.x / static_cast<s32>(block.width)) * bpp;
        for (u32 row = 0; row < rows; ++row) {
            REQUIRE(ref_offset + row * ref_pitch + row_bytes <= whole.size());
            CHECK(std::memcmp(storage.data() + copy.buffer_offset + row * host_pitch,
                              whole.data() + ref_offset + row * ref_pitch, row_bytes) == 0);
        }
    }
    CHECK(end == storage.size());
}

TEST_CASE("Unsupported and untouched row uploads stay empty", "[texture_row_upload]") {
    VideoCommon::ImageInfo info;
    info.type = VideoCommon::ImageType::e2D;
    info.format = VideoCore::Surface::PixelFormat::R8_UNORM;
    info.size = {64, 64, 1};
    info.resources = {1, 1};
    const std::array<std::pair<u64, u64>, 1> outside{{{1ULL << 32, (1ULL << 32) + 1}}};
    const auto plan_outside = VideoCommon::PlanUnswizzleImageRows(0, info, outside);
    CHECK(plan_outside.runs.empty());
    CHECK(plan_outside.copies.empty());
    const auto plan_empty = VideoCommon::PlanUnswizzleImageRows(0, info, {});
    CHECK(plan_empty.runs.empty());
    CHECK(plan_empty.copies.empty());
    info.type = VideoCommon::ImageType::e3D;
    const std::array<std::pair<u64, u64>, 1> all{{{0, 4096}}};
    const auto plan_3d = VideoCommon::PlanUnswizzleImageRows(0, info, all);
    CHECK(plan_3d.runs.empty());
    CHECK(plan_3d.copies.empty());
}

TEST_CASE("Switch 2 headers asking for Maxwell GOBs decode without the bit 5/6 swap",
          "[texture_row_upload]") {
    Tegra::Texture::TICEntry tic{};
    tic.format.Assign(Tegra::Texture::TextureFormat::R8);
    tic.r_type.Assign(Tegra::Texture::ComponentType::UNORM);
    tic.g_type.Assign(Tegra::Texture::ComponentType::UNORM);
    tic.b_type.Assign(Tegra::Texture::ComponentType::UNORM);
    tic.a_type.Assign(Tegra::Texture::ComponentType::UNORM);
    tic.header_version.Assign(Tegra::Texture::TICHeaderVersion::BlockLinear);
    tic.texture_type.Assign(Tegra::Texture::TextureType::Texture2D);
    tic.block_height.Assign(1);
    tic.width_minus_one.Assign(63);
    tic.height_minus_1.Assign(15);
    const bool maxwell = GENERATE(false, true);
    tic.load_store_hint.Assign(maxwell ? 1 : 0);
    const VideoCommon::ImageInfo info{tic};
    REQUIRE(info.maxwell_gobs == maxwell);
    REQUIRE(VideoCommon::UsesNativeGobs(true, info) == !maxwell);
    REQUIRE_FALSE(VideoCommon::UsesNativeGobs(false, info));
    REQUIRE(VideoCommon::UsesNativeGobs(true, VideoCommon::ImageInfo{}));

    const bool old_multi_core = Settings::values.use_multi_core.GetValue();
    SCOPE_EXIT { Settings::values.use_multi_core.SetValue(old_multi_core); };
    Settings::values.use_multi_core.SetValue(false);
    Core::System system;
    system.Initialize();
    Kernel::KProcess process(system.Kernel());
    SCOPE_EXIT { system.Kernel().UnregisterKernelObject(&process); };
    auto& cpu = process.GetMemory();
    auto& table = process.GetPageTable().GetImpl();
    table.Resize(32, 12);
    cpu.SetCurrentPageTable(process);
    constexpr u64 phys = 0x80000, cpu_guest = 0x200000, mapped = 0x10000;
    constexpr GPUVAddr address = 0x1000000;
    system.DeviceMemory().buffer.ClearBackingRegion(phys, mapped, 0);
    cpu.MapMemoryRegion(table, Common::ProcessAddress{cpu_guest}, mapped,
                        Common::PhysicalAddress{Core::DramMemoryMap::Base + phys},
                        Common::MemoryPermission::ReadWrite, false);
    Tegra::MaxwellDeviceMemoryManager device_manager(system.DeviceMemory());
    device_manager.Map(0x100000, cpu_guest, mapped, device_manager.RegisterProcess(&cpu));
    Tegra::MemoryManager gpu_memory(system, device_manager);
    gpu_memory.EnableNativeTiling();
    gpu_memory.Map(address, 0x100000, mapped);

    std::vector<u8> linear(64 * 16);
    for (size_t i = 0; i < linear.size(); ++i) {
        linear[i] = static_cast<u8>(i * 7 + i / 64);
    }
    std::vector<u8> tiled(2 * 512);
    Tegra::Texture::SwizzleTexture(tiled, linear, 1, 64, 16, 1, 1, 0, 1, !maxwell);
    gpu_memory.WriteBlockUnsafe(address, tiled.data(), tiled.size());

    std::vector<u8> decoded(linear.size());
    (void)VideoCommon::UnswizzleImage(gpu_memory, address, info, tiled, decoded);
    REQUIRE(decoded == linear);

    const std::array<std::pair<u64, u64>, 1> all{{{0, tiled.size()}}};
    const auto plan = VideoCommon::PlanUnswizzleImageRows(address, info, all);
    REQUIRE(plan.layout_valid);
    REQUIRE(plan.maxwell_gobs == maxwell);
    std::vector<u8> decoded_rows(plan.total_host_bytes);
    VideoCommon::ExecuteUnswizzlePlan(gpu_memory, plan, decoded_rows);
    REQUIRE(decoded_rows == linear);
}

