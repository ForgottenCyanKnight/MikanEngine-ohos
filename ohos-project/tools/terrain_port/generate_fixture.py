"""Generate deterministic PNG fixtures without third-party Python packages."""
import argparse
import json
import math
import pathlib
import struct
import zlib


def png(path, width, height, depth, color_type, rows):
    def chunk(kind, data):
        return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff)
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, depth, color_type, 0, 0, 0)) +
                    chunk(b'IDAT', zlib.compress(b''.join(b'\0' + row for row in rows))) + chunk(b'IEND', b''))


def generate(root):
    from generate_archipelago import generate as generate_archipelago
    generate_archipelago(root)
    png(root / 'gradient16.png', 3, 2, 16, 0,
        [struct.pack('>3H', 0, 32768, 65535), struct.pack('>3H', 65535, 32768, 0)])
    png(root / 'invalid8.png', 2, 2, 8, 0, [bytes([0, 255])] * 2)
    for name, color in [('red.png', (255, 0, 0, 255)), ('blue.png', (0, 0, 255, 255)),
                        ('control.png', (255, 0, 0, 0)), ('mix_control.png', (128, 128, 0, 0))]:
        png(root / name, 2, 2, 8, 6, [bytes(color) * 2] * 2)
    rows = []
    for y in range(65):
        rows.append(b''.join(struct.pack('>H', round(65535 * 0.6 *
            math.sin(math.pi * x / 64) ** 2 * math.sin(math.pi * y / 64) ** 2)) for x in range(65)))
    png(root / 'terrain/demo_height16.png', 65, 65, 16, 0, rows)
    # The runtime floor occupies [-10,10] in X/Z. A one-unit apron keeps
    # bilinear sampling and boundary triangles entirely on the support plane.
    # Only the outer terrain transitions back into the original rolling hill.
    rows = []
    for y in range(65):
        samples = []
        for x in range(65):
            distance = max(abs(x - 32), abs(y - 32))
            transition = max(0.0, min(1.0, (distance - 11) / 6))
            transition = transition * transition * (3 - 2 * transition)
            hill = 0.6 * math.sin(math.pi * x / 64) ** 2 * math.sin(math.pi * y / 64) ** 2
            samples.append(round(65535 * (0.6 * (1 - transition) + hill * transition)))
        rows.append(struct.pack('>65H', *samples))
    png(root / 'terrain/default_ground_height16.png', 65, 65, 16, 0, rows)
    for i, rgb in enumerate([(106, 142, 69), (130, 125, 117), (135, 94, 63)]):
        png(root / f'terrain/demo_layer{i}.png', 2, 2, 8, 6, [bytes((*rgb, 255)) * 2] * 2)
    scene = {'formatVersion': 1, 'game': '', 'entities': [{
        'id': 9001, 'name': {'name': 'Heightmap terrain'},
        'transform': {'position': [0, -0.94, 0], 'rotation': [1, 0, 0, 0], 'scale': [1, 1, 1]},
        'render': {'visible': True, 'castShadow': True, 'receiveShadow': True},
        'terrain': {'enabled': True, 'heightmapPath': 'terrain/demo_height16.png',
                    'worldSize': [20, 20], 'heightScale': 4, 'heightOffset': 0,
                    'chunkCount': 4, 'patchResolution': 17, 'collisionEnabled': True,
                    'collisionResolution': 65, 'materialTiling': 8, 'blendSharpness': 1,
                    **{f'layer{i}Path': f'terrain/demo_layer{i}.png' for i in range(3)}}}]}
    (root / 'scenes').mkdir(parents=True, exist_ok=True)
    (root / 'scenes/terrain_demo.json').write_text(json.dumps(scene, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    generate(parser.parse_args().output)
