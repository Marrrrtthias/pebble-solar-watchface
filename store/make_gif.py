#!/usr/bin/env python3
"""Assemble emulator screenshots (PNG) into an animated GIF, no dependencies.

Usage: python3 store/make_gif.py OUT.gif FRAME.png [FRAME.png ...] [--delay MS] [--scale N]

Pebble screenshots use the 64-color palette, so one global palette is lossless.
Each frame only stores the rectangle that changed since the previous one.
"""
import argparse
import struct
import zlib


def load_png(path):
    data = open(path, "rb").read()
    i, idat = 8, b""
    while i < len(data):
        n, = struct.unpack(">I", data[i:i + 4])
        kind, body = data[i + 4:i + 8], data[i + 8:i + 8 + n]
        if kind == b"IHDR":
            w, h, depth, ctype = struct.unpack(">IIBB", body[:10])
            if depth != 8 or ctype not in (2, 6):
                raise ValueError(f"{path}: only 8-bit RGB/RGBA PNGs are supported")
        elif kind == b"IDAT":
            idat += body
        i += 12 + n
    raw = zlib.decompress(idat)
    bpp = 3 if ctype == 2 else 4
    stride = w * bpp
    prev = bytearray(stride)
    pixels, o = [], 0
    for _ in range(h):
        f = raw[o]
        r = bytearray(raw[o + 1:o + 1 + stride])
        o += 1 + stride
        for x in range(stride):
            a = r[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                r[x] = (r[x] + a) & 255
            elif f == 2:
                r[x] = (r[x] + b) & 255
            elif f == 3:
                r[x] = (r[x] + (a + b) // 2) & 255
            elif f == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                r[x] = (r[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        pixels.append([tuple(r[x * bpp:x * bpp + 3]) for x in range(w)])
        prev = r
    return w, h, pixels


def lzw_encode(indices, min_size):
    clear, eoi = 1 << min_size, (1 << min_size) + 1
    out = bytearray()
    bitbuf = bitcount = 0

    def emit(code, size):
        nonlocal bitbuf, bitcount
        bitbuf |= code << bitcount
        bitcount += size
        while bitcount >= 8:
            out.append(bitbuf & 255)
            bitbuf >>= 8
            bitcount -= 8

    def reset():
        return {}, min_size + 1, eoi + 1

    table, size, nxt = reset()
    emit(clear, size)
    w = indices[0]
    for k in indices[1:]:
        key = (w, k)
        if key in table:
            w = table[key]
            continue
        emit(w, size)
        if nxt < 4096:
            table[key] = nxt
            nxt += 1
            if nxt > (1 << size) and size < 12:
                size += 1
        else:
            emit(clear, size)
            table, size, nxt = reset()
        w = k
    emit(w, size)
    emit(eoi, size)
    if bitcount:
        out.append(bitbuf & 255)
    return bytes(out)


def sub_blocks(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out += bytes([len(chunk)]) + chunk
    return bytes(out) + b"\x00"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("frames", nargs="+")
    ap.add_argument("--delay", type=int, default=150, help="ms per frame")
    ap.add_argument("--scale", type=int, default=1)
    args = ap.parse_args()

    frames = []
    for p in args.frames:
        w, h, px = load_png(p)
        if args.scale > 1:
            px = [[c for c in row for _ in range(args.scale)] for row in px for _ in range(args.scale)]
            w, h = w * args.scale, h * args.scale
        frames.append(px)

    palette = sorted({c for f in frames for row in f for c in row})
    if len(palette) > 256:
        raise SystemExit(f"too many colors ({len(palette)})")
    index = {c: i for i, c in enumerate(palette)}
    bits = max(1, (len(palette) - 1).bit_length())
    table_size = 1 << bits
    min_code = max(2, bits)

    gif = bytearray(b"GIF89a")
    gif += struct.pack("<HHBBB", w, h, 0x80 | 0x70 | (bits - 1), 0, 0)
    for c in palette + [(0, 0, 0)] * (table_size - len(palette)):
        gif += bytes(c)
    gif += b"\x21\xff\x0bNETSCAPE2.0\x03\x01\x00\x00\x00"  # loop forever

    delay = max(2, round(args.delay / 10))
    prev = None
    for f in frames:
        if prev is None:
            x0, y0, x1, y1 = 0, 0, w - 1, h - 1
        else:
            rows = [y for y in range(h) if f[y] != prev[y]]
            if not rows:  # identical frame: keep a 1px rect so timing is preserved
                rows = [0]
                cols = [0]
            else:
                cols = [x for x in range(w) if any(f[y][x] != prev[y][x] for y in rows)]
            x0, x1, y0, y1 = min(cols), max(cols), min(rows), max(rows)
        sw, sh = x1 - x0 + 1, y1 - y0 + 1
        gif += struct.pack("<BBBBHBB", 0x21, 0xF9, 4, 0x04, delay, 0, 0)
        gif += struct.pack("<BHHHHB", 0x2C, x0, y0, sw, sh, 0)
        idx = [index[f[y][x]] for y in range(y0, y1 + 1) for x in range(x0, x1 + 1)]
        gif += bytes([min_code]) + sub_blocks(lzw_encode(idx, min_code))
        prev = f
    gif += b"\x3b"
    open(args.out, "wb").write(gif)
    print(f"wrote {args.out}: {len(frames)} frames, {len(palette)} colors, {len(gif) // 1024} KB")


if __name__ == "__main__":
    main()
