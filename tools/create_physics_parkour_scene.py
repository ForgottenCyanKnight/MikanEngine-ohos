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
# Static jump trio, timing ferry, then three independent lateral jump bricks.
scene['entities']=[e for e in scene['entities'] if 'camera' in e or 'light' in e or 'skybox' in e]
scene['gameplay']['portals']=scene['gameplay']['portals'][:1]
scene['gameplay']['parkour']={'fallY':1,'revision':3,'completionReward':100,'checkpoints':[
    {'spawn':[0,6.98,0],'halfExtents':[2.4,.2,2.4]},
    {'spawn':[0,7.98,-15.05],'halfExtents':[2,.2,2]},
    {'spawn':[0,8.28,-26.5],'halfExtents':[2,.2,2]},
    {'spawn':[0,9.28,-41.1],'halfExtents':[2.8,.2,1.1]},
    {'spawn':[0,13.28,-59.4],'halfExtents':[2.8,.2,2.2]},
    {'spawn':[0,13.28,-63.9],'halfExtents':[2.8,.2,1.5]},
    {'spawn':[0,13.28,-81.3],'halfExtents':[2.8,.2,1.9]}]}
platforms=[(9500,0,0,6,2.6,2.6,'cyan'),
           (9510,0,-4.65,6.3,1.3,1.3,'cyan'),
           (9520,0,-7.9,6.65,1.2,1.2,'cyan'),
           (9530,0,-11,7,1.15,1.15,'cyan'),
           (9540,0,-15.05,7,2.2,2.2,'cyan'),
           (9550,0,-19.5,7.3,1.5,1.5,'amber'),
           (9560,0,-26.5,7.3,2.2,2.2,'amber'),
           (9570,-1,-30.8,7.6,1.2,1.2,'violet'),
           (9580,1.1,-34.1,7.95,1.1,1.2,'violet'),
           (9590,-1,-37.4,8.3,1.1,1.2,'violet')]
moving_ids={9550,9570,9580,9590}
for index,(first,x,z,top,hx,hz,color) in enumerate(platforms):
    scene['entities'].append(entity(first,f'Course_{index:02d}',[x,top-.25,z],[hx,.25,hz]))
    marker=f'models/parkour/{color}.glb'
    for side in [-1,1]:
        trim=entity(first+1+(side==1),f'Platform {index+1} moving light strip',
            [x+side*(hx-.08),top+.012,z],[.045,.012,hz-.08],marker)
        trim['render']['castShadow']=False
        if first in moving_ids: trim['motionParent']=first
        scene['entities'].append(trim)
    for side in [-1,1]:
        arrow=entity(first+3+(side==1),f'Platform {index+1} arrow',
            [x+side*.14,top+.015,z-.16],[.035,.015,.24],marker)
        arrow['render']['castShadow']=False
        if first in moving_ids: arrow['motionParent']=first
        scene['entities'].append(arrow)
scene['entities']+=gate(9800,0,1.4,6,'Start return')
config=copy.deepcopy(workshop['gameplay']['workshop'])
config['plate'][1]+=8.3;config['plate'][2]-=47.4
config['movingPlatforms']=[{'entityId':9550,'travel':[0,0,-2.5],'period':10},
    {'entityId':9570,'travel':[2,0,0],'period':8},
    {'entityId':9580,'travel':[-2.2,0,0],'period':9},
    {'entityId':9590,'travel':[2,0,0],'period':10}]
scene['gameplay']['workshop']=config
for e in workshop['entities']:
    name=e.get('name',{}).get('name','')
    if 'light' in e or 'skybox' in e or 'camera' in e or name in ('Workshop back wall','Upper back rail') or 'return' in name:
        continue
    e=copy.deepcopy(e);p=e['transform']['position'];p[1]+=8.3;p[2]-=47.4
    scene['entities'].append(e)
# The lift feeds directly into a short stationary pool approach. No duplicate
# moving-platform section after the lift.
scene['entities'] += [
    entity(11003,'Pool approach checkpoint',[0,12.05,-63.9],[3.2,.25,2]),
    entity(11004,'Pool entry step',[0,11.5,-66.3],[3.2,.3,.4]),
    entity(11005,'Pool floor',[0,9.75,-71.7],[3.2,.25,5]),
    entity(11006,'Pool left wall',[-3.4,11.3,-71.7],[.2,1.55,5.4]),
    entity(11007,'Pool right wall',[3.4,11.3,-71.7],[.2,1.55,5.4]),
    entity(11008,'Final deck',[0,11.95,-81.3],[3.2,.35,2.4]),
    entity(11009,'Final back railing',[0,12.75,-83.9],[3.4,.45,.12])]
for i in range(5):
    top=10.8+.3*i
    scene['entities'].append(entity(11100+i,'Pool exit stair',[0,top-.2,-76.7-.55*i],[3.2,.2,.3]))
scene['water']={'enabled':True,'height':11.8,'size':10,'center':[0,0,-71.7],
    'halfExtents':[3.2,0,5],'roughness':.08,'color':[.035,.2,.25]}
scene['entities']+=gate(11200,0,-82.3,12.3,'Combined finish')
scene['gameplay']['portals'].append({'target':'scenes/main.json','label':'COURSE CLEAR - RETURN TO HUB',
    'position':[0,13.28,-82.3],'radius':.8,'color':[.35,.18,1],'opacity':.32})
write('physics_parkour',scene)
main=read('main')
main['entities']=[e for e in main['entities'] if not e.get('name',{}).get('name','').startswith(('ParkourGate','WorkshopGate','CombinedGate'))]
portals=main['gameplay'].setdefault('portals',[])
portals[:]=[p for p in portals if p['target'] not in ('scenes/parkour.json','scenes/workshop.json','scenes/physics_parkour.json')]
portals.append({'target':'scenes/physics_parkour.json','label':'PHYSICS + PARKOUR - WALK THROUGH GATE',
    'position':[5,.02,-4],'radius':.85,'color':[.03,.48,1],'opacity':.32})
ground=next((e for e in main['entities'] if e.get('name',{}).get('name')=='Ground'), {'transform':{'position':[0,-1.14,0],'scale':[20,.2,20]}})
top=ground['transform']['position'][1]+abs(ground['transform']['scale'][1])
main['entities']+=gate(11400,5,-4,top,'CombinedGate')
write('main',main)
print('Course: 3 static bricks, rest, timing ferry, rest, 3 lateral bricks, crate/lift/pool; reward 100; 7 safe checkpoints.')
