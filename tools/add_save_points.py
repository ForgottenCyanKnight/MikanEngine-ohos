"""Author safe save positions and cyan save pillars into shipped scenes.

Run after regenerating the portal/course scenes. Idempotent; changes no course
geometry or portal placement. Positions are runtime render origins, while main
scene marker geometry compensates for the existing ground-alignment importer.
"""
import json
from pathlib import Path

root = Path(__file__).resolve().parents[1] / 'ohos-project/entry/src/main/resources/rawfile/scenes'
for name in ('main', 'forest_test', 'physics_parkour'):
    path = root / (name + '.json')
    scene = json.loads(path.read_text(encoding='utf-8'))
    if name == 'main' and scene.get('game') == 'portal_hub':
        print(name, 'hub save position retained without decorative props')
        continue
    gameplay = scene.setdefault('gameplay', {})
    if name == 'main':
        points = [[-3, .02, 3]]
    elif name == 'forest_test':
        points = [gameplay['playerSpawn'][:]]
    else:
        points = [c['spawn'][:] for c in gameplay['parkour']['checkpoints']]
        # Leave the centre line and final return portal unobstructed.
        for p in points: p[0] = -1.0
    gameplay['savePoints'] = points
    scene['entities'] = [e for e in scene['entities']
                         if not e.get('name', {}).get('name', '').startswith('SavePoint ')]
    alignment = 0
    if name == 'main':
        ground = next(e for e in scene['entities'] if e.get('name', {}).get('name') == 'Ground')
        alignment = ground['transform']['position'][1] + ground['transform']['scale'][1] + .94
    # Save-point curtains are rendered procedurally by both backends.
    path.write_text(json.dumps(scene, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
    print(name, len(points), 'save points')
