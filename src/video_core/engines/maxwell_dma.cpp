// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <bit>
#include <cstdlib>
#include "common/algorithm.h"
#include "common/assert.h"
#include "common/logging.h"
#include <ranges>
#include "common/settings.h"
#include "core/core.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/engines/maxwell_dma.h"
#include "video_core/gpu_split.h"
#include "video_core/guest_memory.h"
#include "video_core/memory_manager.h"
#include "video_core/renderer_base.h"
#include "video_core/textures/decoders.h"

extern std::atomic<u64> g_nx2_watch_qmd_page;

namespace Tegra::Engines {

using namespace Texture;

MaxwellDMA::MaxwellDMA(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_}
{
    execution_mask.reset();
    execution_mask[offsetof(Regs, launch_dma) / sizeof(u32)] = true;
}

MaxwellDMA::~MaxwellDMA() = default;

void MaxwellDMA::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    rasterizer = rasterizer_;
}

void MaxwellDMA::ConsumeSinkImpl(Core::System& system) {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
        front_regs.reg_array[method] = value;
    }
    method_sink.clear();
}

void MaxwellDMA::FrontNoteLaunchDma(VideoCommon::GpuSplit& split) {
    const LaunchDMA& launch = front_regs.launch_dma;
    const GPUVAddr dst = front_regs.offset_out;
    const auto to_cpu = [this, dst, &launch](u64 gpu_addr) {
        const auto dev = memory_manager.GpuToCpuAddress(gpu_addr);
        if (dev && *dev != 0) {
            if (const u64 wp = g_nx2_watch_qmd_page.load(std::memory_order_relaxed);
                wp != 0 && (*dev & ~u64{0xfff}) == wp) {
                static u32 w_dma{};
                if (++w_dma <= 12) {
                    LOG_INFO(HW_GPU,
                             "Nx2WatchQmd MaxwellDMA #{}: ml={} rem={} sem={} dst={:#x} ll={} lc={} "
                             "gpu={:#x} dev={:#x}",
                             w_dma, launch.multi_line_enable.Value(), launch.remap_enable.Value(),
                             static_cast<u32>(launch.semaphore_type.Value()), dst,
                             front_regs.line_length_in, front_regs.line_count, gpu_addr, *dev);
                }
            }
        }
        return dev;
    };
    if (launch.multi_line_enable) {
        const bool is_src_pitch = launch.src_memory_layout == LaunchDMA::MemoryLayout::PITCH;
        const bool is_dst_pitch = launch.dst_memory_layout == LaunchDMA::MemoryLayout::PITCH;
        if (is_dst_pitch) {
            if (front_regs.line_count > 0 && front_regs.line_length_in > 0 && dst != 0) {
                if (front_regs.pitch_out < 0) {
                    split.NoteWriter();
                } else {
                    const u32 pitch = static_cast<u32>(front_regs.pitch_out);
                    if (front_regs.line_count == 1 || pitch == front_regs.line_length_in) {
                        split.NoteWriterGpuRange(
                            dst, u64{front_regs.line_length_in} * front_regs.line_count, to_cpu);
                    } else if (front_regs.line_count <= 32) {
                        for (u32 line = 0; line < front_regs.line_count; ++line) {
                            split.NoteWriterGpuRange(dst + u64{line} * pitch,
                                                     front_regs.line_length_in, to_cpu);
                        }
                    } else {
                        const u64 span = u64{front_regs.line_count - 1} * pitch +
                                         front_regs.line_length_in;
                        if (span <= 0x1000000ULL) {
                            split.NoteWriterGpuRange(dst, span, to_cpu);
                        } else {
                            split.NoteWriter();
                        }
                    }
                }
            }
        } else if (dst != 0) {
            const bool is_remapping = launch.remap_enable != 0;
            const u32 num_remap_components = front_regs.remap_const.num_dst_components_minus_one + 1;
            const u32 remap_components_size = front_regs.remap_const.component_size_minus_one + 1;
            const u32 base_bpp = !is_remapping ? 1U : num_remap_components * remap_components_size;
            u32 dst_width = front_regs.dst_params.width;
            u32 bpp_shift = 0U;
            if (!is_remapping) {
                if (!is_src_pitch) {
                    bpp_shift = Common::FoldRight(
                        4U,
                        [](u32 x, u32 y) {
                            return (std::min)(x, static_cast<u32>(std::countr_zero(y)));
                        },
                        front_regs.src_params.width, dst_width, front_regs.line_length_in,
                        front_regs.src_params.origin.x, front_regs.dst_params.origin.x,
                        static_cast<u32>(front_regs.offset_in), static_cast<u32>(dst));
                } else {
                    bpp_shift = Common::FoldRight(
                        4U,
                        [](u32 x, u32 y) {
                            return (std::min)(x, static_cast<u32>(std::countr_zero(y)));
                        },
                        dst_width, front_regs.line_length_in, front_regs.dst_params.origin.x,
                        static_cast<u32>(dst));
                }
                dst_width >>= bpp_shift;
            }
            const size_t dst_size =
                CalculateSize(true, base_bpp << bpp_shift, dst_width, front_regs.dst_params.height,
                              front_regs.dst_params.depth, front_regs.dst_params.block_size.height,
                              front_regs.dst_params.block_size.depth);
            split.NoteWriterGpuRange(dst, dst_size, to_cpu);
        }
    } else if (dst != 0 && front_regs.line_length_in > 0) {
        const bool is_const_a_dst = front_regs.remap_const.dst_x == RemapConst::Swizzle::CONST_A;
        if (launch.remap_enable != 0 && is_const_a_dst) {
            const u32 component_size = front_regs.remap_const.component_size_minus_one + 1;
            split.NoteWriterGpuRange(dst, u64{front_regs.line_length_in} * component_size, to_cpu);
        } else {
            const GPUVAddr aligned_dst = dst & ~GPUVAddr{0x1ff};
            const u64 aligned_bytes =
                ((dst - aligned_dst) + front_regs.line_length_in + 0x1ffULL) & ~0x1ffULL;
            split.NoteWriterGpuRange(aligned_dst, aligned_bytes, to_cpu);
        }
    }
    switch (launch.semaphore_type) {
    case LaunchDMA::SemaphoreType::NONE:
        break;
    case LaunchDMA::SemaphoreType::RELEASE_ONE_WORD_SEMAPHORE:
        if (front_regs.semaphore.address != 0) {
            split.NoteWriterGpuRange(front_regs.semaphore.address, sizeof(u32), to_cpu);
        }
        break;
    case LaunchDMA::SemaphoreType::RELEASE_FOUR_WORD_SEMAPHORE:
        if (front_regs.semaphore.address != 0) {
            split.NoteWriterGpuRange(front_regs.semaphore.address, 16, to_cpu);
        }
        break;
    default:
        split.NoteWriter();
        break;
    }
}

