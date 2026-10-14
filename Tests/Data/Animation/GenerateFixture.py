"""Rebuild the authored, redistributable two-joint glTF test model (no external assets)."""
import base64
import json
import math
from pathlib import Path
import struct

blob = bytearray()
views = []
accessors = []


def accessor(values, width, kind='f', **extra):
    while len(blob) % 4:
        blob.append(0)
    raw = struct.pack('<' + kind * len(values), *values)
    views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(raw)))
    blob.extend(raw)
    accessors.append(dict(bufferView=len(views)-1, componentType={'f':5126,'H':5123,'B':5121}[kind],
                          count=len(values)//width, type={1:'SCALAR',2:'VEC2',3:'VEC3',4:'VEC4',16:'MAT4'}[width], **extra))
    return len(accessors)-1

positions = accessor([x for y in (0,1,2) for p in ((-.25,y,0),(.25,y,0)) for x in p],3,min=[-.25,0,0],max=[.25,2,0])
normals = accessor([0,0,1]*6,3)
uvs = accessor([0,1,1,1,0,.5,1,.5,0,0,1,0],2)
joints = accessor([0,1,0,0]*6,4,'B')
weights = accessor([1,0,0,0]*2+[.5,.5,0,0]*2+[0,1,0,0]*2,4)
indices = accessor([0,1,2,1,3,2,2,3,4,3,5,4],1,'H')
identity = [1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1]
inverse = identity.copy(); inverse[13]=-1
binds = accessor(identity+inverse,16)
times = accessor([0,1,2],1,min=[0],max=[2])
rotations = accessor([0,0,0,1,0,0,math.sqrt(.5),math.sqrt(.5),0,0,0,1],4)
translations = accessor([0,0,0,2,0,0,0,0,0],3)
cubic = accessor([0,0,0, 0,0,0, 2,0,0, 0,0,0, 2,0,0, 0,0,0, -2,0,0, 0,0,0, 0,0,0],3)
model = dict(asset=dict(version='2.0',generator='Basalt test fixture'),
    buffers=[dict(byteLength=len(blob),uri='data:application/octet-stream;base64,'+base64.b64encode(blob).decode())],
    bufferViews=views,accessors=accessors,
    materials=[dict(name='Orange',doubleSided=True,pbrMetallicRoughness=dict(baseColorFactor=[1,.25,.05,1],metallicFactor=0,roughnessFactor=.6))],
    meshes=[dict(name='Strip',primitives=[dict(attributes=dict(POSITION=positions,NORMAL=normals,TEXCOORD_0=uvs,JOINTS_0=joints,WEIGHTS_0=weights),indices=indices,material=0)])],
    # Deliberately store a child before its parent; mesh transform must not affect skinning.
    nodes=[dict(name='Tip',translation=[0,1,0]),dict(name='Mesh',mesh=0,skin=0,translation=[7,0,0]),dict(name='RootJoint',children=[0])],
    skins=[dict(joints=[2,0],inverseBindMatrices=binds,skeleton=2)],
    animations=[dict(name='Bend',samplers=[dict(input=times,output=rotations,interpolation='LINEAR')],channels=[dict(sampler=0,target=dict(node=0,path='rotation'))]),
                dict(name='Step',samplers=[dict(input=times,output=translations,interpolation='STEP')],channels=[dict(sampler=0,target=dict(node=2,path='translation'))]),
                dict(name='Cubic',samplers=[dict(input=times,output=cubic,interpolation='CUBICSPLINE')],channels=[dict(sampler=0,target=dict(node=2,path='translation'))])],
    scenes=[dict(nodes=[1,2])],scene=0)
root = Path(__file__).resolve().parent
text = json.dumps(model,indent=2)+'\n'
(root/'TwoJoint.gltf').write_text(text)
feature = root.parent/'FeatureTest/Assets/Models/TwoJoint.gltf'
feature.parent.mkdir(parents=True,exist_ok=True)
feature.write_text(text)
(root.parents[1]/'Fuzz/Corpus/FuzzGltf/Animated.gltf').write_bytes(b'\0'+text.encode())
