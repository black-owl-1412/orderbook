"""Convert a binary PPM (P6), as written by `orderbook_viz --screenshot`, to PNG.

Standard library only, so it works without Pillow or ImageMagick:
    python3 -I ppm2png.py shot.ppm shot.png
"""

import struct
import sys
import zlib


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
    # Header: "P6\n<width> <height>\n<maxval>\n" followed by raw RGB bytes.
    magic, dims, maxval, pixels = data.split(b"\n", 3)
    if magic != b"P6" or maxval != b"255":
        raise ValueError(f"{path}: not an 8-bit binary PPM")
    width, height = map(int, dims.split())
    if len(pixels) < width * height * 3:
        raise ValueError(f"{path}: truncated pixel data")
    return width, height, pixels


def write_png(path, width, height, rgb):
    def chunk(kind, payload):
        crc = zlib.crc32(kind + payload) & 0xFFFFFFFF
        return struct.pack(">I", len(payload)) + kind + payload + struct.pack(">I", crc)

    stride = width * 3
    raw = b"".join(b"\x00" + rgb[y * stride:(y + 1) * stride] for y in range(height))
    header = struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)  # 8-bit RGB
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", header))
        f.write(chunk(b"IDAT", zlib.compress(raw, 9)))
        f.write(chunk(b"IEND", b""))


def main():
    if len(sys.argv) != 3:
        sys.exit(f"usage: {sys.argv[0]} input.ppm output.png")
    width, height, rgb = read_ppm(sys.argv[1])
    write_png(sys.argv[2], width, height, rgb)
    print(f"{sys.argv[2]}: {width}x{height}")


if __name__ == "__main__":
    main()