void MaxwellDMA::FrontCallMethod(VideoCommon::GpuSplit& split, u32 method, u32 method_argument,
                                 bool is_last_call, const GPUVAddr* segment) {
    if (method < NUM_REGS) {
        front_regs.reg_array[method] = method_argument;
    }
    split.Method(this, method, method_argument, is_last_call, segment);
    if (method == offsetof(Regs, launch_dma) / sizeof(u32)) {
        FrontNoteLaunchDma(split);
    }
}

void MaxwellDMA::FrontCallMultiMethod(VideoCommon::GpuSplit& split, u32 method,
                                      const u32* base_start, u32 amount, u32 methods_pending,
                                      const GPUVAddr* segment) {
    split.MultiMethod(this, method, base_start, amount, methods_pending, segment);
    if (method >= NUM_REGS) {
        return;
    }
    const bool is_launch = (method == offsetof(Regs, launch_dma) / sizeof(u32));
    for (u32 i = 0; i < amount; ++i) {
        front_regs.reg_array[method] = base_start[i];
        if (is_launch) {
            FrontNoteLaunchDma(split);
        }
    }
}

void MaxwellDMA::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    if (method >= NUM_REGS) {
        // Ampere copy (Switch 2) can use methods past this register file; don't write past it.
        LOG_DEBUG(HW_GPU, "Ignoring MaxwellDMA method {:#x} past the register file", method);
        return;
    }

    regs.reg_array[method] = method_argument;
    if (!VideoCommon::GpuSplit::Get().Active()) {
        front_regs.reg_array[method] = method_argument;
    }

    if (method == offsetof(Regs, launch_dma) / sizeof(u32)) {
        Launch();
    }
}

void MaxwellDMA::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    for (u32 i = 0; i < amount; ++i) {
        CallMethod(system, method, base_start[i], methods_pending - i <= 1);
    }
}

