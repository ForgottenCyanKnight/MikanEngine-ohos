"""Create the compact crate / pressure plate / lift technical demo."""
from pathlib import Path
import copy
import json

ROOT=Path(__file__).resolve().parents[1]/'ohos-project/entry/src/main/resources/rawfile'
def entity(i,name,pos,scale,model='engine/models/Base Model/cube.glb'):
    return {'id':i,'name':{'name':name},'transform':{'position':pos,'rotation':[1,0,0,0],'scale':scale},
        'mesh':{'type':4,'modelPath':model},'render':{'visible':True,'castShadow':not name.startswith(('Workshop route marking','Plate edge light','Pressure plate')),'receiveShadow':True}}
def gate(first,x,z,top,prefix):
    model='models/parkour/cyan.glb'
    return [entity(first,prefix+' left',[x-1.1,top+1.4,z],[.16,1.4,.2],model),
        entity(first+1,prefix+' right',[x+1.1,top+1.4,z],[.16,1.4,.2],model),
        entity(first+2,prefix+' lintel',[x,top+2.95,z],[1.26,.15,.2],model),
        entity(first+3,prefix+' pad',[x,top-.025,z],[1.3,.025,.85])]
mainpath=ROOT/'scenes/main.json'
main=json.loads(mainpath.read_text(encoding='utf-8'))
main['entities']=[e for e in main['entities'] if not e.get('name',{}).get('name','').startswith('WorkshopGate')]
ground=next(e for e in main['entities'] if e.get('name',{}).get('name')=='Ground')
top=ground['transform']['position'][1]+abs(ground['transform']['scale'][1])
portals=main.setdefault('gameplay',{}).setdefault('portals',[])
portals[:]=[p for p in portals if p['target']!='scenes/workshop.json']
# Right of both existing entrances, clear of the demo blocks.
portals.append({'target':'scenes/workshop.json','label':'PHYSICS WORKSHOP - WALK THROUGH GATE',
    'position':[9,.02,-4],'radius':.85,'color':[.03,.48,1],'opacity':.32})
main['entities']+=gate(9900,9,-4,top,'WorkshopGate')
mainpath.write_text(json.dumps(main,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
scene={'formatVersion':1,'game':'physics_workshop','water':{'enabled':False},
    'gameplay':{'playerSpawn':[0,.98,6],'enemyEnabled':False,'demoPropsEnabled':False,'groundEnabled':False,
        'workshop':{'crateId':10001,'gateId':10002,'liftId':10003,'plate':[0,.04,-1.2],'liftTravel':4},
        'portals':[
            {'target':'scenes/main.json','label':'RETURN TO HUB','position':[-1.7,.98,7],'radius':.8,'color':[.35,.18,1],'opacity':.32},
            {'target':'scenes/main.json','label':'WORKSHOP CLEAR - RETURN TO HUB','position':[0,4.98,-13.5],'radius':.8,'color':[.35,.18,1],'opacity':.32}]},'entities':[]}
for e in main['entities']:
    if ('light' in e and e['light'].get('type')==0) or 'skybox' in e or 'camera' in e:
        scene['entities'].append(copy.deepcopy(e))
scene['entities'] += [
    entity(10000,'Workshop lower floor',[0,-.35,.8],[3.2,.35,7.2]),
    entity(10001,'Workshop push crate',[0,.65,2],[.6,.6,.6]),
    entity(10002,'Workshop sliding gate',[0,1.6,-4.6],[3.15,1.6,.18]),
    entity(10003,'Workshop lift',[0,-.25,-7.8],[1.4,.25,1.4]),
    entity(10004,'Workshop upper landing',[0,3.65,-12],[3.2,.35,2.8]),
    entity(10005,'Workshop left wall',[-3.4,1.1,.8],[.2,1.1,7.2]),
    entity(10006,'Workshop right wall',[3.4,1.1,.8],[.2,1.1,7.2]),
    entity(10007,'Workshop back wall',[0,1.1,8.2],[3.6,1.1,.2]),
    entity(10008,'Gate overhead frame',[0,3.5,-4.6],[3.6,.25,.4]),
    entity(10009,'Lift left guide',[-1.7,2,-7.8],[.12,2.4,.12]),
    entity(10010,'Lift right guide',[1.7,2,-7.8],[.12,2.4,.12]),
    entity(10011,'Upper left rail',[-3.3,4.45,-12],[.12,.45,2.8]),
    entity(10012,'Upper right rail',[3.3,4.45,-12],[.12,.45,2.8]),
    entity(10013,'Upper back rail',[0,4.45,-15],[3.4,.45,.12]),
    entity(10014,'Pressure plate',[0,.025,-1.2],[1,.025,1],'models/parkour/amber.glb'),
]
# Low markings lead from the crate to the plate and from the gate to the lift.
for i,z in enumerate([4.5,3.6,.6,-.2,-2.7,-3.6,-5.5]):
    scene['entities'].append(entity(10100+i,'Workshop route marking',[0,.012,z],[.15,.012,.18],'models/parkour/cyan.glb'))
for i,x in enumerate([-1,1]):
    scene['entities'].append(entity(10110+i,'Plate edge light',[x,.05,-1.2],[.035,.025,1],'models/parkour/amber.glb'))
scene['entities']+=gate(10200,-1.7,7,0,'Workshop start return')
scene['entities']+=gate(10210,0,-13.5,4,'Workshop finish return')
(ROOT/'scenes/workshop.json').write_text(json.dumps(scene,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
print('Created workshop: push crate, latching plate, sliding gate, 4m boarding lift, two return gates.')
