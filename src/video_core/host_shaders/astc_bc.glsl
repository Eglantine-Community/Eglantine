// SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later

/* The fused pass's body, included by astc_bc.comp: an invocation makes one 4x4 BC block straight
   from the ASTC blocks its sixteen texels lie in.

   The two-pass path (TranscodeAstcToBcn with astc_decoder.comp and bc_encoder.comp) decodes a
   level into a temporary RGBA8 image and reads it back to compress it: every texel leaves the
   shader and comes back, four bytes each way, and the image holds four times the bytes of the
   BC3 blocks it exists to make (measured on MKW: 236-851 MiB resident, 74-657 MiB/s through it).
   Here the texels stay in registers.

   A block's sixteen texels lie in one ASTC block where the footprint is 4, 8 or 12 a side, else
   in up to two each way; a block decodes once for each 4x4 block over it, at worst nine times for
   12x12. ScarletNX measured that cheaper than a shared-memory tile on a GT 1030: the tile's
   shared memory keeps workgroups out of the encoder, where nearly all the time goes. */

/* The byte offset of block (ax, ay) of layer z in the upload's block-linear staging buffer: the
   same arithmetic astc_decoder.comp's main() does for the block it was dispatched for. */
uvec4 LoadBlock(uint ax, uint ay, uint z) {
    const uvec2 pos = uvec2(ax << 4u, ay);
    uint swizzle = ((pos.x & 32u) << 3u) | ((pos.y & 6u) << 5u) | ((pos.x & 16u) << 1u) |
                   ((pos.y & 1u) << 4u) | (pos.x & 15u);
    if (native_tiling != 0u) {
        // Switch 2 GOBs swap offset bits 5 and 6 (see block_linear_unswizzle_2d.comp).
        const uint difference = ((swizzle >> 5u) ^ (swizzle >> 6u)) & 1u;
        swizzle ^= (difference << 5u) | (difference << 6u);
    }
    const uint block_y = pos.y >> 3u;
    uint offset = 0u;
    offset += z * layer_stride;
    offset += (block_y >> block_height) * block_size;
    offset += (block_y & block_height_mask) << 9u;
    offset += (pos.x >> 6u) << x_shift;
    offset += swizzle;
    return astc_data[offset / 16u];
}

vec4 UnpackTexel(uint word) {
    return vec4(uvec4(word & 0xFFu, (word >> 8u) & 0xFFu, (word >> 16u) & 0xFFu, word >> 24u)) /
           255.0;
}

void main() {
    const uint bx = gl_GlobalInvocationID.x;
    const uint by = gl_GlobalInvocationID.y;
    const uint z = gl_WorkGroupID.z;
    const uint cx = (width + 3u) >> 2u;
    const uint cy = (height + 3u) >> 2u;
    if (bx >= cx || by >= cy) {
        return;
    }

    const uint x0 = bx * 4u;
    const uint y0 = by * 4u;
    /* an edge block repeats the level's last texels, as the two-pass encoder does */
    const uint x1 = min(x0 + 3u, width - 1u);
    const uint y1 = min(y0 + 3u, height - 1u);

    vec4 px[16];
    for (uint k = 0u; k < 16u; k++) {
        px[k] = vec4(0.0);
    }
    bool any_transparent = false;

    if (((block_w | block_h) & 3u) == 0u) {
        /* the footprint is whole 4x4 blocks each way: every texel of this block is in one ASTC
           block, which decodes once */
        const uint ax = (x0 * recip_w) >> 18u;
        const uint ay = (y0 * recip_h) >> 18u;
        const uint left = ax * block_w;
        const uint top = ay * block_h;
        blk = LoadBlock(ax, ay, z);
        DecodeBlock();
        for (uint k = 0u; k < 16u; k++) {
            const uint i = min(x0 + (k & 3u), x1) - left;
            const uint j = min(y0 + (k >> 2u), y1) - top;
            px[k] = UnpackTexel(SlotTexel(0u, i, j, infill_ds, infill_dt));
        }
    } else {
        /* a 4x4 block straddles up to two ASTC blocks each way (5, 6, 10 and their widths): each
           one decodes once and hands over the texels of this block that lie inside it */
        const uint ax0 = (x0 * recip_w) >> 18u;
        const uint ax1 = (x1 * recip_w) >> 18u;
        const uint ay0 = (y0 * recip_h) >> 18u;
        const uint ay1 = (y1 * recip_h) >> 18u;
        for (uint ay = ay0; ay <= ay1; ay++) {
            for (uint ax = ax0; ax <= ax1; ax++) {
                blk = LoadBlock(ax, ay, z);
                DecodeBlock();
                const uint left = ax * block_w;
                const uint top = ay * block_h;
                for (uint k = 0u; k < 16u; k++) {
                    const uint i = min(x0 + (k & 3u), x1) - left;
                    const uint j = min(y0 + (k >> 2u), y1) - top;
                    if (i < block_w && j < block_h) {
                        px[k] = UnpackTexel(SlotTexel(0u, i, j, infill_ds, infill_dt));
                    }
                }
            }
        }
    }
    for (uint k = 0u; k < 16u; k++) {
        any_transparent = any_transparent || px[k].a < 0.5;
    }

    /* the texels inside the level, for the refined fit's error */
    const uvec2 inside = uvec2(clamp(ivec2(width, height) - ivec2(x0, y0), ivec2(0), ivec2(4)));
    const uint row = (1u << inside.x) - 1u;
    const uint real = row * (((1u << (4u * inside.y)) - 1u) / 15u);

    const uint block = (z * cy + by) * cx + bx;
    const uint kind = mode & MODE_MASK;
    const bool refine = (mode & REFINE) != 0u;
    if (kind == MODE_BC3) {
        out_blocks[out_offset + block * 2u] = EncodeAlpha(px);
        out_blocks[out_offset + block * 2u + 1u] =
            refine ? EncodeColorRefined(px, real) : EncodeColor(px, false);
    } else if (kind == MODE_BC4) {
        out_blocks[out_offset + block] = EncodeRed(px);
    } else if (kind == MODE_BC5) {
        out_blocks[out_offset + block * 2u] = EncodeRed(px);
        out_blocks[out_offset + block * 2u + 1u] = EncodeAlpha(px);
    } else {
        const bool punch_through = kind != MODE_BC1_OPAQUE && any_transparent;
        out_blocks[out_offset + block] = refine && !punch_through ? EncodeColorRefined(px, real)
                                                                  : EncodeColor(px, punch_through);
    }
}
