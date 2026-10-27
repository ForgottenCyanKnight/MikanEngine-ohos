"""Merge the jump trail, crate mechanism, moving platforms and swimming lane.

Run after create_parkour_scene.py and create_workshop_scene.py. The old scenes
remain isolated fixtures for regression tests; only this combined route is in
the hub's active portal graph.
"""
from pathlib import Path
import copy
import json

ROOT=Path(__file__).resolve().parents[1]/'ohos-project/entry/src/main/resources/rawfile'
def read(name): return json.loads((ROOT/f'scenes/{name}.json').read_text(encoding='utf-8'))
def write(name,doc): (ROOT/f'scenes/{name}.json').write_text(json.dumps(doc,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
def entity(i,name,pos,scale,model='engine/models/Base Model/cube.glb'):
    return {'id':i,'name':{'name':name},'transform':{'position':pos,'rotation':[1,0,0,0],'scale':scale},
        'mesh':{'type':4,'modelPath':model},'render':{'visible':True,'castShadow':not name.startswith('Safe island beacon'),'receiveShadow':True}}
def gate(first,x,z,top,prefix):
    model='models/parkour/cyan.glb'
    return [entity(first,prefix+' left',[x-1.1,top+1.4,z],[.16,1.4,.2],model),
        entity(first+1,prefix+' right',[x+1.1,top+1.4,z],[.16,1.4,.2],model),
        entity(first+2,prefix+' lintel',[x,top+2.95,z],[1.26,.15,.2],model),
        entity(first+3,prefix+' pad',[x,top-.025,z],[1.3,.025,.85])]

scene=read('parkour'); workshop=read('workshop')
scene['game']='physics_parkour'
# Remove the old finish portal; its island becomes the workshop approach.
scene['entities']=[e for e in scene['entities'] if not e.get('name',{}).get('name','').startswith('Finish return')]
scene['gameplay']['portals']=scene['gameplay']['portals'][:1]
scene['gameplay']['parkour']['fallY']=1
config=copy.deepcopy(workshop['gameplay']['workshop'])
config['plate'][1]+=8.9; config['plate'][2]-=52
config['movingPlatforms']=[{'entityId':11000,'travel':[0,0,-2],'period':10},
    {'entityId':11001,'travel':[3.6,0,0],'period':12}]
scene['gameplay']['workshop']=config
for e in workshop['entities']:
    name=e.get('name',{}).get('name','')
    if 'light' in e or 'skybox' in e or 'camera' in e or name in ('Workshop back wall','Upper back rail') or 'return' in name:
        continue
    e=copy.deepcopy(e); p=e['transform']['position']; p[1]+=8.9; p[2]-=52
    scene['entities'].append(e)
scene['entities'] += [
    entity(11000,'Longitudinal shuttle',[0,12.65,-68.2],[1.3,.25,1.3]),
    entity(11001,'Lateral shuttle',[-1.8,12.65,-77.6],[1.3,.25,1.3]),
    entity(11002,'Moving course rest island',[0,12.65,-73.4],[3.2,.25,1.8]),
    entity(11003,'Pool approach checkpoint',[0,12.65,-81.8],[3.2,.25,1.8]),
    entity(11004,'Pool entry step',[0,12.1,-84],[3.2,.3,.4]),
    entity(11005,'Pool floor',[0,10.35,-89.4],[3.2,.25,5]),
    entity(11006,'Pool left wall',[-3.4,11.9,-89.4],[.2,1.55,5.4]),
    entity(11007,'Pool right wall',[3.4,11.9,-89.4],[.2,1.55,5.4]),
    entity(11008,'Final deck',[0,12.55,-99],[3.2,.35,2.4]),
    entity(11009,'Final back railing',[0,13.35,-101.6],[3.4,.45,.12]),
]
# Shallow stairs start below the swimming feet height, with <=0.3 m rises.
for i in range(5):
    top=11.4+.3*i
    scene['entities'].append(entity(11100+i,'Pool exit stair', [0,top-.2,-94.4-.55*i],[3.2,.2,.3]))
scene['water']={'enabled':True,'height':12.4,'size':10,'center':[0,0,-89.4],
    'halfExtents':[3.2,0,5],'roughness':.08,'color':[.035,.2,.25]}
# Safe islands save progress after the workshop and before the pool.
for z,hx,hz in [(-64,2.8,2.2),(-81.8,2.8,1.5),(-99,2.8,1.9)]:
    scene['gameplay']['parkour']['checkpoints'].append({'spawn':[0,13.88,z], 'halfExtents':[hx,.2,hz]})
scene['entities']+=gate(11200,0,-100,12.9,'Combined finish')
scene['gameplay']['portals'].append({'target':'scenes/main.json','label':'COURSE CLEAR - RETURN TO HUB',
    'position':[0,13.88,-100],'radius':.8,'color':[.35,.18,1],'opacity':.32})
# Bright markers on stationary rest islands; moving cubes themselves use live
# physics transforms, so no decorative geometry is left behind when they move.
for i,(x,z) in enumerate([(-2.6,-73.4),(2.6,-73.4),(-2.6,-81.8),(2.6,-81.8),(-2.6,-99),(2.6,-99)]):
    scene['entities'].append(entity(11300+i,'Safe island beacon',[x,13.3,z],[.07,.4,.07],'models/parkour/cyan.glb'))
write('physics_parkour',scene)
main=read('main')
main['entities']=[e for e in main['entities'] if not e.get('name',{}).get('name','').startswith(('ParkourGate','WorkshopGate','CombinedGate'))]
portals=main['gameplay'].setdefault('portals',[])
portals[:]=[p for p in portals if p['target'] not in ('scenes/parkour.json','scenes/workshop.json','scenes/physics_parkour.json')]
portals.append({'target':'scenes/physics_parkour.json','label':'PHYSICS + PARKOUR - WALK THROUGH GATE',
    'position':[5,.02,-4],'radius':.85,'color':[.03,.48,1],'opacity':.32})
ground=next(e for e in main['entities'] if e.get('name',{}).get('name')=='Ground')
top=ground['transform']['position'][1]+abs(ground['transform']['scale'][1])
main['entities']+=gate(11400,5,-4,top,'CombinedGate')
write('main',main)
print('Merged course: 12 jumps, crate/gate/lift, 2 moving platforms, bounded pool with exit stairs, 6 checkpoints.')
