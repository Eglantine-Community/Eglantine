// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2018 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <bit>
#include "common/algorithm.h"
#include "common/assert.h"
#include "common/logging.h"
#include "core/core.h"
#include "video_core/engines/kepler_memory.h"
#include "video_core/engines/maxwell_3d.h"
#include "video_core/gpu_split.h"
#include "video_core/memory_manager.h"
#include "video_core/rasterizer_interface.h"
#include "video_core/textures/decoders.h"

extern std::atomic<u64> g_nx2_watch_qmd_page;

namespace Tegra::Engines {

KeplerMemory::KeplerMemory(MemoryManager& memory_manager_)
    : memory_manager{memory_manager_}, upload_state{memory_manager_, regs.upload}
{}

KeplerMemory::~KeplerMemory() = default;

void KeplerMemory::BindRasterizer(VideoCore::RasterizerInterface* rasterizer_) {
    upload_state.BindRasterizer(rasterizer_);

    execution_mask.reset();
    execution_mask[KEPLERMEMORY_REG_INDEX(exec)] = true;
    execution_mask[KEPLERMEMORY_REG_INDEX(data)] = true;
}

void KeplerMemory::ConsumeSinkImpl(Core::System& system) {
    for (auto [method, value] : method_sink) {
        regs.reg_array[method] = value;
        front_regs.reg_array[method] = value;
    }
    method_sink.clear();
}

void KeplerMemory::FrontNoteWrite(VideoCommon::GpuSplit& split, u32 write_off, u32 byte_count) {
    const GPUVAddr dst = front_regs.upload.dest.Address();
    if (dst == 0) {
        return;
    }
    const auto to_cpu = [this, dst, write_off, byte_count](u64 gpu_addr) {
        const auto dev = memory_manager.GpuToCpuAddress(gpu_addr);
        if (dev && *dev != 0) {
            if (const u64 wp = g_nx2_watch_qmd_page.load(std::memory_order_relaxed);
                wp != 0 && (*dev & ~u64{0xfff}) == wp) {
                static u32 w_km{};
                if (++w_km <= 12) {
                    LOG_INFO(HW_GPU,
                             "Nx2WatchQmd KeplerMemory #{}: lin={} dst={:#x} off={:#x} bytes={} "
                             "lc={} ll={} dev={:#x}",
                             w_km, front_is_linear, dst, write_off, byte_count,
                             front_regs.upload.line_count, front_regs.upload.line_length_in, *dev);
                }
            }
        }
        return dev;
    };
    if (front_is_linear) {
        if (front_regs.upload.line_count == 1) {
            split.NoteWriterGpuRange(dst + write_off, byte_count, to_cpu);
        } else if (front_regs.upload.line_count > 1 && front_regs.upload.line_length_in > 0) {
            if (front_regs.upload.dest.pitch == front_regs.upload.line_length_in) {
                split.NoteWriterGpuRange(
                    dst, u64{front_regs.upload.line_length_in} * front_regs.upload.line_count,
                    to_cpu);
            } else if (front_regs.upload.line_count <= 32) {
                for (u32 line = 0; line < front_regs.upload.line_count; ++line) {
                    split.NoteWriterGpuRange(dst + u64{line} * front_regs.upload.dest.pitch,
                                             front_regs.upload.line_length_in, to_cpu);
                }
            } else {
                const u64 span =
                    u64{front_regs.upload.line_count - 1} * front_regs.upload.dest.pitch +
                    front_regs.upload.line_length_in;
                if (span <= 0x1000000ULL) {
                    split.NoteWriterGpuRange(dst, span, to_cpu);
                } else {
                    split.NoteWriter();
                }
            }
        }
        return;
    }
    u32 width = front_regs.upload.dest.width;
    u32 x_elements = front_regs.upload.line_length_in;
    u32 x_offset = front_regs.upload.dest.x;
    const u32 bpp_shift = Common::FoldRight(
        4U, [](u32 x, u32 y) { return (std::min)(x, static_cast<u32>(std::countr_zero(y))); },
        width, x_elements, x_offset, static_cast<u32>(dst));
    width >>= bpp_shift;
    const std::size_t dst_size = Tegra::Texture::CalculateSize(
        true, 1U << bpp_shift, width, front_regs.upload.dest.height, front_regs.upload.dest.depth,
        front_regs.upload.dest.BlockHeight(), front_regs.upload.dest.BlockDepth());
    split.NoteWriterGpuRange(dst, dst_size, to_cpu);
}

void KeplerMemory::FrontCallMethod(VideoCommon::GpuSplit& split, u32 method, u32 method_argument,
                                   bool is_last_call, const GPUVAddr* segment) {
    if (method < Regs::NUM_REGS) {
        front_regs.reg_array[method] = method_argument;
    }
    split.Method(this, method, method_argument, is_last_call, segment);
    if (method == KEPLERMEMORY_REG_INDEX(exec)) {
        front_write_offset = 0;
        front_copy_size = front_regs.upload.line_length_in * front_regs.upload.line_count;
        front_is_linear = front_regs.exec.linear != 0;
    } else if (method == KEPLERMEMORY_REG_INDEX(data)) {
        const u32 rem = front_copy_size > front_write_offset ? front_copy_size - front_write_offset
                                                             : 0U;
        const u32 sub = (std::min)(4U, rem);
        front_write_offset += sub;
        if (is_last_call) {
            const u32 bytes = front_copy_size > 0 ? front_copy_size : front_write_offset;
            if (bytes > 0) {
                FrontNoteWrite(split, 0, bytes);
            }
        }
    }
}

void KeplerMemory::FrontCallMultiMethod(VideoCommon::GpuSplit& split, u32 method,
                                        const u32* base_start, u32 amount, u32 methods_pending,
                                        const GPUVAddr* segment) {
    if (method == KEPLERMEMORY_REG_INDEX(data)) {
        split.MultiMethod(this, method, base_start, amount, methods_pending, segment);
        if (amount > 0) {
            front_regs.reg_array[method] = base_start[amount - 1];
            const u32 bytes = front_copy_size > 0
                                  ? front_copy_size
                                  : static_cast<u32>((std::min<u64>)(u64{amount} * sizeof(u32),
                                                                     u64{UINT32_MAX}));
            FrontNoteWrite(split, 0, bytes);
        }
        return;
    }
    for (u32 i = 0; i < amount; ++i) {
        FrontCallMethod(split, method, base_start[i], methods_pending - i <= 1, segment);
    }
}

void KeplerMemory::CallMethod(Core::System& system, u32 method, u32 method_argument, bool is_last_call) {
    ASSERT_MSG(method < Regs::NUM_REGS, "Invalid KeplerMemory register, increase the size of the Regs structure");

    regs.reg_array[method] = method_argument;
    if (!VideoCommon::GpuSplit::Get().Active()) {
        front_regs.reg_array[method] = method_argument;
    }

    switch (method) {
    case KEPLERMEMORY_REG_INDEX(exec): {
        upload_state.ProcessExec(regs.exec.linear != 0);
        break;
    }
    case KEPLERMEMORY_REG_INDEX(data): {
        upload_state.ProcessData(method_argument, is_last_call);
        break;
    }
    }
}

void KeplerMemory::CallMultiMethod(Core::System& system, u32 method, const u32* base_start, u32 amount, u32 methods_pending) {
    switch (method) {
    case KEPLERMEMORY_REG_INDEX(data):
        upload_state.ProcessData(base_start, amount);
        return;
    default:
        for (u32 i = 0; i < amount; i++) {
            CallMethod(system, method, base_start[i], methods_pending - i <= 1);
        }
        break;
    }
}

} // namespace Tegra::Engines