void MaxwellDMA::Launch() {
    LOG_TRACE(Render_OpenGL, "DMA copy {:#x} -> {:#x}", static_cast<GPUVAddr>(regs.offset_in), GPUVAddr(regs.offset_out));
    // EDEN_NX2_TRACE_DST=<hex>: also log copies writing within 32 MiB above that GPU address.
    static const u64 trace_destination = [] {
        const char* value = std::getenv("EDEN_NX2_TRACE_DST");
        return value != nullptr ? std::strtoull(value, nullptr, 16) : 0ULL;
    }();
    if (trace_destination != 0 && GPUVAddr(regs.offset_out) >= trace_destination &&
        GPUVAddr(regs.offset_out) < trace_destination + (32ULL << 20)) {
        static u32 traced_destination{};
        if (traced_destination++ < 40) {
            LOG_INFO(HW_GPU,
                     "nx2 DMA to traced dst: {:#x} -> {:#x} lines={} length={} pitch in={:#x} "
                     "out={:#x} launch={:#x} src_layout={} dst_layout={}",
                     static_cast<GPUVAddr>(regs.offset_in), GPUVAddr(regs.offset_out),
                     regs.line_count, regs.line_length_in, regs.pitch_in, regs.pitch_out,
                     regs.reg_array[0xc0], regs.launch_dma.src_memory_layout.Value() == LaunchDMA::MemoryLayout::PITCH ? "pitch" : "block",
                     regs.launch_dma.dst_memory_layout.Value() == LaunchDMA::MemoryLayout::PITCH ? "pitch" : "block");
        }
    }
    if (static const bool trace = std::getenv("EDEN_NX2_RT_TRACE") != nullptr; trace) {
        static u32 traced{};
        if (traced++ < 64) {
            LOG_INFO(HW_GPU, "nx2 DMA {:#x} -> {:#x} lines={} length={} launch={:#x}",
                     static_cast<GPUVAddr>(regs.offset_in), GPUVAddr(regs.offset_out),
                     regs.line_count, regs.line_length_in, regs.reg_array[0xc0]);
        }
    }

    // TODO(Subv): Perform more research and implement all features of this engine.
    const LaunchDMA& launch = regs.launch_dma;
    ASSERT(launch.interrupt_type == LaunchDMA::InterruptType::NONE);

    if (launch.multi_line_enable) {
        const bool is_src_pitch = launch.src_memory_layout == LaunchDMA::MemoryLayout::PITCH;
        const bool is_dst_pitch = launch.dst_memory_layout == LaunchDMA::MemoryLayout::PITCH;
        memory_manager.FlushCaching();
        if (!is_src_pitch && !is_dst_pitch) {
            // If both the source and the destination are in block layout, assert.
            CopyBlockLinearToBlockLinear();
            ReleaseSemaphore();
            return;
        }

        if (is_src_pitch && is_dst_pitch) {
            for (u32 line = 0; line < regs.line_count; ++line) {
                const GPUVAddr source_line =
                    regs.offset_in + static_cast<size_t>(line) * regs.pitch_in;
                const GPUVAddr dest_line =
                    regs.offset_out + static_cast<size_t>(line) * regs.pitch_out;
                memory_manager.CopyBlock(dest_line, source_line, regs.line_length_in);
            }
        } else {
            if (!is_src_pitch && is_dst_pitch) {
                CopyBlockLinearToPitch();
            } else {
                CopyPitchToBlockLinear();
            }
        }
    } else {
        // TODO: allow multisized components.
        auto& accelerate = rasterizer->AccessAccelerateDMA();
        const bool is_const_a_dst = regs.remap_const.dst_x == RemapConst::Swizzle::CONST_A;
        if (regs.launch_dma.remap_enable != 0 && is_const_a_dst) {
            const u32 component_size = regs.remap_const.component_size_minus_one + 1;
            ASSERT(component_size == 1 || component_size == 2 || component_size == 4);
            if (component_size == 4) {
                accelerate.BufferClear(regs.offset_out, regs.line_length_in, regs.remap_const.remap_consta_value);
            }
            read_buffer.resize_destructive(regs.line_length_in * sizeof(u32));
            std::ranges::fill(std::span<u32>(reinterpret_cast<u32*>(read_buffer.data()), regs.line_length_in), regs.remap_const.remap_consta_value);
            memory_manager.WriteBlockUnsafe(regs.offset_out, reinterpret_cast<u8*>(read_buffer.data()), static_cast<size_t>(regs.line_length_in) * component_size);
        } else {
            memory_manager.FlushCaching();
            const auto convert_linear_2_blocklinear_addr = [](u64 address) {
                return (address & ~0x1f0ULL) | ((address & 0x40) >> 2) | ((address & 0x10) << 1) |
                       ((address & 0x180) >> 1) | ((address & 0x20) << 3);
            };
            const auto src_kind = memory_manager.GetPageKind(regs.offset_in);
            const auto dst_kind = memory_manager.GetPageKind(regs.offset_out);
            const bool is_src_pitch = IsPitchKind(src_kind);
            const bool is_dst_pitch = IsPitchKind(dst_kind);
            if (!is_src_pitch && is_dst_pitch) {
                UNIMPLEMENTED_IF(regs.line_length_in % 16 != 0);
                UNIMPLEMENTED_IF(regs.offset_in % 16 != 0);
                UNIMPLEMENTED_IF(regs.offset_out % 16 != 0);
                read_buffer.resize_destructive(16);
                for (u32 offset = 0; offset < regs.line_length_in; offset += 16) {
                    Tegra::Memory::GpuGuestMemoryScoped<
                        u8, Tegra::Memory::GuestMemoryFlags::SafeReadCachedWrite>
                        tmp_write_buffer(memory_manager,
                                         convert_linear_2_blocklinear_addr(regs.offset_in + offset),
                                         16, &read_buffer);
                    tmp_write_buffer.SetAddressAndSize(regs.offset_out + offset, 16);
                }
            } else if (is_src_pitch && !is_dst_pitch) {
                UNIMPLEMENTED_IF(regs.line_length_in % 16 != 0);
                UNIMPLEMENTED_IF(regs.offset_in % 16 != 0);
                UNIMPLEMENTED_IF(regs.offset_out % 16 != 0);
                read_buffer.resize_destructive(16);
                for (u32 offset = 0; offset < regs.line_length_in; offset += 16) {
                    Tegra::Memory::GpuGuestMemoryScoped<
                        u8, Tegra::Memory::GuestMemoryFlags::SafeReadCachedWrite>
                        tmp_write_buffer(memory_manager, regs.offset_in + offset, 16, &read_buffer);
                    tmp_write_buffer.SetAddressAndSize(
                        convert_linear_2_blocklinear_addr(regs.offset_out + offset), 16);
                }
            } else {
                if (!accelerate.BufferCopy(regs.offset_in, regs.offset_out, regs.line_length_in)) {
                    Tegra::Memory::GpuGuestMemoryScoped<
                        u8, Tegra::Memory::GuestMemoryFlags::SafeReadCachedWrite>
                        tmp_write_buffer(memory_manager, regs.offset_in, regs.line_length_in,
                                         &read_buffer);
                    tmp_write_buffer.SetAddressAndSize(regs.offset_out, regs.line_length_in);
                }
            }
        }
    }

    ReleaseSemaphore();
}

