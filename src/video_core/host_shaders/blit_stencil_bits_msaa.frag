// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

#version 450 core

layout(binding = 1) uniform usampler2DMS stencil_tex;

layout(push_constant) uniform Mask {
    layout(offset = 16) uint bit_mask;
};

layout(location = 0) in vec2 texcoord;

void main() {
    if ((texelFetch(stencil_tex, ivec2(texcoord), 0).r & bit_mask) == 0u) {
        discard;
    }
}
