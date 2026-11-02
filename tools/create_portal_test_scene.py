"""Build the small independent forest scene and the original scene's test gate."""
from pathlib import Path
import copy,json,math,struct,random
from PIL import Image
ROOT=Path(__file__).resolve().parents[1]/'ohos-project/entry/src/main/resources/rawfile'

def hill(x,z):
    return .18+.95*math.exp(-((x+6)**2+(z+6)**2)/19)+.70*math.exp(-((x-6)**2+(z+7)**2)/15)+.30*math.exp(-((x-7)**2+(z-5)**2)/12)

def entity(i,name,pos,scale,model='engine/models/Base Model/cube.glb',yaw=0):
    return {'id':i,'name':{'name':name},'transform':{'position':pos,'rotation':[math.cos(yaw/2),0,math.sin(yaw/2),0],'scale':scale},'mesh':{'type':4,'modelPath':model},'render':{'visible':True,'castShadow':True,'receiveShadow':True}}

def gate(first,x,z,ground):
    return [entity(first,'PortalTest_Left',[x-1.1,ground+1.4,z],[.16,1.4,.2]),entity(first+1,'PortalTest_Right',[x+1.1,ground+1.4,z],[.16,1.4,.2]),entity(first+2,'PortalTest_Top',[x,ground+2.95,z],[1.26,.15,.2]),entity(first+3,'PortalTest_Pad',[x,ground-.025,z],[1.3,.025,.85])]

mainpath=ROOT/'scenes/main.json'
main=json.loads(mainpath.read_text(encoding='utf-8'))
main['entities']=[e for e in main['entities'] if not e.get('name',{}).get('name','').startswith('PortalTest_')]
ground=next((e for e in main['entities'] if e.get('name',{}).get('name')=='Ground'), {'transform':{'position':[0,-1.14,0],'scale':[20,.2,20]}})
authored_top=ground['transform']['position'][1]+abs(ground['transform']['scale'][1])
main.setdefault('gameplay',{})['portal']={'target':'scenes/forest_test.json','label':'FOREST TEST - WALK THROUGH GATE','position':[0,.02,-4],'radius':.85,'color':[.08,.85,.4],'opacity':.32}
main['entities']+=gate(9100,0,-4,authored_top)
mainpath.write_text(json.dumps(main,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
# Sixteen-bit height data: PNG top row corresponds to local +Z.
image=Image.new('I;16',(65,65))
for row in range(65):
 for col in range(65):
    x=(col/64-.5)*36;z=(.5-row/64)*36
    image.putpixel((col,row),round(hill(x,z)/2*65535))
image.save(ROOT/'terrain/forest_test_height16.png')
scene={'formatVersion':1,'game':'forest_portal_test','water':{'enabled':False},'gameplay':{'playerSpawn':[0,round(hill(0,0)+.02,4),0],'enemyEnabled':False,'demoPropsEnabled':False,'portal':{'target':'scenes/main.json','label':'RETURN - WALK THROUGH GATE','position':[0,round(hill(0,3)+.02,4),3],'radius':.85,'color':[.35,.18,1.0],'opacity':.32}},'entities':[]}
for e in main['entities']:
 if 'light' in e and e['light'].get('type')==0 or 'skybox' in e or 'camera' in e:
    scene['entities'].append(copy.deepcopy(e))
terrain=json.loads((ROOT/'scenes/terrain_demo.json').read_text())['entities'][0]
terrain['transform']['position']=[0,-.94,0]
terrain['terrain'].update({'heightmapPath':'terrain/forest_test_height16.png','worldSize':[36,36],'heightScale':2,'materialTiling':10})
scene['entities'].append(terrain)
# The loader normalises every model's largest dimension to 2 and centres it.
# Compute the baked bounding-box height so all props sit on the heightmap.
def halfheight(name):
 b=(ROOT/f'models/EZTree/{name}.glb').read_bytes();n=struct.unpack_from('<I',b,12)[0];d=json.loads(b[20:20+n])
 bounds=[]
 def visit(idx,parent):
    node=d['nodes'][idx];t=node.get('translation',[0,0,0]);q=node.get('rotation',[0,0,0,1]);sc=node.get('scale',[1,1,1])
    def tr(v):
     v=[v[i]*sc[i] for i in range(3)];qx,qy,qz,qw=q
     uv=[qy*v[2]-qz*v[1],qz*v[0]-qx*v[2],qx*v[1]-qy*v[0]]
     uuv=[qy*uv[2]-qz*uv[1],qz*uv[0]-qx*uv[2],qx*uv[1]-qy*uv[0]]
     return parent([v[i]+2*(qw*uv[i]+uuv[i])+t[i] for i in range(3)])
    if 'mesh' in node:
     for p in d['meshes'][node['mesh']]['primitives']:
      a=d['accessors'][p['attributes']['POSITION']]
      for x in [a['min'][0],a['max'][0]]:
       for y in [a['min'][1],a['max'][1]]:
        for z in [a['min'][2],a['max'][2]]:bounds.append(tr([x,y,z]))
    for child in node.get('children',[]):visit(child,tr)
 for idx in d['scenes'][d.get('scene',0)]['nodes']:visit(idx,lambda v:v)
 spans=[max(v[i] for v in bounds)-min(v[i] for v in bounds) for i in range(3)]
 return spans[1]/max(spans)
props=[('tree',x,z,s) for x,z,s in [(-5,-4,2.5),(5,-5,2.8),(-7,1,2.4),(7,2,2.6),(-4,-9,2.7),(3,-9,2.4),(-8,7,2.2),(7,7,2.5)]]
props += [('rock1',-3,-5,.65),('rock2',4,-3,.8),('rock3',-6,4,.65),('rock1',6,5,.55)]
rng=random.Random(20261102)
for kind,count in [('grass',150),('flowers',60)]:
 for i in range(count):
  while True:
   x=rng.uniform(-16.5,16.5);z=rng.uniform(-16.5,16.5)
   if x*x+z*z>4 and x*x+(z-3)**2>6.25:break
  name=kind if kind=='grass' else ['flower_blue','flower_white','flower_yellow'][i%3]
  props.append((name,x,z,rng.uniform(.3,.48)))
props += [('tree',x,z,2.5) for x,z in [(-13,-12),(13,-12),(-14,7),(14,8),(-7,14),(8,14)]]
heights={name:halfheight(name) for name,_,_,_ in props}
for i,(name,x,z,s) in enumerate(props):
 scene['entities'].append(entity(20000+i,f'EZTree_{name}_{i}',[x,round(-.94+hill(x,z)+heights[name]*s-.025,4),z],[s,s,s],f'models/EZTree/{name}.glb',i*.83))
scene['entities']+=gate(9100,0,3,-.94+hill(0,3))
# Low stone perimeter keeps the player inside the loaded terrain.
for i,(pos,scale) in enumerate([([0,-.3,-18],[18,1.7,.25]),([0,-.3,18],[18,1.7,.25]),([-18,-.3,0],[.25,1.7,18]),([18,-.3,0],[.25,1.7,18])]):
 scene['entities'].append(entity(9300+i,'Forest boundary',pos,scale))
(ROOT/'scenes/forest_test.json').write_text(json.dumps(scene,ensure_ascii=False,indent=2)+'\n',encoding='utf-8',newline='\n')
print(f'Created 36x36m forest, {len(props)} EZTree props, two reciprocal portals.')
