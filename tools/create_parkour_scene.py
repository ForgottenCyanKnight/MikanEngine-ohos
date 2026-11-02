"""Deterministic, collision-matched floating course. Run after create_portal_test_scene.py."""
from pathlib import Path
import copy
import json
import struct

ROOT = Path(__file__).resolve().parents[1] / 'ohos-project/entry/src/main/resources/rawfile'

def entity(i, name, pos, scale, model='engine/models/Base Model/cube.glb'):
    return {'id': i, 'name': {'name': name}, 'transform': {'position': pos,
        'rotation': [1, 0, 0, 0], 'scale': scale}, 'mesh': {'type': 4, 'modelPath': model},
        'render': {'visible': True, 'castShadow': not name.startswith(('Route edge','Forward arrow','Checkpoint beacon')), 'receiveShadow': True}}

def write_marker(name, color):
    # Untextured cube, [-1,1] bounds. Static model strips are decorative only;
    # the underlying ordinary scene cube supplies the exact solid platform.
    vertices, normals, indices = [], [], []
    faces = [([1,0,0],[(1,-1,-1),(1,1,-1),(1,1,1),(1,-1,1)]),
             ([-1,0,0],[(-1,-1,1),(-1,1,1),(-1,1,-1),(-1,-1,-1)]),
             ([0,1,0],[(-1,1,-1),(-1,1,1),(1,1,1),(1,1,-1)]),
             ([0,-1,0],[(-1,-1,1),(-1,-1,-1),(1,-1,-1),(1,-1,1)]),
             ([0,0,1],[(-1,-1,1),(1,-1,1),(1,1,1),(-1,1,1)]),
             ([0,0,-1],[(1,-1,-1),(-1,-1,-1),(-1,1,-1),(1,1,-1)])]
    for normal, points in faces:
        first = len(vertices)
        vertices.extend(points); normals.extend([normal]*4)
        indices.extend([first,first+1,first+2,first,first+2,first+3])
    data = b''.join(struct.pack('<3f', *v) for v in vertices+normals)
    data += struct.pack('<36H', *indices)
    doc = {'asset': {'version':'2.0'}, 'scene':0, 'scenes':[{'nodes':[0]}],
        'nodes':[{'mesh':0}], 'buffers':[{'byteLength':len(data)}],
        'bufferViews':[{'buffer':0,'byteOffset':0,'byteLength':288},
                       {'buffer':0,'byteOffset':288,'byteLength':288},
                       {'buffer':0,'byteOffset':576,'byteLength':72}],
        'accessors':[{'bufferView':0,'componentType':5126,'count':24,'type':'VEC3','min':[-1,-1,-1],'max':[1,1,1]},
                     {'bufferView':1,'componentType':5126,'count':24,'type':'VEC3'},
                     {'bufferView':2,'componentType':5123,'count':36,'type':'SCALAR'}],
        'meshes':[{'primitives':[{'attributes':{'POSITION':0,'NORMAL':1},'indices':2,'material':0}]}],
        'materials':[{'pbrMetallicRoughness':{'baseColorFactor':[*color,1], 'metallicFactor':.15,'roughnessFactor':.4},
                      'emissiveFactor':[v*.45 for v in color]}]}
    raw = json.dumps(doc,separators=(',',':')).encode(); raw += b' '*(-len(raw)%4)
    glb = struct.pack('<III',0x46546c67,2,28+len(raw)+len(data))
    glb += struct.pack('<II',len(raw),0x4e4f534a)+raw+struct.pack('<II',len(data),0x004e4942)+data
    path = ROOT/f'models/parkour/{name}.glb'; path.parent.mkdir(exist_ok=True)
    path.write_bytes(glb)

def gate(first, x, z, top, prefix, marker):
    return [entity(first,prefix+' left',[x-1.1,top+1.4,z],[.16,1.4,.2],marker),
        entity(first+1,prefix+' right',[x+1.1,top+1.4,z],[.16,1.4,.2],marker),
        entity(first+2,prefix+' lintel',[x,top+2.95,z],[1.26,.15,.2],marker),
        entity(first+3,prefix+' pad',[x,top-.025,z],[1.3,.025,.85])]

for name, color in [('amber',[1,.43,.055]),('cyan',[.05,.75,.85]),('violet',[.58,.22,1])]:
    write_marker(name,color)
