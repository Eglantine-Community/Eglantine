// SPDX-License-Identifier: GPL-3.0-or-later

layout(push_constant) uniform PushConstants {
    ivec2 dst_offset;
    ivec2 src_offset;
    ivec2 scale;
    uint stencil_bit;
    ivec2 clip_origin;
    uvec2 clip_extent;
};

ivec2 SourceSampleCoord() {
    const ivec2 pixel = ivec2(gl_FragCoord.xy) - dst_offset;
    const ivec2 sample_offset = ivec2(gl_SampleID % scale.x, gl_SampleID / scale.x);
    const ivec2 coord = pixel * scale + sample_offset + src_offset;
    if (clip_extent.x != 0u && any(greaterThanEqual(uvec2(coord - clip_origin), clip_extent))) {
        discard;
    }
    return coord;
}
