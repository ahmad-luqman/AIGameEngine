"""Rebuild Assets/Models/Mannequin.gltf: a boxy mannequin with human proportions that faces +Z, left side on +X,
arms down at the sides. Each box is skinned rigidly to one joint. Clips: Walk (a 1 s cycle covering about 1.5 m),
Wave (the right arm, for a masked layer) and Breathe (the chest, for an additive layer; frame 0 is the reference).
"""
import base64, json, math, struct
from pathlib import Path

blob = bytearray(); views = []; accessors = []

def accessor(values, width, kind='f', **extra):
    while len(blob) % 4:
        blob.append(0)
    raw = struct.pack('<' + kind * len(values), *values)
    views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(raw)))
    blob.extend(raw)
    accessors.append(dict(bufferView=len(views) - 1, componentType={'f': 5126, 'H': 5123, 'B': 5121}[kind],
                          count=len(values) // width, type={1: 'SCALAR', 3: 'VEC3', 4: 'VEC4', 16: 'MAT4'}[width], **extra))
    return len(accessors) - 1

def quat(axis, degrees):
    h = math.radians(degrees) / 2
    s = math.sin(h)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h)]

joints = [
    ('Hips', None, (0, 0.95, 0)),
    ('Spine', 'Hips', (0, 0.15, 0)),
    ('Chest', 'Spine', (0, 0.2, 0)),
    ('Neck', 'Chest', (0, 0.22, 0)),
    ('Head', 'Neck', (0, 0.08, 0)),
    ('LeftUpperArm', 'Chest', (0.21, 0.18, 0)),
    ('LeftForeArm', 'LeftUpperArm', (0, -0.28, 0)),
    ('LeftHand', 'LeftForeArm', (0, -0.25, 0.01)),
    ('RightUpperArm', 'Chest', (-0.21, 0.18, 0)),
    ('RightForeArm', 'RightUpperArm', (0, -0.28, 0)),
    ('RightHand', 'RightForeArm', (0, -0.25, 0.01)),
    ('LeftUpperLeg', 'Hips', (0.1, -0.05, 0)),
    ('LeftLowerLeg', 'LeftUpperLeg', (0, -0.42, 0.015)),
    ('LeftFoot', 'LeftLowerLeg', (0, -0.42, -0.015)),
    ('RightUpperLeg', 'Hips', (-0.1, -0.05, 0)),
    ('RightLowerLeg', 'RightUpperLeg', (0, -0.42, 0.015)),
    ('RightFoot', 'RightLowerLeg', (0, -0.42, -0.015)),
]
index = {name: i for i, (name, _, _) in enumerate(joints)}
world = []
for name, parent, offset in joints:
    base = world[index[parent]] if parent else (0, 0, 0)
    world.append(tuple(b + o for b, o in zip(base, offset)))

positions, normals, skin, weights, indices = [], [], [], [], []

