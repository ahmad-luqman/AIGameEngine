"""Builds the boxy, rigidly skinned glTF characters of the samples (Samples/MovementDemo/GenerateMannequin.py,
Samples/Platformer/GenerateHero.py): a joint hierarchy with identity rest rotations, boxes each skinned to one joint,
and clips keyed evenly over their duration. The rig faces +Z with its left side on +X.
"""
import base64, json, math, struct
from pathlib import Path

X, Y, Z = (1, 0, 0), (0, 1, 0), (0, 0, 1)


def quat(axis, degrees):
    h = math.radians(degrees) / 2
    s = math.sin(h)
    return [axis[0] * s, axis[1] * s, axis[2] * s, math.cos(h)]


def rx(*degrees):
    """Rotation keys about X; negative X rotation swings a leg forward."""
    return [(X, d) for d in degrees]


def rz(*degrees):
    """Rotation keys about Z; positive Z rotation raises a left arm sideways."""
    return [(Z, d) for d in degrees]


def loop(keys):
    """Closes a list of per-key values into a loop (first == last)."""
    return keys + keys[:1]


def shifted(keys):
    """The same cycle half a period later (the other leg or arm), closed into a loop; `keys` has no loop key."""
    half = len(keys) // 2
    return loop(keys[half:] + keys[:half])


def cycle(degrees):
    """X rotation keys of a limb over a cycle and of its opposite half a period later, both closed into loops."""
    return rx(*loop(degrees)), rx(*shifted(degrees))


def pairs(*names):
    """Consecutive pairs: each joint with its child along a chain."""
    return list(zip(names, names[1:]))


