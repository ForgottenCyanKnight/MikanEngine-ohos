"""Deterministic 16-bit terrain data for the default OHOS archipelago."""
import argparse
import math
import pathlib
import struct
import zlib

WORLD_SIZE = 192.0
RESOLUTION = 257
BASE_Y = -6.5
HEIGHT_SCALE = 20.0
# centre X/Z, elliptical radii X/Z, peak Y, rotation (radians)
ISLANDS = [(-54, -43, 23, 21, 2, 0.25), (54, -46, 23, 25, 3, -0.4),
           (57, 43, 26, 23, 2.5, 0.6), (-51, 48, 22, 20, 2, -0.65),
           (0, 69, 18, 16, 1, 0.3)]


def smooth(value):
    value = max(0.0, min(1.0, value))
    return value * value * (3 - 2 * value)


def height(x, z):
    # Keep the full enlarged slab plus a two-unit apron supported at -1.34.
    distance = max(abs(x), abs(z))
    result = -1.34 + (BASE_Y + 1.34) * smooth((distance - 22) / 14)
    for index, (cx, cz, rx, rz, peak, angle) in enumerate(ISLANDS):
        dx, dz = x - cx, z - cz
        u = (math.cos(angle) * dx + math.sin(angle) * dz) / rx
        v = (-math.sin(angle) * dx + math.cos(angle) * dz) / rz
        theta = math.atan2(v, u)
        # Different non-circular coastlines; all meet the same seabed smoothly.
        radius = math.hypot(u, v) / (1 + 0.10 * math.sin(theta * 3 + index) +
                                      0.07 * math.cos(theta * 5 - index))
        shoulder = max(0.0, 1 - radius * radius)
        island = BASE_Y + (peak - BASE_Y) * shoulder * shoulder
        result = max(result, island)
    return result


def png(path, width, height, depth, color_type, rows):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, depth, color_type, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(b''.join(b'\0' + row for row in rows))) + chunk(b'IEND', b''))


def generate(root):
    rows = []
    for y in range(RESOLUTION):
        # The runtime importer maps the bottom PNG row to negative world Z.
        z = (0.5 - y / (RESOLUTION - 1)) * WORLD_SIZE
        values = []
        for x in range(RESOLUTION):
            wx = (x / (RESOLUTION - 1) - 0.5) * WORLD_SIZE
            values.append(round((height(wx, z) - BASE_Y) / HEIGHT_SCALE * 65535))
        rows.append(struct.pack(f'>{RESOLUTION}H', *values))
    png(root / 'terrain/archipelago_height16.png', RESOLUTION, RESOLUTION, 16, 0, rows)
    for name, rgb in [('grass', (76, 126, 60)), ('rock', (133, 137, 143)), ('soil', (151, 112, 74))]:
        png(root / f'terrain/island_{name}.png', 2, 2, 8, 6, [bytes((*rgb, 255)) * 2] * 2)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', type=pathlib.Path, required=True)
    generate(parser.parse_args().output)
