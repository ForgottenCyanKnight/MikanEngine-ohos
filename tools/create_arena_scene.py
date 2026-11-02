"""Author a central-spawn arena with four cardinal monster doors."""
from pathlib import Path
import copy
import json
import math

ROOT=Path(__file__).resolve().parents[1]/'ohos-project/entry/src/main/resources/rawfile'
hub=json.loads((ROOT/'scenes/main.json').read_text(encoding='utf-8'))
def cube(i,name,p,s,model='models/hub/white.glb',yaw=0,solid=True):
    e={'id':i,'name':{'name':name},'transform':{'position':p,
        'rotation':[math.cos(yaw/2),0,math.sin(yaw/2),0],'scale':s},
        'mesh':{'type':4,'modelPath':model},'render':{'visible':True,'castShadow':solid,'receiveShadow':True}}
    if solid:e['collider']={'type':0,'size':[2,2,2],'syncWithModel':True}
    return e
scene={'formatVersion':1,'game':'arena','water':{'enabled':False},
    'gameplay':{'playerSpawn':[0,.02,0],'enemyEnabled':False,'demoPropsEnabled':False,'groundEnabled':False,
        'arena':{'spawnGates':[[0,.02,-8.2],[8.2,.02,0],[0,.02,8.2],[-8.2,.02,0]]},
        'portals':[{'target':'scenes/main.json','label':'RETURN TO HUB','position':[-8,.02,8],
            'radius':.85,'color':[.7,.3,1],'opacity':.32}]},
    'entities':[copy.deepcopy(e) for e in hub['entities'] if 'camera' in e or 'skybox' in e or 'light' in e]}
scene['entities'] += [cube(14000,'Arena floor',[0,-1.44,0],[12,.5,12]),
    cube(14001,'Arena north wall',[0,.56,-12],[12,1.5,.25]),
    cube(14002,'Arena south wall',[0,.56,12],[12,1.5,.25]),
    cube(14003,'Arena west wall',[-12,.56,0],[.25,1.5,12]),
    cube(14004,'Arena east wall',[12,.56,0],[.25,1.5,12])]
for i,(x,z,yaw) in enumerate([(0,-9.5,0),(9.5,0,math.pi/2),(0,9.5,0),(-9.5,0,math.pi/2)]):
    color='models/parkour/'+('amber' if i%2 else 'cyan')+'.glb'
    for j,(dx,y,scale) in enumerate([(-1.2,.46,[.18,1.4,.25]),(1.2,.46,[.18,1.4,.25]),(0,2.01,[1.38,.15,.25])]):
        scene['entities'].append(cube(14100+i*10+j,f'Spawn door {i+1} frame {j}',
            [x+dx*math.cos(yaw),y,z-dx*math.sin(yaw)],scale,color,yaw))
    scene['entities'].append(cube(14103+i*10,f'Spawn door {i+1} membrane',[x,.46,z],
        [1,1.4,.025],color,yaw,False))
# The return portal sits off the central combat lanes, distinct from spawn doors.
x,z=-8,8
for j,(dx,y,scale) in enumerate([(-1.1,.46,[.16,1.4,.2]),(1.1,.46,[.16,1.4,.2]),(0,2.01,[1.26,.15,.2])]):
    scene['entities'].append(cube(14200+j,'Arena return gate',[x+dx,y,z],scale,'models/parkour/violet.glb'))
(ROOT/'scenes/arena.json').write_text(json.dumps(scene,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
print('Arena: central spawn, four cardinal doors, return portal; runtime waves 1/2/3/4.')