class SkinnedModel:
    def __init__(self, joints):
        """joints: (name, parent or None, offset from the parent) in parent-before-child order."""
        self.joints = joints
        self.index = {name: i for i, (name, _, _) in enumerate(joints)}
        self.world = []
        for name, parent, offset in joints:
            base = self.world[self.index[parent]] if parent else (0, 0, 0)
            self.world.append(tuple(b + o for b, o in zip(base, offset)))
        self.positions, self.normals, self.skin, self.weights = [], [], [], []
        # Triangle indices per material.
        self.indices = {}
        self.clips = []
        self.blob = bytearray()
        self.views = []
        self.accessors = []

    def accessor(self, values, width, kind='f', **extra):
        while len(self.blob) % 4:
            self.blob.append(0)
        raw = struct.pack('<' + kind * len(values), *values)
        self.views.append(dict(buffer=0, byteOffset=len(self.blob), byteLength=len(raw)))
        self.blob.extend(raw)
        self.accessors.append(dict(bufferView=len(self.views) - 1, componentType={'f': 5126, 'H': 5123, 'B': 5121}[kind],
                                   count=len(values) // width, type={1: 'SCALAR', 3: 'VEC3', 4: 'VEC4', 16: 'MAT4'}[width], **extra))
        return len(self.accessors) - 1

    def box(self, joint, center, half, material=0):
        """An axis-aligned box (bind pose) rigidly skinned to `joint`, drawn with material number `material`."""
        j = self.index[joint]
        cx, cy, cz = center
        hx, hy, hz = half
        faces = [((1, 0, 0), (0, 1, 0), (0, 0, 1)), ((-1, 0, 0), (0, 0, 1), (0, 1, 0)),
                 ((0, 1, 0), (0, 0, 1), (1, 0, 0)), ((0, -1, 0), (1, 0, 0), (0, 0, 1)),
                 ((0, 0, 1), (1, 0, 0), (0, 1, 0)), ((0, 0, -1), (0, 1, 0), (1, 0, 0))]
        for n, u, v in faces:
            first = len(self.positions) // 3
            for su, sv in ((-1, -1), (1, -1), (1, 1), (-1, 1)):
                p = [cx + hx * (n[0] + su * u[0] + sv * v[0]), cy + hy * (n[1] + su * u[1] + sv * v[1]), cz + hz * (n[2] + su * u[2] + sv * v[2])]
                self.positions.extend(p); self.normals.extend(n); self.skin.extend([j, 0, 0, 0]); self.weights.extend([1, 0, 0, 0])
            # Counter-clockwise seen from outside: u x v = n for every face above.
            self.indices.setdefault(material, []).extend([first, first + 1, first + 2, first, first + 2, first + 3])

    def position(self, joint):
        """Where `joint` is in the bind pose."""
        return self.world[self.index[joint]]

    def limb(self, joint, child, thickness, material=0):
        """A box from `joint` to `child`, at least `thickness` thick on each side."""
        a, b = self.position(joint), self.position(child)
        center = tuple((x + y) / 2 for x, y in zip(a, b))
        half = tuple(max(abs(x - y) / 2, thickness) for x, y in zip(a, b))
        self.box(joint, center, half, material)

    def at(self, joint, offset, half, material=0):
        """A box at `offset` from `joint`."""
        self.box(joint, tuple(w + o for w, o in zip(self.position(joint), offset)), half, material)

    def clip(self, name, duration, channels):
        """channels: (joint, 'rotation' with [(axis, degrees)] keys | 'translation' with [xyz] keys), keys evenly spread
        over the duration (first == last loops)."""
        self.clips.append((name, duration, channels))

    def write(self, path, generator, materials):
        """materials: (name, base color RGBA) per material number."""
        xs, ys, zs = self.positions[0::3], self.positions[1::3], self.positions[2::3]
        P = self.accessor(self.positions, 3, min=[min(xs), min(ys), min(zs)], max=[max(xs), max(ys), max(zs)])
        N = self.accessor(self.normals, 3); J = self.accessor(self.skin, 4, 'B'); W = self.accessor(self.weights, 4)
        I = {material: self.accessor(self.indices[material], 1, 'H') for material in sorted(self.indices)}
        binds = []
        for x, y, z in self.world:
            binds += [1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, -x, -y, -z, 1]
        B = self.accessor(binds, 16)
        animations = []
        for name, duration, channels in self.clips:
            samplers, targets = [], []
            for joint, kind, keys in channels:
                times = [duration * i / (len(keys) - 1) for i in range(len(keys))]
                t = self.accessor(times, 1, min=[0], max=[duration])
                if kind == 'rotation':
                    out = self.accessor([v for axis, deg in keys for v in quat(axis, deg)], 4)
                else:
                    out = self.accessor([v for key in keys for v in key], 3)
                samplers.append(dict(input=t, output=out, interpolation='LINEAR'))
                targets.append(dict(sampler=len(samplers) - 1, target=dict(node=self.index[joint] + 1, path=kind)))
            animations.append(dict(name=name, samplers=samplers, channels=targets))

        nodes = [dict(name='Mesh', mesh=0, skin=0)]
        for name, parent, offset in self.joints:
            node = dict(name=name, translation=list(offset))
            children = [self.index[c] + 1 for c, p, _ in self.joints if p == name]
            if children:
                node['children'] = children
            nodes.append(node)
        model = dict(asset=dict(version='2.0', generator=generator),
                     buffers=[dict(byteLength=len(self.blob), uri='data:application/octet-stream;base64,' + base64.b64encode(self.blob).decode())],
                     bufferViews=self.views, accessors=self.accessors,
                     materials=[dict(name=name, pbrMetallicRoughness=dict(baseColorFactor=color, metallicFactor=0.1, roughnessFactor=0.45)) for name, color in materials],
                     meshes=[dict(name='Body', primitives=[dict(attributes=dict(POSITION=P, NORMAL=N, JOINTS_0=J, WEIGHTS_0=W), indices=I[material], material=material) for material in sorted(I)])],
                     nodes=nodes, skins=[dict(joints=[i + 1 for i in range(len(self.joints))], inverseBindMatrices=B, skeleton=1)],
                     animations=animations, scenes=[dict(nodes=[0, 1])], scene=0)
        path = Path(path)
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(json.dumps(model) + '\n')
        print('wrote', path)
