#!/usr/bin/env python3

# SPDX-FileCopyrightText: Copyright 2026 Eglantine Emulator Project
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decodes the block-compressed sampled textures of a frame dump to PNG.

The dump reads BCn images (and ASTC the host re-compressed to BC1/BC3) back as the host's raw
blocks ("host_format" in manifest.jsonl, "png": null). This writes <name>.png next to each one for
BC1-BC5; BC6H and BC7 are listed as skipped. Needs the zstd command.

usage: frame_dump_bc.py <dump dir> [name substring]
"""

import json
import struct
import subprocess
import sys
import zlib
from pathlib import Path


def write_png(path, width, height, rgb):
    stride = width * 3
    raw = b''.join(b'\0' + bytes(rgb[y * stride:(y + 1) * stride]) for y in range(height))

    def chunk(kind, data):
        body = kind + data
        return struct.pack('>I', len(data)) + body + struct.pack('>I', zlib.crc32(body))

    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 2, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(raw, 6)) + chunk(b'IEND', b''))


def color_block(block):
    """BC1 colour block: 16 (r, g, b) texels."""
    c0, c1, bits = struct.unpack('<HHI', block)

    def rgb565(c):
        return ((c >> 11) * 255 // 31, ((c >> 5) & 63) * 255 // 63, (c & 31) * 255 // 31)

    a, b = rgb565(c0), rgb565(c1)
    if c0 > c1:
        palette = [a, b, tuple((2 * x + y) // 3 for x, y in zip(a, b)),
                   tuple((x + 2 * y) // 3 for x, y in zip(a, b))]
    else:
        palette = [a, b, tuple((x + y) // 2 for x, y in zip(a, b)), (0, 0, 0)]
    return [palette[(bits >> (2 * i)) & 3] for i in range(16)]


def channel_block(block, signed=False):
    """BC4 channel block: 16 values 0-255 (signed ones shifted by 128)."""
    a0, a1 = struct.unpack('<bb' if signed else '<BB', block[:2])
    bits = int.from_bytes(block[2:8], 'little')
    if a0 > a1:
        palette = [a0, a1] + [((7 - i) * a0 + i * a1) // 7 for i in range(1, 7)]
    else:
        palette = [a0, a1] + [((5 - i) * a0 + i * a1) // 5 for i in range(1, 5)]
        palette += [-128, 127] if signed else [0, 255]
    shift = 128 if signed else 0
    return [palette[(bits >> (3 * i)) & 7] + shift for i in range(16)]


def decode(fmt, data, width, height):
    bw = (width + 3) // 4
    bh = (height + 3) // 4
    size = 8 if fmt in ('BC1', 'BC4', 'BC4S') else 16
    out = bytearray(width * height * 3)
    for by in range(bh):
        for bx in range(bw):
            offset = (by * bw + bx) * size
            block = data[offset:offset + size]
            if fmt == 'BC1':
                texels = color_block(block)
            elif fmt in ('BC2', 'BC3'):
                texels = color_block(block[8:])
            elif fmt in ('BC4', 'BC4S'):
                texels = [(v, v, v) for v in channel_block(block, fmt == 'BC4S')]
            else:  # BC5, BC5S
                signed = fmt == 'BC5S'
                texels = [(r, g, 0) for r, g in zip(channel_block(block[:8], signed),
                                                    channel_block(block[8:], signed))]
            for i, texel in enumerate(texels):
                x, y = bx * 4 + (i & 3), by * 4 + (i >> 2)
                if x < width and y < height:
                    out[(y * width + x) * 3:(y * width + x) * 3 + 3] = bytes(texel)
    return out


def main():
    directory = Path(sys.argv[1])
    needle = sys.argv[2] if len(sys.argv) > 2 else ''
    for line in (directory / 'manifest.jsonl').open():
        entry = json.loads(line)
        fmt = entry.get('host_format')
        if not fmt or needle not in entry['name']:
            continue
        if fmt not in ('BC1', 'BC2', 'BC3', 'BC4', 'BC4S', 'BC5', 'BC5S'):
            print(f"{entry['name']}: {fmt} skipped")
            continue
        data = subprocess.run(['zstd', '-dc', str(directory / entry['raw'])],
                              capture_output=True, check=True).stdout
        out = directory / (entry['name'] + '.png')
        write_png(out, entry['width'], entry['height'],
                  decode(fmt, data, entry['width'], entry['height']))
        print(out)


if __name__ == '__main__':
    main()
