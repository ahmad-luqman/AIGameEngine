"""Rebuild the retargeting rigs in Animation and FeatureTest, and the source rig in the FuzzGltf corpus.
MixamoRig is a Mixamo-style library: "mixamorig:" names under an FBX-style Armature (Z-up, centimetres, so rotated -90
degrees about X and scaled 0.01), every joint with its own arbitrary rest frame, legs 1.0 m to the hips. UnrealRig is
an Unreal-style character: different names, identity joint frames, a clavicle the library lacks, 1.4 m legs and its own
Idle clip. Both stand in the same T-pose facing +Z with their left side on +X, so bone directions must match after
retargeting.
"""
import base64
import json
import math
from pathlib import Path
import struct


class Builder:
    def __init__(self):
        self.blob = bytearray()
        self.views = []
        self.accessors = []

    def accessor(self, values, width, kind='f', **extra):
        while len(self.blob) % 4:
            self.blob.append(0)
        raw = struct.pack('<' + kind * len(values), *values)
        self.views.append(dict(buffer=0, byteOffset=len(self.blob), byteLength=len(raw)))
        self.blob.extend(raw)
        self.accessors.append(dict(bufferView=len(self.views) - 1, componentType={'f': 5126, 'H': 5123}[kind],
                                   count=len(values) // width, type={1: 'SCALAR', 3: 'VEC3', 4: 'VEC4'}[width], **extra))
        return len(self.accessors) - 1

    def model(self, **parts):
        return dict(asset=dict(version='2.0', generator='Basalt test fixture'),
                    buffers=[dict(byteLength=len(self.blob), uri='data:application/octet-stream;base64,' + base64.b64encode(self.blob).decode())],
                    bufferViews=self.views, accessors=self.accessors, **parts)


def qmul(a, b):
    ax, ay, az, aw = a
    bx, by, bz, bw = b
    return [aw * bx + ax * bw + ay * bz - az * by, aw * by - ax * bz + ay * bw + az * bx,
            aw * bz + ax * by - ay * bx + az * bw, aw * bw - ax * bx - ay * by - az * bz]


def qinv(q):
    return [-q[0], -q[1], -q[2], q[3]]


def axis_angle(axis, degrees):
    length = math.sqrt(sum(c * c for c in axis))
    half = math.radians(degrees) / 2
    s = math.sin(half) / length
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(half)]


def rotate(q, v):
    p = qmul(qmul(q, [v[0], v[1], v[2], 0]), qinv(q))
    return p[:3]


def triangle(builder, size):
    positions = builder.accessor([-size, 0, 0, size, 0, 0, 0, size, 0], 3, min=[-size, 0, 0], max=[size, size, 0])
    indices = builder.accessor([0, 1, 2], 1, 'H')
    return dict(primitives=[dict(attributes=dict(POSITION=positions), indices=indices, material=0)])


material = [dict(name='Marker', pbrMetallicRoughness=dict(baseColorFactor=[0.2, 0.6, 0.9, 1], metallicFactor=0, roughnessFactor=0.5), doubleSided=True)]

# --- Mixamo-style library --------------------------------------------------------------------------------------------
# name, parent, model-space T-pose position (metres)
mixamo = [
    ('Hips', None, (0, 1.0, 0)), ('Spine', 'Hips', (0, 1.1, 0)), ('Spine1', 'Spine', (0, 1.25, 0)),
    ('Neck', 'Spine1', (0, 1.45, 0)), ('Head', 'Neck', (0, 1.55, 0)),
    ('LeftArm', 'Spine1', (0.2, 1.4, 0)), ('LeftForeArm', 'LeftArm', (0.45, 1.4, 0)), ('LeftHand', 'LeftForeArm', (0.7, 1.4, 0)),
    ('RightArm', 'Spine1', (-0.2, 1.4, 0)), ('RightForeArm', 'RightArm', (-0.45, 1.4, 0)), ('RightHand', 'RightForeArm', (-0.7, 1.4, 0)),
    ('LeftUpLeg', 'Hips', (0.1, 0.95, 0)), ('LeftLeg', 'LeftUpLeg', (0.1, 0.5, 0)), ('LeftFoot', 'LeftLeg', (0.1, 0.08, 0)), ('LeftToeBase', 'LeftFoot', (0.1, 0.0, 0.12)),
    ('RightUpLeg', 'Hips', (-0.1, 0.95, 0)), ('RightLeg', 'RightUpLeg', (-0.1, 0.5, 0)), ('RightFoot', 'RightLeg', (-0.1, 0.08, 0)), ('RightToeBase', 'RightFoot', (-0.1, 0.0, 0.12)),
]
# Armature space is Z-up centimetres: model (X, Y, Z) = armature (x, z, -y) / 100.
armature_rotation = axis_angle((1, 0, 0), -90)


def to_armature(p):
    return (p[0] * 100, -p[2] * 100, p[1] * 100)


# Each joint's rest frame, in armature space: arbitrary, so the test fails unless rest orientations are corrected.
frames = {name: axis_angle((math.sin(i * 1.7) + 0.2, math.cos(i * 0.9), math.sin(i * 0.4 + 1)), 25 + 37 * i % 140) for i, (name, _, _) in enumerate(mixamo)}

source = Builder()
nodes = [dict(name='Armature', rotation=armature_rotation, scale=[0.01, 0.01, 0.01], children=[])]
index = {}
for name, parent, position in mixamo:
    index[name] = len(nodes)
    world = frames[name]
    if parent:
        parent_world = frames[parent]
        offset = [a - b for a, b in zip(to_armature(position), to_armature(dict((n, p) for n, _, p in mixamo)[parent]))]
    else:
        parent_world = [0, 0, 0, 1]
        offset = list(to_armature(position))
    nodes.append(dict(name='mixamorig:' + name, rotation=qmul(qinv(parent_world), world), translation=rotate(qinv(parent_world), offset), children=[]))
    nodes[index[parent] if parent else 0]['children'].append(len(nodes) - 1)
nodes.append(dict(name='Body', mesh=0))
nodes[0]['children'].append(len(nodes) - 1)
for node in nodes:
    if not node.get('children', True):
        del node['children']


def local_track(name, keys):
    # Rest local rotation turned about axes of the joint's own frame.
    rest = nodes[index[name]]['rotation']
    return [c for axis, degrees in keys for c in qmul(rest, axis_angle(axis, degrees))]


times = source.accessor([0, 0.5, 1], 1, min=[0], max=[1])
wave_arm = source.accessor(local_track('LeftArm', [((1, 0, 0), 0), ((1, 0.3, 0), 70), ((1, 0, 0), 0)]), 4)
wave_forearm = source.accessor(local_track('LeftForeArm', [((0, 0, 1), 0), ((0.2, 0, 1), 50), ((0, 0, 1), 0)]), 4)
hips_rest = nodes[index['Hips']]['translation']
walk_hips = source.accessor([c for dz, dy in ((0, 0), (0.25, -0.04), (0.5, 0)) for c in (hips_rest[0], hips_rest[1] - dz * 100, hips_rest[2] + dy * 100)], 3)
walk_left = source.accessor(local_track('LeftUpLeg', [((1, 0, 0), 0), ((1, 0, 0.2), 35), ((1, 0, 0), 0)]), 4)
walk_right = source.accessor(local_track('RightUpLeg', [((1, 0, 0), 0), ((1, 0, 0.2), -35), ((1, 0, 0), 0)]), 4)
walk_spine = source.accessor(local_track('Spine', [((0, 1, 0), 0), ((0, 1, 0.1), 15), ((0, 1, 0), 0)]), 4)
nod_times = source.accessor([0, 1], 1, min=[0], max=[1])
head_rest = nodes[index['Head']]['rotation']
# Cubic keys: in-tangent, value, out-tangent (flat tangents ease in and out).
flat = [0, 0, 0, 0]
nod = source.accessor(flat + qmul(head_rest, axis_angle((1, 0, 0), 0)) + flat + flat + qmul(head_rest, axis_angle((1, 0, 0), 40)) + flat, 4)
library = source.model(
    materials=material, meshes=[triangle(source, 5)], nodes=nodes,
    animations=[
        dict(name='Wave', samplers=[dict(input=times, output=wave_arm, interpolation='LINEAR'), dict(input=times, output=wave_forearm, interpolation='LINEAR')],
             channels=[dict(sampler=0, target=dict(node=index['LeftArm'], path='rotation')), dict(sampler=1, target=dict(node=index['LeftForeArm'], path='rotation'))]),
        dict(name='Walk', samplers=[dict(input=times, output=walk_hips, interpolation='LINEAR'), dict(input=times, output=walk_left, interpolation='LINEAR'),
                                    dict(input=times, output=walk_right, interpolation='LINEAR'), dict(input=times, output=walk_spine, interpolation='LINEAR')],
             channels=[dict(sampler=0, target=dict(node=index['Hips'], path='translation')), dict(sampler=1, target=dict(node=index['LeftUpLeg'], path='rotation')),
                       dict(sampler=2, target=dict(node=index['RightUpLeg'], path='rotation')), dict(sampler=3, target=dict(node=index['Spine'], path='rotation'))]),
        dict(name='Nod', samplers=[dict(input=nod_times, output=nod, interpolation='CUBICSPLINE')], channels=[dict(sampler=0, target=dict(node=index['Head'], path='rotation'))]),
        # The target model has its own Idle, which hides this one.
        dict(name='Idle', samplers=[dict(input=times, output=walk_spine, interpolation='LINEAR')], channels=[dict(sampler=0, target=dict(node=index['Spine'], path='rotation'))]),
    ],
    scenes=[dict(nodes=[0])], scene=0)

# --- Unreal-style character ------------------------------------------------------------------------------------------
unreal = [
    ('root', None, (0, 0, 0)), ('pelvis', 'root', (0, 1.4, 0)), ('spine_01', 'pelvis', (0, 1.5, 0)), ('spine_02', 'spine_01', (0, 1.65, 0)),
    ('neck_01', 'spine_02', (0, 1.85, 0)), ('head', 'neck_01', (0, 1.95, 0)),
    ('clavicle_l', 'spine_02', (0.05, 1.8, 0)), ('upperarm_l', 'clavicle_l', (0.22, 1.8, 0)), ('lowerarm_l', 'upperarm_l', (0.5, 1.8, 0)), ('hand_l', 'lowerarm_l', (0.78, 1.8, 0)),
    ('clavicle_r', 'spine_02', (-0.05, 1.8, 0)), ('upperarm_r', 'clavicle_r', (-0.22, 1.8, 0)), ('lowerarm_r', 'upperarm_r', (-0.5, 1.8, 0)), ('hand_r', 'lowerarm_r', (-0.78, 1.8, 0)),
    ('thigh_l', 'pelvis', (0.12, 1.35, 0)), ('calf_l', 'thigh_l', (0.12, 0.7, 0)), ('foot_l', 'calf_l', (0.12, 0.1, 0)), ('ball_l', 'foot_l', (0.12, 0.0, 0.15)),
    ('thigh_r', 'pelvis', (-0.12, 1.35, 0)), ('calf_r', 'thigh_r', (-0.12, 0.7, 0)), ('foot_r', 'calf_r', (-0.12, 0.1, 0)), ('ball_r', 'foot_r', (-0.12, 0.0, 0.15)),
]
target = Builder()
positions = {name: position for name, _, position in unreal}
nodes = []
index = {}
for name, parent, position in unreal:
    index[name] = len(nodes)
    offset = [a - b for a, b in zip(position, positions[parent])] if parent else list(position)
    nodes.append(dict(name=name, translation=offset, children=[]))
    if parent:
        nodes[index[parent]]['children'].append(len(nodes) - 1)
for marker, bone in (('ForearmMarker', 'lowerarm_l'), ('ShinMarker', 'calf_l'), ('ChestMarker', 'spine_02')):
    nodes.append(dict(name=marker, mesh=0, translation=[0, 0, 0.05]))
    nodes[index[bone]]['children'].append(len(nodes) - 1)
for node in nodes:
    if not node.get('children', True):
        del node['children']
idle_times = target.accessor([0, 1, 2], 1, min=[0], max=[2])
idle = target.accessor(axis_angle((1, 0, 0), 0) + axis_angle((1, 0, 0), 5) + axis_angle((1, 0, 0), 0), 4)
character = target.model(
    materials=material, meshes=[triangle(target, 0.08)], nodes=nodes,
    animations=[dict(name='Idle', samplers=[dict(input=idle_times, output=idle, interpolation='LINEAR')], channels=[dict(sampler=0, target=dict(node=index['spine_01'], path='rotation'))])],
    scenes=[dict(nodes=[0])], scene=0)

root = Path(__file__).resolve().parent
for name, model in (('MixamoRig.gltf', library), ('UnrealRig.gltf', character)):
    text = json.dumps(model, indent=2) + '\n'
    (root / name).write_text(text)
    feature = root.parent / 'FeatureTest/Assets/Models' / name
    feature.write_text(text)
    if name == 'MixamoRig.gltf':
        (root.parents[1] / 'Fuzz/Corpus/FuzzGltf/MixamoRig.gltf').write_bytes(b'\0' + text.encode())
