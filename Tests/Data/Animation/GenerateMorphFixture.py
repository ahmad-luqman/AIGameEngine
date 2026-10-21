"""Rebuild the morph target model in Animation, FeatureTest and the FuzzGltf corpus.
Face is a skinned 3x3 grid facing +Z with two named targets: Smile pushes the middle row toward +Z (with NORMAL
deltas) and Blink, a sparse target without NORMAL deltas, pushes the top row back (-Z), tilting the upper quads. Teeth is a rigid triangle under the Head
joint whose single target is also named Smile. The Face node overrides the mesh's default weights.
"""
import base64
import json
from pathlib import Path
import struct

blob = bytearray()
views = []
accessors = []


def view(raw):
    while len(blob) % 4:
        blob.append(0)
    views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(raw)))
    blob.extend(raw)
    return len(views) - 1


def accessor(values, width, kind='f', **extra):
    raw = struct.pack('<' + kind * len(values), *values)
    accessors.append(dict(bufferView=view(raw), componentType={'f': 5126, 'H': 5123, 'B': 5121}[kind],
                          count=len(values) // width, type={1: 'SCALAR', 2: 'VEC2', 3: 'VEC3', 4: 'VEC4', 16: 'MAT4'}[width], **extra))
    return len(accessors) - 1


def sparse(count, indices, values):
    # A sparse VEC3 accessor without a bufferView: zero everywhere except `indices`.
    index_view = view(struct.pack('<' + 'H' * len(indices), *indices))
    value_view = view(struct.pack('<' + 'f' * len(values), *values))
    accessors.append(dict(componentType=5126, count=count, type='VEC3',
                          min=[min(0, *values[i::3]) for i in range(3)], max=[max(0, *values[i::3]) for i in range(3)],
                          sparse=dict(count=len(indices), indices=dict(bufferView=index_view, componentType=5123), values=dict(bufferView=value_view))))
    return len(accessors) - 1


grid = [(x, y, 0.0) for y in (0.0, 0.5, 1.0) for x in (-0.5, 0.0, 0.5)]
positions = accessor([c for p in grid for c in p], 3, min=[-0.5, 0, 0], max=[0.5, 1, 0])
normals = accessor([0, 0, 1] * 9, 3)
joints = accessor([0, 0, 0, 0] * 9, 4, 'B')
weights = accessor([1, 0, 0, 0] * 9, 4)
quads = [(0, 1, 4, 3), (1, 2, 5, 4), (3, 4, 7, 6), (4, 5, 8, 7)]
indices = accessor([i for a, b, c, d in quads for i in (a, b, c, a, c, d)], 1, 'H')
smile = [c for v in range(9) for c in ((0, 0, 0.3) if 3 <= v <= 5 else (0, 0, 0))]
smile_positions = accessor(smile, 3, min=[0, 0, 0], max=[0, 0, 0.3])
smile_normals = accessor([c for v in range(9) for c in ((0, -0.2, 0) if 3 <= v <= 5 else (0, 0, 0))], 3)
blink_positions = sparse(9, [6, 7, 8], [0, 0, -0.25] * 3)
teeth_positions = accessor([-0.1, 0.4, 0.05, 0.1, 0.4, 0.05, 0, 0.5, 0.05], 3, min=[-0.1, 0.4, 0.05], max=[0.1, 0.5, 0.05])
teeth_smile = accessor([0.1, 0, 0] * 3, 3, min=[0.1, 0, 0], max=[0.1, 0, 0])
teeth_indices = accessor([0, 1, 2], 1, 'H')
identity = [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1]
binds = accessor(identity, 16)

times = accessor([0, 1, 2], 1, min=[0], max=[2])
talk_face = accessor([0, 0, 1, 0.5, 0, 0], 1)
step_times = accessor([0, 1], 1, min=[0], max=[1])
talk_teeth = accessor([0.2, 0.8], 1)
# Per key: in-tangents, values, out-tangents (two weights each).
cubic_face = accessor([0, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0], 1)
nod_rotations = accessor([0, 0, 0, 1, 0.38268343, 0, 0, 0.92387953], 4)

model = dict(
    asset=dict(version='2.0', generator='Basalt test fixture'),
    buffers=[dict(byteLength=len(blob), uri='data:application/octet-stream;base64,' + base64.b64encode(blob).decode())],
    bufferViews=views, accessors=accessors,
    materials=[dict(name='Skin', pbrMetallicRoughness=dict(baseColorFactor=[0.9, 0.6, 0.5, 1], metallicFactor=0, roughnessFactor=0.6), doubleSided=True)],
    meshes=[dict(name='Face', weights=[0, 0.25], extras=dict(targetNames=['Smile', 'Blink']),
                 primitives=[dict(attributes=dict(POSITION=positions, NORMAL=normals, JOINTS_0=joints, WEIGHTS_0=weights), indices=indices, material=0,
                                  targets=[dict(POSITION=smile_positions, NORMAL=smile_normals), dict(POSITION=blink_positions)])]),
            dict(name='Teeth', extras=dict(targetNames=['Smile']),
                 primitives=[dict(attributes=dict(POSITION=teeth_positions), indices=teeth_indices, material=0, targets=[dict(POSITION=teeth_smile)])])],
    nodes=[dict(name='Root', children=[1, 2]),
           dict(name='Head', children=[3]),
           dict(name='Face', mesh=0, skin=0, weights=[0.5, 0.25]),
           dict(name='Teeth', mesh=1)],
    skins=[dict(joints=[1], inverseBindMatrices=binds, skeleton=1)],
    animations=[dict(name='Talk', samplers=[dict(input=times, output=talk_face, interpolation='LINEAR'), dict(input=step_times, output=talk_teeth, interpolation='STEP')],
                     channels=[dict(sampler=0, target=dict(node=2, path='weights')), dict(sampler=1, target=dict(node=3, path='weights'))]),
                dict(name='CubicSmile', samplers=[dict(input=step_times, output=cubic_face, interpolation='CUBICSPLINE')],
                     channels=[dict(sampler=0, target=dict(node=2, path='weights'))]),
                dict(name='Nod', samplers=[dict(input=step_times, output=nod_rotations, interpolation='LINEAR')],
                     channels=[dict(sampler=0, target=dict(node=1, path='rotation'))])],
    scenes=[dict(nodes=[0])], scene=0)

root = Path(__file__).resolve().parent
text = json.dumps(model, indent=2) + '\n'
(root / 'Morph.gltf').write_text(text)
feature = root.parent / 'FeatureTest/Assets/Models/Morph.gltf'
feature.parent.mkdir(parents=True, exist_ok=True)
feature.write_text(text)
(root.parents[1] / 'Fuzz/Corpus/FuzzGltf/Morph.gltf').write_bytes(b'\0' + text.encode())

