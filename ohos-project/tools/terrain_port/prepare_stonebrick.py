"""Split the supplied four-quadrant atlas and reuse the existing cube geometry."""
import argparse
import hashlib
import json
import pathlib
import struct
from PIL import Image


def generate(source, cube, output):
    output.mkdir(parents=True, exist_ok=True)
    original = source.read_bytes()
    atlas = Image.open(source).convert('RGBA')
    w, h = atlas.size
    if w != h or w % 2: raise ValueError('Expected an even square 2x2 atlas')
    size = w // 2
    color = atlas.crop((0, 0, size, size))
    roughness = atlas.crop((size, 0, w, size)).getchannel('R')
    normal = atlas.crop((0, size, size, h))
    ao = atlas.crop((size, size, w, h)).getchannel('R')
    orm = Image.merge('RGBA', (ao, roughness, Image.new('L',(size,size),0), Image.new('L',(size,size),255)))
    (output / 'source_atlas.png').write_bytes(original)
    for name, image in [('albedo',color),('normal',normal),('orm',orm)]:
        image.save(output / (name + '.png'))
    encoded = cube.read_bytes()
    length, kind = struct.unpack_from('<II',encoded,12)
    if kind != 0x4e4f534a: raise ValueError('Missing GLB JSON chunk')
    document = json.loads(encoded[20:20+length])
    binary_size, binary_kind = struct.unpack_from('<II',encoded,20+length)
    if binary_kind != 0x004e4942: raise ValueError('Missing GLB binary chunk')
    binary = encoded[28+length:28+length+binary_size]
    (output / 'cube.bin').write_bytes(binary)
    document['buffers'][0]['uri'] = 'cube.bin'
    document['images'] = [{'uri':name+'.png'} for name in ['albedo','normal','orm']]
    document['samplers'] = [{'wrapS':10497,'wrapT':10497,'minFilter':9987,'magFilter':9729}]
    document['textures'] = [{'source':i,'sampler':0} for i in range(3)]
    document['materials'] = [{
        'name':'CrackedStonebrick',
        'pbrMetallicRoughness':{'baseColorFactor':[1,1,1,1],'baseColorTexture':{'index':0},
            'metallicFactor':0,'roughnessFactor':1,'metallicRoughnessTexture':{'index':2}},
        'normalTexture':{'index':1,'scale':1},'occlusionTexture':{'index':2,'strength':1}
    }]
    for mesh in document['meshes']:
        for primitive in mesh['primitives']: primitive['material']=0
    (output / 'cube.gltf').write_text(json.dumps(document,indent=2)+'\n',encoding='utf-8')
    metadata={'sourceSha256':hashlib.sha256(original).hexdigest(), 'quadrantSize':size,
        'layout':{'topLeft':'unmodified albedo','topRight':'roughness, red channel',
            'bottomLeft':'unmodified tangent-space normal','bottomRight':'occlusion, red channel'},
        'outputORM':'R=occlusion, G=roughness, B=0 (nonmetal)',
        'rendererMapping':'world-space dominant-axis cube mapping, one repeat per metre'}
    (output/'conversion.json').write_text(json.dumps(metadata,indent=2)+'\n',encoding='utf-8')
    assert Image.open(output/'albedo.png').tobytes()==color.tobytes()
    assert Image.open(output/'normal.png').tobytes()==normal.tobytes()
    assert Image.open(output/'orm.png').getchannel('G').tobytes()==roughness.tobytes()
    assert Image.open(output/'orm.png').getchannel('R').tobytes()==ao.tobytes()
    print('PASS: original albedo/normal preserved; ORM repacked; cube geometry preserved')

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=pathlib.Path,required=True)
    parser.add_argument('--cube',type=pathlib.Path,required=True)
    parser.add_argument('--output',type=pathlib.Path,required=True)
    args=parser.parse_args()
    generate(args.source,args.cube,args.output)
