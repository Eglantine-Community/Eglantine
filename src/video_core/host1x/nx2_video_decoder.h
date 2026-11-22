// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common/common_types.h"

namespace FFmpeg {
class Frame;
}

namespace Tegra::Host1x {

/// A picture decoded by Nx2VideoDecoder, in display order.
struct Nx2Picture {
    u32 width{};
    u32 height{};
    u64 pts{};
    std::shared_ptr<FFmpeg::Frame> frame;
};

enum class Nx2PictureFormat : u32 {
    Nv12,
    Rgba8,
    Bgra8,
};

enum class Nx2GpuDecodeFault {
    None,
    Open,
    Packet,
    Busy,
    NoCpu,
    Replay,
    Hold,
    ReplayReceive,
    SwitchReceive,
};

bool IsH264Stream(std::span<const u8> data);

class Nx2VideoDecoder {
public:
    /// mime: the codec name nn::codec sends with Configure ("video/vp9", "video/vp8", ...).
    explicit Nx2VideoDecoder(std::string_view mime,
                             Nx2GpuDecodeFault fault = Nx2GpuDecodeFault::None);
    ~Nx2VideoDecoder();

    void SetGpuDecodeFault(Nx2GpuDecodeFault fault);

    bool IsOpen() const;

    const std::string& Name() const;

    void Decode(std::span<const u8> data, u64 pts, std::vector<Nx2Picture>& pictures);

    void Drain(std::vector<Nx2Picture>& pictures);

    void Reset();

    void Render(const Nx2Picture& picture, Nx2PictureFormat format, u32 width, u32 height,
                u64 pitch, u64 chroma_offset, std::span<u8> out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