void MaxwellDMA::CopyBlockLinearToPitch() {
   

    u32 bytes_per_pixel = 1;
    DMA::ImageOperand src_operand;
    src_operand.bytes_per_pixel = bytes_per_pixel;
    src_operand.params = regs.src_params;
    src_operand.address = regs.offset_in;

    DMA::BufferOperand dst_operand;
    dst_operand.pitch = static_cast<u32>(std::abs(regs.pitch_out));
    dst_operand.width = regs.line_length_in;
    dst_operand.height = regs.line_count;
    dst_operand.address = regs.offset_out;
    DMA::ImageCopy copy_info{};
    copy_info.length_x = regs.line_length_in;
    copy_info.length_y = regs.line_count;
    auto& accelerate = rasterizer->AccessAccelerateDMA();
    if (accelerate.ImageToBuffer(copy_info, src_operand, dst_operand)) {
        return;
    }

    UNIMPLEMENTED_IF(regs.src_params.block_size.width != 0);
    UNIMPLEMENTED_IF(regs.src_params.block_size.depth != 0);
    UNIMPLEMENTED_IF(regs.src_params.block_size.depth == 0 && regs.src_params.depth != 1);

    // Deswizzle the input and copy it over.
    const DMA::Parameters& src_params = regs.src_params;

    const bool is_remapping = regs.launch_dma.remap_enable != 0;

    const u32 num_remap_components = regs.remap_const.num_dst_components_minus_one + 1;
    const u32 remap_components_size = regs.remap_const.component_size_minus_one + 1;

    const u32 base_bpp = !is_remapping ? 1U : num_remap_components * remap_components_size;

    u32 width = src_params.width;
    u32 x_elements = regs.line_length_in;
    u32 x_offset = src_params.origin.x;
    u32 bpp_shift = 0U;
    if (!is_remapping) {
        bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return (std::min)(x, static_cast<u32>(std::countr_zero(y))); },
            width, x_elements, x_offset, static_cast<u32>(regs.offset_in));
        width >>= bpp_shift;
        x_elements >>= bpp_shift;
        x_offset >>= bpp_shift;
    }

    bytes_per_pixel = base_bpp << bpp_shift;
    const u32 height = src_params.height;
    const u32 depth = src_params.depth;
    const u32 block_height = src_params.block_size.height;
    const u32 block_depth = src_params.block_size.depth;
    const size_t src_size =
        CalculateSize(true, bytes_per_pixel, width, height, depth, block_height, block_depth);

    const size_t dst_size = dst_operand.pitch * regs.line_count;

    Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::SafeRead> tmp_read_buffer(
        memory_manager, src_operand.address, src_size, &read_buffer);
    Tegra::Memory::GpuGuestMemoryScoped<u8, Tegra::Memory::GuestMemoryFlags::UnsafeReadCachedWrite>
        tmp_write_buffer(memory_manager, dst_operand.address, dst_size, &write_buffer);

    UnswizzleSubrect(tmp_write_buffer, tmp_read_buffer, bytes_per_pixel, width, height, depth,
                     x_offset, src_params.origin.y, x_elements, regs.line_count, block_height,
                     block_depth, dst_operand.pitch, memory_manager.UsesNativeTiling());
}

