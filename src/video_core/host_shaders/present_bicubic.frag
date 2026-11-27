// SPDX-FileCopyrightText: Copyright 2025 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later
#version 460 core
layout (location = 0) in vec2 frag_tex_coord;
layout (location = 0) out vec4 color;
layout (binding = 0) uniform sampler2D color_texture;

vec4 sampleRow(sampler2D samp, vec3 x, float y, vec3 weights) {
    return textureLod(samp, vec2(x[0], y), 0.0) * weights[0] +
           textureLod(samp, vec2(x[1], y), 0.0) * weights[1] +
           textureLod(samp, vec2(x[2], y), 0.0) * weights[2];
}

vec4 textureBicubic(sampler2D samp, vec2 uv) {
    vec2 tex_size = vec2(textureSize(samp, 0));
    vec2 texel = uv * tex_size - 0.5;
    vec2 center = floor(texel) + 0.5;
    vec2 t = fract(texel);
    vec2 t2 = t * t;
    vec2 t3 = t2 * t;
    vec2 w0 = -0.5 * t + t2 - 0.5 * t3;
    vec2 w1 = 1.0 - 2.5 * t2 + 1.5 * t3;
    vec2 w2 = 0.5 * t + 2.0 * t2 - 1.5 * t3;
    vec2 w3 = -0.5 * t2 + 0.5 * t3;

    // The outer Catmull-Rom weights are negative. Only combine the two
    // middle taps with bilinear sampling, keeping the outer taps separate.
    vec2 middle_weight = w1 + w2;
    vec2 inv_size = 1.0 / tex_size;
    vec2 p0 = (center - 1.0) * inv_size;
    vec2 p12 = (center + w2 / middle_weight) * inv_size;
    vec2 p3 = (center + 2.0) * inv_size;
    vec3 x = vec3(p0.x, p12.x, p3.x);
    vec3 wx = vec3(w0.x, middle_weight.x, w3.x);
    return sampleRow(samp, x, p0.y, wx) * w0.y +
           sampleRow(samp, x, p12.y, wx) * middle_weight.y +
           sampleRow(samp, x, p3.y, wx) * w3.y;
}

void main() {
    color = textureBicubic(color_texture, frag_tex_coord);
}