def box(joint, center, half):
    """An axis-aligned box (bind pose) rigidly skinned to `joint`."""
    j = index[joint]
    cx, cy, cz = center
    hx, hy, hz = half
    faces = [((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
             ((0, 1, 0), (0, 0, 1), (1, 0, 0)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
             ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (0, 1, 0), (1, 0, 0))]
    for n, u, v in faces:
        first = len(positions) // 3
        for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
            p = [cx + hx * (n[0] + su * u[0] + sv * v[0]), cy + hy * (n[1] + su * u[1] + sv * v[1]), cz + hz * (n[2] + su * u[2] + sv * v[2])]
            positions.extend(p); normals.extend(n); skin.extend([j, 0, 0, 0]); weights.extend([1, 0, 0, 0])
        # Counter-clockwise seen from outside: u x v = n for every face above.
        indices.extend([first, first + 1, first + 2, first, first + 2, first + 3])

def limb(joint, child, thickness):
    a, b = world[index[joint]], world[index[child]]
    center = tuple((x + y) / 2 for x, y in zip(a, b))
    half = tuple(max(abs(x - y) / 2, thickness) for x, y in zip(a, b))
    box(joint, center, half)

def at(joint, offset, half):
    box(joint, tuple(w + o for w, o in zip(world[index[joint]], offset)), half)

at('Hips', (0, 0.02, 0), (0.17, 0.09, 0.11))
at('Spine', (0, 0.1, 0), (0.15, 0.1, 0.1))
at('Chest', (0, 0.12, 0), (0.2, 0.13, 0.12))
at('Neck', (0, 0.04, 0), (0.05, 0.05, 0.05))
at('Head', (0, 0.12, 0.01), (0.1, 0.12, 0.11))
at('Head', (0, 0.12, 0.12), (0.03, 0.02, 0.02))  # nose: shows where the head looks
for side in ('Left', 'Right'):
    limb(side + 'UpperArm', side + 'ForeArm', 0.05)
    limb(side + 'ForeArm', side + 'Hand', 0.045)
    at(side + 'Hand', (0, -0.06, 0), (0.04, 0.06, 0.025))
    limb(side + 'UpperLeg', side + 'LowerLeg', 0.075)
    limb(side + 'LowerLeg', side + 'Foot', 0.06)
    at(side + 'Foot', (0, -0.015, 0.06), (0.055, 0.035, 0.13))

xs, ys, zs = positions[0::3], positions[1::3], positions[2::3]
P = accessor(positions, 3, min=[min(xs), min(ys), min(zs)], max=[max(xs), max(ys), max(zs)])
N = accessor(normals, 3); J = accessor(skin, 4, 'B'); W = accessor(weights, 4); I = accessor(indices, 1, 'H')
binds = []
for x, y, z in world:
    binds += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1]
B = accessor(binds, 16)

X, Z = (1, 0, 0), (0, 0, 1)

def clip(name, duration, channels):
    """channels: (joint, path, [values per key]) with keys evenly spread over the duration (first == last loops)."""
    samplers, targets = [], []
    for joint, path, keys in channels:
        times = [duration * i / (len(keys) - 1) for i in range(len(keys))]
        t = accessor(times, 1, min=[0], max=[duration])
        if path == 'rotation':
            out = accessor([v for axis, deg in keys for v in quat(axis, deg)], 4)
        else:
            out = accessor([v for key in keys for v in key], 3)
        samplers.append(dict(input=t, output=out, interpolation='LINEAR'))
        targets.append(dict(sampler=len(samplers) - 1, target=dict(node=index[joint] + 1, path=path)))
    return dict(name=name, samplers=samplers, channels=targets)

def rx(*degrees):
    return [(X, d) for d in degrees]

# Walk: 8 keys over 1 s (+ the loop key). Negative X rotation swings a leg forward (the rig faces +Z).
legL = rx(-25, -12, 2, 15, 25, 12, -5, -18, -25)
kneeL = rx(5, 3, 8, 20, 55, 50, 20, 8, 5)
legR = legL[4:] + legL[1:5]
kneeR = kneeL[4:] + kneeL[1:5]
armL = rx(18, 10, 0, -10, -18, -10, 0, 10, 18)
armR = armL[4:] + armL[1:5]
elbow = rx(-12, -10, -8, -10, -12, -10, -8, -10, -12)
hips = [(0, 0.95 + 0.025 * math.cos(4 * math.pi * i / 8 + math.pi), 0) for i in range(9)]
twist = [((0, 1, 0), 6 * math.cos(2 * math.pi * i / 8)) for i in range(9)]
animations = [
    clip('Walk', 1.0, [('LeftUpperLeg', 'rotation', legL), ('LeftLowerLeg', 'rotation', kneeL),
                       ('RightUpperLeg', 'rotation', legR), ('RightLowerLeg', 'rotation', kneeR),
                       ('LeftUpperArm', 'rotation', armL), ('RightUpperArm', 'rotation', armR),
                       ('LeftForeArm', 'rotation', elbow), ('RightForeArm', 'rotation', elbow),
                       ('Hips', 'translation', hips), ('Spine', 'rotation', twist)]),
    # The right arm up beside the head, the forearm waving.
    clip('Wave', 1.0, [('RightUpperArm', 'rotation', [(Z, -160)] * 5),
                       ('RightForeArm', 'rotation', [(Z, -25), (Z, 5), (Z, 30), (Z, 5), (Z, -25)])]),
    # Additive breathing: the chest rises and settles (frame 0 is the reference).
    clip('Breathe', 3.0, [('Chest', 'rotation', rx(0, -3, -5, -3, 0)), ('Neck', 'rotation', rx(0, 2, 3, 2, 0))]),
]

nodes = [dict(name='Mesh', mesh=0, skin=0)]
for name, parent, offset in joints:
    node = dict(name=name, translation=list(offset))
    children = [index[c] + 1 for c, p, _ in joints if p == name]
    if children:
        node['children'] = children
    nodes.append(node)
model = dict(asset=dict(version='2.0', generator='Basalt MovementDemo sample'),
             buffers=[dict(byteLength=len(blob), uri='data:application/octet-stream;base64,' + base64.b64encode(blob).decode())],
             bufferViews=views, accessors=accessors,
             materials=[dict(name='Mannequin', pbrMetallicRoughness=dict(baseColorFactor=[0.92, 0.55, 0.22, 1], metallicFactor=0.1, roughnessFactor=0.45))],
             meshes=[dict(name='Body', primitives=[dict(attributes=dict(POSITION=P, NORMAL=N, JOINTS_0=J, WEIGHTS_0=W), indices=I, material=0)])],
             nodes=nodes, skins=[dict(joints=[i + 1 for i in range(len(joints))], inverseBindMatrices=B, skeleton=1)],
             animations=animations, scenes=[dict(nodes=[0, 1])], scene=0)
out = Path(__file__).resolve().parent / 'Assets/Models/Mannequin.gltf'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps(model) + '\n')
print('wrote', out)