void MaxwellDMA::CopyPitchToBlockLinear() {
    UNIMPLEMENTED_IF_MSG(regs.dst_params.block_size.width != 0, "Block width is not one");
    UNIMPLEMENTED_IF(regs.dst_params.layer != 0);

    const bool is_remapping = regs.launch_dma.remap_enable != 0;
    const u32 num_remap_components = regs.remap_const.num_dst_components_minus_one + 1;
    const u32 remap_components_size = regs.remap_const.component_size_minus_one + 1;

    u32 bytes_per_pixel = 1;
    DMA::ImageOperand dst_operand;
    dst_operand.bytes_per_pixel = bytes_per_pixel;
    dst_operand.params = regs.dst_params;
    dst_operand.address = regs.offset_out;
    DMA::BufferOperand src_operand;
    src_operand.pitch = regs.pitch_in;
    src_operand.width = regs.line_length_in;
    src_operand.height = regs.line_count;
    src_operand.address = regs.offset_in;
    DMA::ImageCopy copy_info{};
    copy_info.length_x = regs.line_length_in;
    copy_info.length_y = regs.line_count;
    auto& accelerate = rasterizer->AccessAccelerateDMA();
    if (accelerate.BufferToImage(copy_info, src_operand, dst_operand)) {
        return;
    }

    const auto& dst_params = regs.dst_params;

    const u32 base_bpp = !is_remapping ? 1U : num_remap_components * remap_components_size;

    u32 width = dst_params.width;
    u32 x_elements = regs.line_length_in;
    u32 x_offset = dst_params.origin.x;
    u32 bpp_shift = 0U;
    if (!is_remapping) {
        bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return (std::min)(x, static_cast<u32>(std::countr_zero(y))); },
            width, x_elements, x_offset, static_cast<u32>(regs.offset_out));
        width >>= bpp_shift;
        x_elements >>= bpp_shift;
        x_offset >>= bpp_shift;
    }

    bytes_per_pixel = base_bpp << bpp_shift;
    const u32 height = dst_params.height;
    const u32 depth = dst_params.depth;
    const u32 block_height = dst_params.block_size.height;
    const u32 block_depth = dst_params.block_size.depth;
    const size_t dst_size =
        CalculateSize(true, bytes_per_pixel, width, height, depth, block_height, block_depth);
    const size_t src_size = static_cast<size_t>(regs.pitch_in) * regs.line_count;

    GPUVAddr src_addr = regs.offset_in;
    GPUVAddr dst_addr = regs.offset_out;
    Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::SafeRead> tmp_read_buffer(
        memory_manager, src_addr, src_size, &read_buffer);
    Tegra::Memory::GpuGuestMemoryScoped<u8, Tegra::Memory::GuestMemoryFlags::UnsafeReadCachedWrite>
        tmp_write_buffer(memory_manager, dst_addr, dst_size, &write_buffer);

    //  If the input is linear and the output is tiled, swizzle the input and copy it over.
    SwizzleSubrect(tmp_write_buffer, tmp_read_buffer, bytes_per_pixel, width, height, depth,
                   x_offset, dst_params.origin.y, x_elements, regs.line_count, block_height,
                   block_depth, regs.pitch_in, memory_manager.UsesNativeTiling());
}

