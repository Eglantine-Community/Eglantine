// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 450 core

layout(binding = 1) uniform usampler2DMS stencil_tex;

layout(push_constant) uniform PushConstants {
    ivec2 dst_offset;
    ivec2 src_offset;
    ivec2 scale;
    uint stencil_bit;
};

void main() {
    const ivec2 coord = ivec2(gl_FragCoord.xy) - dst_offset + src_offset;
    const ivec2 msaa_coord = coord / scale;
    const ivec2 sample_offset = coord % scale;
    const int sample_id = sample_offset.x + scale.x * sample_offset.y;
    if ((texelFetch(stencil_tex, msaa_coord, sample_id).r & stencil_bit) == 0u) {
        discard;
    }
}
