#!/usr/bin/env python3
"""Render the Solar store icons (144x144 and 48x48) without dependencies.

Usage: python3 store/make_icon.py
"""
import math
import os
import struct
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))

HORIZON = 0.58      # y of the horizon line (0 = top, 1 = bottom)
AMP = 0.30          # curve amplitude
PEAK_X = 0.60       # solar noon position
SUN_X = 0.70        # current time on the curve


def curve_y(x):
    return HORIZON - AMP * math.cos(2 * math.pi * (x - PEAK_X))


def curve_dy(x):
    return AMP * 2 * math.pi * math.sin(2 * math.pi * (x - PEAK_X))


def smoothstep(e0, e1, v):
    t = min(1.0, max(0.0, (v - e0) / (e1 - e0)))
    return t * t * (3 - 2 * t)


def mix(a, b, t):
    return tuple(a[i] + (b[i] - a[i]) * t for i in range(3))


def shade(x, y, px, stroke_curve, stroke_horizon):
    """Color and alpha at normalized point (x, y); px = size of one pixel."""
    sun = (SUN_X, curve_y(SUN_X))

    # Background: deep night-blue at the top fading to black
    col = mix((14, 20, 42), (2, 3, 8), y)

    # Sun glow (wide blue halo + brighter inner halo)
    d2 = (x - sun[0]) ** 2 + (y - sun[1]) ** 2
    ground = 0.55 if y > HORIZON else 1.0
    halo = 0.95 * math.exp(-d2 / (2 * 0.17 ** 2)) * ground
    inner = 0.9 * math.exp(-d2 / (2 * 0.06 ** 2))
    col = tuple(c + g * halo for c, g in zip(col, (25, 105, 255)))
    col = tuple(c + g * inner for c, g in zip(col, (150, 200, 255)))

    # Horizon line
    hd = abs(y - HORIZON)
    a = 1 - smoothstep(stroke_horizon / 2 - px, stroke_horizon / 2 + px, hd)
    col = mix(col, (120, 128, 140), a * 0.9)

    # Sun path
    cd = abs(y - curve_y(x)) / math.sqrt(1 + curve_dy(x) ** 2)
    a = 1 - smoothstep(stroke_curve / 2 - px, stroke_curve / 2 + px, cd)
    col = mix(col, (215, 220, 228), a)

    # Sun disc
    r = 0.075
    a = 1 - smoothstep(r - px, r + px, math.sqrt(d2))
    col = mix(col, (255, 255, 255), a)

    col = tuple(min(255.0, c) for c in col)

    # Rounded-square mask
    rad = 0.2
    qx = max(abs(x - 0.5) - (0.5 - rad), 0)
    qy = max(abs(y - 0.5) - (0.5 - rad), 0)
    edge = math.hypot(qx, qy) - rad
    alpha = 1 - smoothstep(-px, px, edge)
    return col, alpha


def render(size, stroke_curve, stroke_horizon, ss=4):
    px = 1.0 / size
    rows = []
    for j in range(size):
        row = bytearray()
        for i in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sj in range(ss):
                for si in range(ss):
                    x = (i + (si + 0.5) / ss) / size
                    y = (j + (sj + 0.5) / ss) / size
                    (r, g, b), a = shade(x, y, px * 0.5, stroke_curve, stroke_horizon)
                    acc[0] += r * a
                    acc[1] += g * a
                    acc[2] += b * a
                    acc[3] += a
            n = ss * ss
            a = acc[3] / n
            if a > 0:
                rgb = [int(round(acc[k] / acc[3])) for k in range(3)]
            else:
                rgb = [0, 0, 0]
            row += bytes(rgb + [int(round(a * 255))])
        rows.append(row)
    return rows


def write_png(path, rows):
    h, w = len(rows), len(rows[0]) // 4
    raw = b"".join(b"\x00" + bytes(r) for r in rows)

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9))
    png += chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


if __name__ == "__main__":
    write_png(os.path.join(HERE, "icon_144.png"), render(144, 0.022, 0.010))
    write_png(os.path.join(HERE, "icon_48.png"), render(48, 0.048, 0.024))
    print("wrote store/icon_144.png and store/icon_48.png")
