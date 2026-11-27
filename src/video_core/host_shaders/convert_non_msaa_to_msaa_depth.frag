// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 450 core
#extension GL_GOOGLE_include_directive : require

layout(binding = 0) uniform sampler2D img_in;

#include "msaa_sample_grid.glsl"

void main() {
    const ivec2 coord = SourceSampleCoord();
    gl_FragDepth = texelFetch(img_in, coord, 0).r;
}
