"""Generates orderbook.ico, the Windows icon of the viewer (standard library only).

    python3 -I apps/viz/windows/make_icon.py

The icon is a tiny depth ladder in the viewer's Gruvbox colours: red ask bars growing
right above green bid bars growing left, on a dark rounded tile. It is drawn at 4x and
box-filtered down for smooth edges, then stored at 256, 48, 32 and 16 px as PNG images
inside one .ico file (supported by Windows Vista and later).
"""

import os
import struct
import zlib

BG = (0x28, 0x28, 0x28)      # gruvbox bg0
BORDER = (0x3C, 0x38, 0x36)  # gruvbox bg1
BID = (0xB8, 0xBB, 0x26)     # gruvbox green
ASK = (0xFB, 0x49, 0x34)     # gruvbox red

SIZE = 256
SS = 4  # supersampling factor


def shapes():
    """Rounded rectangles (x0, y0, x1, y1, radius, rgb) in 256-px coordinates, back to front."""
    out = [(8, 8, 248, 248, 52, BORDER), (14, 14, 242, 242, 46, BG)]
    centre, gap = 128, 7
    row, step = 22, 30            # bar height, distance between bar tops
    asks = [58, 88, 46]           # top to bottom; the best ask is nearest the middle
    bids = [78, 54, 92]           # top to bottom; the best bid is nearest the middle
    # Longest bar ends 128 - 7 - 92 = 29 px from the edge: inside the 14 px tile inset.
    top = 128 - 10 - 3 * row - 2 * (step - row)  # 10 px of "spread" each side of the middle
    for i, length in enumerate(asks):
        y = top + i * step
        out.append((centre + gap, y, centre + gap + length, y + row, 7, ASK))
    top = 128 + 10
    for i, length in enumerate(bids):
        y = top + i * step
        out.append((centre - gap - length, y, centre - gap, y + row, 7, BID))
    return out


def inside(px, py, x0, y0, x1, y1, r):
    if px < x0 or px >= x1 or py < y0 or py >= y1:
        return False
    cx = min(max(px, x0 + r), x1 - r)
    cy = min(max(py, y0 + r), y1 - r)
    return (px - cx) ** 2 + (py - cy) ** 2 <= r * r


def render():
    """RGBA rows at SIZE x SIZE, supersampled."""
    big = SIZE * SS
    rects = [(x0 * SS, y0 * SS, x1 * SS, y1 * SS, r * SS, c) for x0, y0, x1, y1, r, c in shapes()]
    canvas = [[None] * big for _ in range(big)]
    for x0, y0, x1, y1, r, c in rects:
        for y in range(int(y0), int(y1)):
            row = canvas[y]
            for x in range(int(x0), int(x1)):
                if inside(x + 0.5, y + 0.5, x0, y0, x1, y1, r):
                    row[x] = c
    return downsample(canvas, SS)


def downsample(canvas, f):
    """Box filter by an integer factor; None means transparent."""
    n = len(canvas) // f
    out = []
    for y in range(n):
        line = []
        for x in range(n):
            r = g = b = a = 0
            for dy in range(f):
                for dx in range(f):
                    p = canvas[y * f + dy][x * f + dx]
                    if p is not None:
                        r += p[0]; g += p[1]; b += p[2]; a += 1
            line.append((r // a, g // a, b // a, 255 * a // (f * f)) if a else (0, 0, 0, 0))
        out.append(line)
    return out


def resize(rgba, size):
    """Area-average an RGBA image (premultiplied, so edges stay clean) to size x size."""
    src = len(rgba)
    out = []
    for y in range(size):
        y0, y1 = y * src // size, (y + 1) * src // size
        line = []
        for x in range(size):
            x0, x1 = x * src // size, (x + 1) * src // size
            r = g = b = a = n = 0
            for yy in range(y0, y1):
                for xx in range(x0, x1):
                    pr, pg, pb, pa = rgba[yy][xx]
                    r += pr * pa; g += pg * pa; b += pb * pa; a += pa; n += 1
            line.append((r // a, g // a, b // a, a // n) if a else (0, 0, 0, 0))
        out.append(line)
    return out


def png(rgba):
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    size = len(rgba)
    raw = b"".join(b"\x00" + bytes(c for px in line for c in px) for line in rgba)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def ico(images):
    """images: list of (size, png bytes)."""
    header = struct.pack("<HHH", 0, 1, len(images))
    offset = 6 + 16 * len(images)
    entries, blobs = b"", b""
    for size, data in images:
        dim = 0 if size >= 256 else size  # 0 means 256 in the ICO format
        entries += struct.pack("<BBBBHHII", dim, dim, 0, 0, 1, 32, len(data), offset)
        blobs += data
        offset += len(data)
    return header + entries + blobs


def main():
    full = render()
    images = [(256, png(full))] + [(s, png(resize(full, s))) for s in (48, 32, 16)]
    out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "orderbook.ico")
    with open(out, "wb") as f:
        f.write(ico(images))
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
