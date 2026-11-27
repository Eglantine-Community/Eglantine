// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 450 core
#extension GL_GOOGLE_include_directive : require
#extension GL_ARB_shader_stencil_export : require

layout(binding = 0) uniform sampler2D depth_tex;
layout(binding = 1) uniform usampler2D stencil_tex;

#include "msaa_sample_grid.glsl"

void main() {
    const ivec2 coord = SourceSampleCoord();
    gl_FragDepth = texelFetch(depth_tex, coord, 0).r;
    gl_FragStencilRefARB = int(texelFetch(stencil_tex, coord, 0).r);
}
