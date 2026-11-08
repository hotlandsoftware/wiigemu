#!/usr/bin/env python3
"""Build packaging/hbc/icon.png (128x48, the Homebrew Channel icon size) from
packaging/gemu.png (GEMU's logo) and the CGA 8x8 font.  Pure stdlib, no PIL.

    python3 tools/make_icon.py [repo-dir] [out.png]
"""
import re
import struct
import sys
import zlib

root = sys.argv[1] if len(sys.argv) > 1 else "."
out = sys.argv[2] if len(sys.argv) > 2 else f"{root}/packaging/hbc/icon.png"
W, H = 128, 48


def read_png_rgba(path):
    data = open(path, "rb").read()
    assert data[:8] == b"\x89PNG\r\n\x1a\n", "not a PNG"
    pos, idat, w = 8, b"", 0
    while pos < len(data):
        ln, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + ln]
        if typ == b"IHDR":
            w, h, depth, ctype, _, _, inter = struct.unpack(">IIBBBBB", body)
            assert depth == 8 and ctype == 6 and inter == 0, "need 8-bit RGBA"
        elif typ == b"IDAT":
            idat += body
        pos += 12 + ln
    raw, bpp, stride = zlib.decompress(idat), 4, w * 4
    rows, prev, i = [], bytearray(stride), 0
    for _ in range(h):
        f, line = raw[i], bytearray(raw[i + 1:i + 1 + stride])
        i += 1 + stride
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b, c = prev[x], prev[x - bpp] if x >= bpp else 0
            if f == 1: line[x] = (line[x] + a) & 255
            elif f == 2: line[x] = (line[x] + b) & 255
            elif f == 3: line[x] = (line[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        rows.append(line)
        prev = line
    return w, h, rows


def write_png(path, w, h, px):
    raw = b"".join(b"\0" + bytes(px[y * w * 4:(y + 1) * w * 4]) for y in range(h))
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    open(path, "wb").write(png)


font_src = open(f"{root}/vga/x86/cgafont8x8.c").read()
font_body = re.sub(r"/\*.*?\*/", "", font_src.split("{", 1)[1], flags=re.S)
font = [int(v, 16) for v in re.findall(r"0x([0-9A-Fa-f]{2})", font_body)]
assert len(font) == 256 * 8, len(font)

px = bytearray(W * H * 4)
def put(x, y, rgba):
    if 0 <= x < W and 0 <= y < H:
        px[(y * W + x) * 4:(y * W + x) * 4 + 4] = bytes(rgba)

# Background: dark rounded panel.
for y in range(H):
    for x in range(W):
        r = 6
        cx = min(x, W - 1 - x); cy = min(y, H - 1 - y)
        if cx < r and cy < r and (r - cx) ** 2 + (r - cy) ** 2 > r * r:
            continue
        put(x, y, (0x20, 0x24, 0x30, 255))

# Logo, 64x64 -> 40x40 nearest neighbour.
lw, lh, logo = read_png_rgba(f"{root}/packaging/gemu.png")
for y in range(40):
    for x in range(40):
        p = logo[y * lh // 40][(x * lw // 40) * 4:(x * lw // 40) * 4 + 4]
        if p[3] > 127:
            put(4 + x, 4 + y, (p[0], p[1], p[2], 255))

# "GEMU" at 2x, "CHIP-8" at 1x.
def text(s, x0, y0, scale, rgba):
    for n, ch in enumerate(s):
        for row in range(8):
            bits = font[ord(ch) * 8 + row]
            for col in range(8):
                if bits & (0x80 >> col):
                    for dy in range(scale):
                        for dx in range(scale):
                            put(x0 + (n * 8 + col) * scale + dx, y0 + row * scale + dy, rgba)

# Wordmark centred in the space right of the logo (x 48..124).
def centred(s, y, scale, rgba):
    text(s, 48 + (76 - len(s) * 8 * scale) // 2, y, scale, rgba)

centred("GEMU", (48 - 16) // 2, 2, (0xFF, 0xCC, 0x00, 255))
write_png(out, W, H, px)
print(f"wrote {out}")
