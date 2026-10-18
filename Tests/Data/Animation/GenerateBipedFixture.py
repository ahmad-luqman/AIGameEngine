"""Rebuild the authored biped used by the layer and IK tests, in Animation and FeatureTest.
The rig faces +Z with its left side on +X. Every rest rotation is identity so the tests can check poses
analytically; the knees bend slightly forward so legs have a bend plane.
"""
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


def axis_angle(axis, degrees):
    half = math.radians(degrees) / 2
    s = math.sin(half)
    return [axis[0]*s, axis[1]*s, axis[2]*s, math.cos(half)]


# name, parent, translation relative to the parent
joints = [
    ('Hips', None, (0, 1.0, 0)),
    ('Spine', 'Hips', (0, 0.2, 0)),
    ('Chest', 'Spine', (0, 0.2, 0)),
    ('Head', 'Chest', (0, 0.3, 0)),
    ('LeftUpperArm', 'Chest', (0.2, 0.15, 0)),
    ('LeftForeArm', 'LeftUpperArm', (0.3, 0, 0)),
    ('LeftHand', 'LeftForeArm', (0.3, 0, 0)),
    ('RightUpperArm', 'Chest', (-0.2, 0.15, 0)),
    ('RightForeArm', 'RightUpperArm', (-0.3, 0, 0)),
    ('RightHand', 'RightForeArm', (-0.3, 0, 0)),
    ('LeftUpperLeg', 'Hips', (0.1, -0.05, 0)),
    ('LeftLowerLeg', 'LeftUpperLeg', (0, -0.45, 0.02)),
    ('LeftFoot', 'LeftLowerLeg', (0, -0.45, -0.02)),
    ('RightUpperLeg', 'Hips', (-0.1, -0.05, 0)),
    ('RightLowerLeg', 'RightUpperLeg', (0, -0.45, 0.02)),
    ('RightFoot', 'RightLowerLeg', (0, -0.45, -0.02)),
]
index = {name: i for i, (name, _, _) in enumerate(joints)}
world = []
for name, parent, offset in joints:
    base = world[index[parent]] if parent else (0, 0, 0)
    world.append(tuple(b + o for b, o in zip(base, offset)))

# A small quad at every joint, rigidly skinned to it.
positions, normals, skin_joints, weights, indices = [], [], [], [], []
for j, (x, y, z) in enumerate(world):
    first = len(positions) // 3
    for dx, dy in ((-.04, -.04), (.04, -.04), (-.04, .04), (.04, .04)):
        positions += [x + dx, y + dy, z]
        normals += [0, 0, 1]
        skin_joints += [j, 0, 0, 0]
        weights += [1, 0, 0, 0]
    indices += [first, first + 1, first + 2, first + 1, first + 3, first + 2]
xs, ys, zs = positions[0::3], positions[1::3], positions[2::3]
position_accessor = accessor(positions, 3, min=[min(xs), min(ys), min(zs)], max=[max(xs), max(ys), max(zs)])
normal_accessor = accessor(normals, 3)
joint_accessor = accessor(skin_joints, 4, 'B')
weight_accessor = accessor(weights, 4)
index_accessor = accessor(indices, 1, 'H')
binds = []
for x, y, z in world:
    binds += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1]
bind_accessor = accessor(binds, 16)

times = accessor([0, 0.5, 1], 1, min=[0], max=[1])
X, Z = (1, 0, 0), (0, 0, 1)


def rotations(axis, *degrees):
    return accessor([v for d in degrees for v in axis_angle(axis, d)], 4)


def clip(name, channels):
    samplers, targets = [], []
    for node, path, output in channels:
        samplers.append(dict(input=times, output=output, interpolation='LINEAR'))
        targets.append(dict(sampler=len(samplers) - 1, target=dict(node=index[node], path=path)))
    return dict(name=name, samplers=samplers, channels=targets)


animations = [
    # Legs swing in opposition; the hips bob.
    clip('Walk', [('LeftUpperLeg', 'rotation', rotations(X, 20, -20, 20)),
                  ('RightUpperLeg', 'rotation', rotations(X, -20, 20, -20)),
                  ('Hips', 'translation', accessor([0, 1.0, 0, 0, 1.02, 0, 0, 1.0, 0], 3))]),
    # Only the right arm.
    clip('Wave', [('RightUpperArm', 'rotation', rotations(Z, -45, -90, -45)),
                  ('RightForeArm', 'rotation', rotations(Z, 0, -30, 0))]),
    # Chest pitch 5 -> 15 -> 5 degrees: an additive delta of 10 degrees at 0.5 s from frame 0.
    clip('Breathe', [('Chest', 'rotation', rotations(X, 5, 15, 5))]),
    # Constant 20 degree roll of the spine.
    clip('Lean', [('Spine', 'rotation', rotations(Z, 20, 20, 20))]),
]

nodes = [dict(name='Mesh', mesh=0, skin=0)]
# A held prop with a fixed matrix: IK cannot turn such nodes, so the tests check that it says so.
prop = len(joints) + 1
for name, parent, offset in joints:
    node = dict(name=name, translation=list(offset))
    children = [index[child] + 1 for child, p, _ in joints if p == name]
    if name == 'LeftHand':
        children.append(prop)
    if children:
        node['children'] = children
    nodes.append(node)
nodes.append(dict(name='Prop', matrix=[1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0.1, 0, 0, 1]))
for animation in animations:
    for channel in animation['channels']:
        channel['target']['node'] += 1

model = dict(asset=dict(version='2.0', generator='Basalt test fixture'),
    buffers=[dict(byteLength=len(blob), uri='data:application/octet-stream;base64,' + base64.b64encode(blob).decode())],
    bufferViews=views, accessors=accessors,
    materials=[dict(name='Blue', doubleSided=True, pbrMetallicRoughness=dict(baseColorFactor=[.1, .4, 1, 1], metallicFactor=0, roughnessFactor=.6))],
    meshes=[dict(name='Joints', primitives=[dict(attributes=dict(POSITION=position_accessor, NORMAL=normal_accessor, JOINTS_0=joint_accessor, WEIGHTS_0=weight_accessor), indices=index_accessor, material=0)])],
    nodes=nodes,
    skins=[dict(joints=[i + 1 for i in range(len(joints))], inverseBindMatrices=bind_accessor, skeleton=1)],
    animations=animations,
    scenes=[dict(nodes=[0, 1])], scene=0)
root = Path(__file__).resolve().parent
text = json.dumps(model, indent=2) + '\n'
(root / 'Biped.gltf').write_text(text)
feature = root.parent / 'FeatureTest/Assets/Models/Biped.gltf'
feature.parent.mkdir(parents=True, exist_ok=True)
feature.write_text(text)