amber='models/parkour/amber.glb'
mainpath=ROOT/'scenes/main.json'
main=json.loads(mainpath.read_text(encoding='utf-8'))
main['entities']=[e for e in main['entities'] if not e.get('name',{}).get('name','').startswith('ParkourGate')]
ground=next((e for e in main['entities'] if e.get('name',{}).get('name')=='Ground'), {'transform':{'position':[0,-1.14,0],'scale':[20,.2,20]}})
top=ground['transform']['position'][1]+abs(ground['transform']['scale'][1])
portals=main.setdefault('gameplay',{}).setdefault('portals',[])
portals[:]=[p for p in portals if p['target']!='scenes/parkour.json']
portals.append({'target':'scenes/parkour.json','label':'SKY PARKOUR - WALK THROUGH GATE',
    'position':[5,.02,-4], 'radius':.85,'color':[1,.38,.04],'opacity':.32})
main['entities']+=gate(9400,5,-4,top,'ParkourGate',amber)
mainpath.write_text(json.dumps(main,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')

# x,z,top,halfWidth,halfDepth. Clear gaps 0.65..0.9 m; rises <=0.35 m.
# A 5 m/s jump under 9.81 m/s² gravity reaches ~1.27 m high and ~1.3 m
# at walking speed, with sprint available for more forgiving approaches.
platforms=[(0,0,6,2.6,2.6),
    (0,-4.65,6.3,1.3,1.3),(0,-7.9,6.65,1.2,1.2),(0,-11,7,1.15,1.15),
    (0,-15.05,7,2.2,2.2),
    (1.4,-19.2,7.3,1.2,1.2),(2.8,-22.35,7.65,1.2,1.2),
    (1.2,-25.5,7.95,1.15,1.15),(0,-29.6,7.95,2.2,2.2),
    (0,-33.85,8.25,.8,1.3),(0,-37.15,8.6,.7,1.2),
    (0,-40.35,8.9,.7,1.2),(0,-44.5,8.9,2.5,2.2)]
scene={'formatVersion':1,'game':'sky_parkour','water':{'enabled':False},
    'gameplay':{'playerSpawn':[0,6.98,0],'enemyEnabled':False,'demoPropsEnabled':False,
        'groundEnabled':False,'portals':[
            {'target':'scenes/main.json','label':'RETURN TO HUB','position':[0,6.98,1.4], 'radius':.8,'color':[.35,.18,1],'opacity':.32},
            {'target':'scenes/main.json','label':'FINISH - RETURN TO HUB','position':[0,9.88,-45.5],'radius':.8,'color':[.35,.18,1],'opacity':.32}],
        'parkour':{'fallY':1,'checkpoints':[]}}, 'entities':[]}
for e in main['entities']:
    if ('light' in e and e['light'].get('type')==0) or 'skybox' in e or 'camera' in e:
        scene['entities'].append(copy.deepcopy(e))
for index,(x,z,y,hx,hz) in enumerate(platforms):
    section=0 if index<=4 else (1 if index<=8 else 2)
    marker=f'models/parkour/{["cyan","amber","violet"][section]}.glb'
    start=9500+index*10
    scene['entities'].append(entity(start,f'Course_{index:02d}',[x,y-.25,z],[hx,.25,hz]))
    # Side rails are low, inset light strips. Keep the landing face unobstructed.
    for side in [-1,1]:
        scene['entities'].append(entity(start+1+(side==1),f'Route edge {index}',
            [x+side*(hx-.08),y+.012,z],[.045,.012,hz-.08],marker))
    # Two chevrons indicate forward travel; these use coloured strips atop each landing.
    for side in [-1,1]:
        e=entity(start+3+(side==1),f'Forward arrow {index}',[x+side*.14,y+.015,z-.16],[.035,.015,.24],marker)
        import math
        yaw=side*.6; e['transform']['rotation']=[math.cos(yaw/2),0,math.sin(yaw/2),0]
        scene['entities'].append(e)
    if index in (0,4,8,12):
        scene['gameplay']['parkour']['checkpoints'].append({'spawn':[x,y+.98,z+.5 if index==12 else z],
            'halfExtents':[hx-.2,.2,hz-.2]})
        # Four slim beacons frame safe rest islands and distinguish them from jump pads.
        for corner,(sx,sz) in enumerate([(-1,-1),(1,-1),(-1,1),(1,1)]):
            scene['entities'].append(entity(start+5+corner,f'Checkpoint beacon {index}',
                [x+sx*(hx-.15),y+.35,z+sz*(hz-.15)],[.07,.35,.07],marker))
scene['entities']+=gate(9800,0,1.4,6,'Start return','models/parkour/violet.glb')
scene['entities']+=gate(9810,0,-45.5,8.9,'Finish return','models/parkour/violet.glb')
(ROOT/'scenes/parkour.json').write_text(json.dumps(scene,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
print(f'Created {len(platforms)} floating platforms, 2 rest checkpoints and finish, 3 scene graph.')
