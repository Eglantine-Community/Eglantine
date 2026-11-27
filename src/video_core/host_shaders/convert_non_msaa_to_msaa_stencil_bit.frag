// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 450 core
#extension GL_GOOGLE_include_directive : require

layout(binding = 1) uniform usampler2D stencil_tex;

#include "msaa_sample_grid.glsl"

void main() {
    const ivec2 coord = SourceSampleCoord();
    if ((texelFetch(stencil_tex, coord, 0).r & stencil_bit) == 0u) {
        discard;
    }
}
