#!/usr/bin/env python3
"""Bake a seamless RGBA water-noise gradient tile and all mip levels.

Run from the repository root with:
    python ohos-project/tools/bake_water_normal.py

The resulting WaterNormalMip.bin is a static OHOS rawfile resource. RG and BA
hold independent analytic gradient fields; the shaders scroll/sample them at
the existing four water-noise octave coordinates.
"""

import argparse
import math
import struct
from pathlib import Path


SIZE = 256
PERIOD_CELLS = 32
MIP_LEVELS = 9
GRADIENT_RANGE = 1.875
HEADER = struct.Struct("<4sIIII")
UINT32_MASK = 0xFFFFFFFF


def lattice_value(x, y, seed):
    """Stable integer hash with an exact PERIOD_CELLS repeat in both axes."""
    x %= PERIOD_CELLS
    y %= PERIOD_CELLS
    value = (x * 0x9E3779B1) ^ (y * 0x85EBCA77) ^ seed
    value &= UINT32_MASK
    value ^= value >> 16
    value = (value * 0x7FEB352D) & UINT32_MASK
    value ^= value >> 15
    value = (value * 0x846CA68B) & UINT32_MASK
    value ^= value >> 16
    return (value & 0x00FFFFFF) / 16777216.0


def gradient_noise(x, y, seed):
    """Analytic gradient of quintic-interpolated periodic value noise."""
    cell_x = math.floor(x)
    cell_y = math.floor(y)
    fx = x - cell_x
    fy = y - cell_y
    ux = fx * fx * fx * (fx * (fx * 6.0 - 15.0) + 10.0)
    uy = fy * fy * fy * (fy * (fy * 6.0 - 15.0) + 10.0)
    dux = 30.0 * fx * fx * (fx * (fx - 2.0) + 1.0)
    duy = 30.0 * fy * fy * (fy * (fy - 2.0) + 1.0)

    a = lattice_value(cell_x, cell_y, seed)
    b = lattice_value(cell_x + 1, cell_y, seed)
    c = lattice_value(cell_x, cell_y + 1, seed)
    d = lattice_value(cell_x + 1, cell_y + 1, seed)
    gx = ((b - a) * (1.0 - uy) + (d - c) * uy) * dux
    gy = ((c - a) * (1.0 - ux) + (d - b) * ux) * duy
    return gx, gy


def encode_gradient(component):
    normalized = max(-1.0, min(1.0, component / GRADIENT_RANGE))
    return max(0, min(255, round((normalized + 1.0) * 127.5)))


def bake_base_level():
    pixels = bytearray(SIZE * SIZE * 4)
    cell_per_texel = PERIOD_CELLS / SIZE
    for y in range(SIZE):
        qy = (y + 0.5) * cell_per_texel
        for x in range(SIZE):
            qx = (x + 0.5) * cell_per_texel
            gx0, gy0 = gradient_noise(qx, qy, 0x13579BDF)
            gx1, gy1 = gradient_noise(qx, qy, 0x2468ACE1)
            offset = (y * SIZE + x) * 4
            pixels[offset:offset + 4] = bytes((
                encode_gradient(gx0), encode_gradient(gy0),
                encode_gradient(gx1), encode_gradient(gy1),
            ))
    return pixels


def downsample_toroidal(source, width):
    next_width = max(width // 2, 1)
    destination = bytearray(next_width * next_width * 4)
    for y in range(next_width):
        for x in range(next_width):
            dst = (y * next_width + x) * 4
            for channel in range(4):
                total = 0
                for dy in range(2):
                    for dx in range(2):
                        sx = (x * 2 + dx) % width
                        sy = (y * 2 + dy) % width
                        total += source[(sy * width + sx) * 4 + channel]
                destination[dst + channel] = (total + 2) // 4
    return destination, next_width


def bake():
    levels = [bake_base_level()]
    width = SIZE
    while len(levels) < MIP_LEVELS:
        level, width = downsample_toroidal(levels[-1], width)
        levels.append(level)
    return levels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--output",
        type=Path,
        default=Path(__file__).resolve().parents[1]
        / "entry/src/main/resources/rawfile/WaterNormalMip.bin",
        help="output rawfile path (defaults to the OHOS resource tree)",
    )
    args = parser.parse_args()

    levels = bake()
    payload = bytearray(HEADER.pack(b"WNRM", 1, SIZE, PERIOD_CELLS, MIP_LEVELS))
    payload.extend(b"".join(levels))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(payload)
    print(f"wrote {args.output} ({SIZE}x{SIZE}, {MIP_LEVELS} RGBA8 mip levels, {len(payload)} bytes)")


if __name__ == "__main__":
    main()