void MaxwellDMA::CopyBlockLinearToBlockLinear() {
    UNIMPLEMENTED_IF(regs.src_params.block_size.width != 0);

    const bool is_remapping = regs.launch_dma.remap_enable != 0;

    // Deswizzle the input and copy it over.
    const DMA::Parameters& src = regs.src_params;
    const DMA::Parameters& dst = regs.dst_params;

    const u32 num_remap_components = regs.remap_const.num_dst_components_minus_one + 1;
    const u32 remap_components_size = regs.remap_const.component_size_minus_one + 1;

    const u32 base_bpp = !is_remapping ? 1U : num_remap_components * remap_components_size;

    u32 src_width = src.width;
    u32 dst_width = dst.width;
    u32 x_elements = regs.line_length_in;
    u32 src_x_offset = src.origin.x;
    u32 dst_x_offset = dst.origin.x;
    u32 bpp_shift = 0U;
    if (!is_remapping) {
        bpp_shift = Common::FoldRight(
            4U, [](u32 x, u32 y) { return (std::min)(x, static_cast<u32>(std::countr_zero(y))); },
            src_width, dst_width, x_elements, src_x_offset, dst_x_offset,
            static_cast<u32>(regs.offset_in), static_cast<u32>(regs.offset_out));
        src_width >>= bpp_shift;
        dst_width >>= bpp_shift;
        x_elements >>= bpp_shift;
        src_x_offset >>= bpp_shift;
        dst_x_offset >>= bpp_shift;
    }

    const u32 bytes_per_pixel = base_bpp << bpp_shift;
    const size_t src_size = CalculateSize(true, bytes_per_pixel, src_width, src.height, src.depth,
                                          src.block_size.height, src.block_size.depth);
    const size_t dst_size = CalculateSize(true, bytes_per_pixel, dst_width, dst.height, dst.depth,
                                          dst.block_size.height, dst.block_size.depth);

    const u32 pitch = x_elements * bytes_per_pixel;
    const size_t mid_buffer_size = pitch * regs.line_count;

    intermediate_buffer.resize_destructive(mid_buffer_size);

    Tegra::Memory::GpuGuestMemory<u8, Tegra::Memory::GuestMemoryFlags::SafeRead> tmp_read_buffer(
        memory_manager, regs.offset_in, src_size, &read_buffer);
    Tegra::Memory::GpuGuestMemoryScoped<u8, Tegra::Memory::GuestMemoryFlags::SafeReadCachedWrite>
        tmp_write_buffer(memory_manager, regs.offset_out, dst_size, &write_buffer);

    UnswizzleSubrect(intermediate_buffer, tmp_read_buffer, bytes_per_pixel, src_width, src.height,
                     src.depth, src_x_offset, src.origin.y, x_elements, regs.line_count,
                     src.block_size.height, src.block_size.depth, pitch, memory_manager.UsesNativeTiling());

    SwizzleSubrect(tmp_write_buffer, intermediate_buffer, bytes_per_pixel, dst_width, dst.height,
                   dst.depth, dst_x_offset, dst.origin.y, x_elements, regs.line_count,
                   dst.block_size.height, dst.block_size.depth, pitch, memory_manager.UsesNativeTiling());
}

void MaxwellDMA::ReleaseSemaphore() {
    const auto type = regs.launch_dma.semaphore_type;
    const GPUVAddr address = regs.semaphore.address;
    const u32 payload = regs.semaphore.payload;
    VideoCommon::QueryPropertiesFlags flags{VideoCommon::QueryPropertiesFlags::IsAFence};
    switch (type) {
    case LaunchDMA::SemaphoreType::NONE:
        break;
    case LaunchDMA::SemaphoreType::RELEASE_ONE_WORD_SEMAPHORE: {
        rasterizer->Query(address, VideoCommon::QueryType::Payload, flags, payload, 0);
        break;
    }
    case LaunchDMA::SemaphoreType::RELEASE_FOUR_WORD_SEMAPHORE: {
        rasterizer->Query(address, VideoCommon::QueryType::Payload,
                          flags | VideoCommon::QueryPropertiesFlags::HasTimeout, payload, 0);
        break;
    }
    default:
        ASSERT_MSG(false, "Unknown semaphore type: {}", static_cast<u32>(type.Value()));
        break;
    }
}

} // namespace Tegra::Engines
