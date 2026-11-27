// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 450 core
#extension GL_GOOGLE_include_directive : require

#ifndef SAMPLER_TYPE
#define SAMPLER_TYPE sampler2D
#endif
#ifndef TEXEL_TYPE
#define TEXEL_TYPE vec4
#endif

layout(binding = 0) uniform SAMPLER_TYPE img_in;

#include "msaa_sample_grid.glsl"

layout(location = 0) out TEXEL_TYPE frag_color;

void main() {
    const ivec2 coord = SourceSampleCoord();
    frag_color = texelFetch(img_in, coord, 0);
}
