import struct
import os

def pack_ico(png_files_with_sizes, output_ico):
    # png_files_with_sizes: list of (size_int, path)
    count = len(png_files_with_sizes)
    header = struct.pack("<HHH", 0, 1, count)
    
    entries = []
    data_blobs = []
    
    offset = 6 + count * 16
    for sz, path in png_files_with_sizes:
        data = open(path, "rb").read()
        w = 0 if sz >= 256 else sz
        h = 0 if sz >= 256 else sz
        entry = struct.pack("<BBBBHHII", w, h, 0, 0, 1, 32, len(data), offset)
        entries.append(entry)
        data_blobs.append(data)
        offset += len(data)
        
    with open(output_ico, "wb") as f:
        f.write(header)
        for e in entries:
            f.write(e)
        for d in data_blobs:
            f.write(d)
    print(f"Successfully packed {count} frames into {output_ico}")

if __name__ == "__main__":
    sizes = [16, 24, 32, 48, 64, 128, 256]
    pairs = []
    for s in sizes:
        p = f"dist/icon_{s}.png"
        if os.path.exists(p):
            pairs.append((s, p))
    if pairs:
        pack_ico(pairs, "dist/eden.ico")
        pack_ico(pairs, "dist/dalia.ico")

