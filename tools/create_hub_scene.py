"""Replace the demo with a white, enclosed portal hub.

Run last after other scene generators: destination scenes remain independent;
future routes are authored as entries in gameplay.portals and matching frames.
"""
from pathlib import Path
import copy
import json
import struct

ROOT = Path(__file__).resolve().parents[1] / 'ohos-project/entry/src/main/resources/rawfile'
path = ROOT / 'scenes/main.json'
old = json.loads(path.read_text(encoding='utf-8'))

# Reuse the small unit-cube geometry with an untextured, non-emissive white
# material. Explicit box colliders match the normalized [-1,1] mesh bounds.
source = (ROOT / 'models/parkour/cyan.glb').read_bytes()
size = struct.unpack_from('<I', source, 12)[0]
doc = json.loads(source[20:20+size])
for material in doc['materials']:
    material['pbrMetallicRoughness'] = {'baseColorFactor': [1,1,1,1],
        'metallicFactor': 0, 'roughnessFactor': .85}
    material['emissiveFactor'] = [0,0,0]
encoded = json.dumps(doc, separators=(',', ':')).encode()
encoded += b' ' * (-len(encoded) % 4)
tail = source[20+size:]
folder = ROOT / 'models/hub'
folder.mkdir(exist_ok=True)
(folder / 'white.glb').write_bytes(struct.pack('<III', 0x46546c67,2,20+len(encoded)+len(tail)) +
    struct.pack('<II', len(encoded),0x4e4f534a) + encoded + tail)

def cube(i, name, position, scale):
    return {'id': i, 'name': {'name': name},
        'transform': {'position': position, 'rotation': [1,0,0,0], 'scale': scale},
        'hierarchy': {'parent': 4294967295, 'children': []},
        'mesh': {'type': 4, 'modelPath': 'models/hub/white.glb'},
        'render': {'visible': True, 'castShadow': True, 'receiveShadow': True},
        'collider': {'type': 0, 'size': [2,2,2], 'offset': [0,0,0],
                     'isTrigger': False, 'syncWithModel': True, 'autoFitToModel': False}}

scene = {'formatVersion': 1, 'game': 'portal_hub', 'water': {'enabled': False},
    'gameplay': {'playerSpawn': [0,.02,3], 'enemyEnabled': False,
        'demoPropsEnabled': False, 'helmetEnabled': True, 'groundEnabled': False,
        'savePoints': [[-3,.02,3]], 'portals': [
            {'target': 'scenes/forest_test.json', 'label': 'FOREST',
             'position': [-5,.02,-5], 'radius': .85, 'color': [.08,.85,.4], 'opacity': .32},
            {'target': 'scenes/physics_parkour.json', 'label': 'PHYSICS + PARKOUR',
             'position': [5,.02,-5], 'radius': .85, 'color': [.03,.48,1], 'opacity': .32},
            {'target': 'scenes/arena.json', 'label': 'ARENA - 4 WAVES',
             'position': [0,.02,-5], 'radius': .85, 'color': [1,.35,.05], 'opacity': .32}]},
    'entities': [copy.deepcopy(e) for e in old['entities']
                 if 'camera' in e or 'skybox' in e or e.get('light',{}).get('type') == 0]}
scene['entities'] += [
    cube(13000,'Hub white platform',[0,-1.44,0],[12,.5,10]),
    cube(13001,'Hub north wall',[0,.36,-10],[12,1.3,.2]),
    cube(13002,'Hub south wall',[0,.36,10],[12,1.3,.2]),
    cube(13003,'Hub west wall',[-12,.36,0],[.2,1.3,10]),
    cube(13004,'Hub east wall',[12,.36,0],[.2,1.3,10])]
for i,(x,z,half) in enumerate([(3,3,.45),(4.2,3,.45),(3.6,4.2,.55),(5.2,4.4,.4),(6,2.5,.5)]):
    block=cube(13200+i,f'Hub pushable cube {i+1}',[x,-.94+half+.03,z],[half,half,half])
    block['dynamicBody']=True
    scene['entities'].append(block)
for i, portal in enumerate(scene['gameplay']['portals']):
    x,_,z=portal['position']; base=-.94; first=13100+i*4
    scene['entities'] += [
        cube(first,f'Hub gate {i+1} left',[x-1.1,base+1.4,z],[.16,1.4,.2]),
        cube(first+1,f'Hub gate {i+1} right',[x+1.1,base+1.4,z],[.16,1.4,.2]),
        cube(first+2,f'Hub gate {i+1} lintel',[x,base+2.95,z],[1.26,.15,.2]),
        cube(first+3,f'Hub gate {i+1} pad',[x,base-.025,z],[1.3,.025,.85])]
path.write_text(json.dumps(scene,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
print('Created 24x20 m white hub: four walls, three portal routes, one save position.')
