"""Rebuild Assets/Models/Hero.gltf: the platformer's boxy hero (the MovementDemo mannequin's proportions) with a
ponytail and a two-strand cape for spring bones. It faces +Z, left side on +X. `Root` sits on the ground under the
hips and carries the root motion of the Walk (1.5 m/s) and Run (4 m/s) cycles. Clips: Idle, Walk, Run (loops), Jump
and Land (play once) and Fall (loops).
"""
import math, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from skinned_gltf import SkinnedModel, X, Y, Z, rx

joints = [
    ('Root', None, (0, 0, 0)),
    ('Hips', 'Root', (0, 0.95, 0)),
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
    # A ponytail from the back of the head; HairTip only gives the last segment a direction to swing.
    ('Hair1', 'Head', (0, 0.2, -0.1)),
    ('Hair2', 'Hair1', (0, -0.04, -0.09)),
    ('Hair3', 'Hair2', (0, -0.1, -0.03)),
    ('HairTip', 'Hair3', (0, -0.1, 0)),
    # Two cape strands hanging from the shoulder blades.
    ('CapeL1', 'Chest', (0.09, 0.22, -0.15)),
    ('CapeL2', 'CapeL1', (0, -0.2, -0.02)),
    ('CapeL3', 'CapeL2', (0, -0.2, -0.01)),
    ('CapeLTip', 'CapeL3', (0, -0.2, 0)),
    ('CapeR1', 'Chest', (-0.09, 0.22, -0.15)),
    ('CapeR2', 'CapeR1', (0, -0.2, -0.02)),
    ('CapeR3', 'CapeR2', (0, -0.2, -0.01)),
    ('CapeRTip', 'CapeR3', (0, -0.2, 0)),
]
BODY, HAIR, CAPE = 0, 1, 2
model = SkinnedModel(joints)
at, limb, box = model.at, model.limb, model.box

at('Hips', (0, 0.02, 0), (0.17, 0.09, 0.11))
at('Spine', (0, 0.1, 0), (0.15, 0.1, 0.1))
at('Chest', (0, 0.12, 0), (0.2, 0.13, 0.12))
at('Neck', (0, 0.04, 0), (0.05, 0.05, 0.05))
at('Head', (0, 0.12, 0.01), (0.1, 0.12, 0.11))
at('Head', (0, 0.12, 0.12), (0.03, 0.02, 0.02))  # nose: shows where the head looks
at('Head', (0, 0.23, -0.01), (0.11, 0.03, 0.12), HAIR)  # hair cap
for side in ('Left', 'Right'):
    limb(side + 'UpperArm', side + 'ForeArm', 0.05)
    limb(side + 'ForeArm', side + 'Hand', 0.045)
    at(side + 'Hand', (0, -0.06, 0), (0.04, 0.06, 0.025))
    limb(side + 'UpperLeg', side + 'LowerLeg', 0.075)
    limb(side + 'LowerLeg', side + 'Foot', 0.06)
    at(side + 'Foot', (0, -0.015, 0.06), (0.055, 0.035, 0.13))
for joint, child in (('Hair1', 'Hair2'), ('Hair2', 'Hair3'), ('Hair3', 'HairTip')):
    limb(joint, child, 0.035, HAIR)
for strand in ('CapeL', 'CapeR'):
    for i in range(1, 4):
        joint, child = strand + str(i), strand + (str(i + 1) if i < 3 else 'Tip')
        a, b = model.world[model.index[joint]], model.world[model.index[child]]
        # A thin panel per segment, a little longer than the bone so bent segments still overlap.
        box(joint, tuple((x + y) / 2 for x, y in zip(a, b)), (0.085, abs(a[1] - b[1]) / 2 + 0.015, 0.012), CAPE)


def loop(keys):
    """Closes a list of per-key values into a loop (first == last)."""
    return keys + keys[:1]


def shifted(keys):
    """The same cycle half a period later (the other leg or arm); keys excludes the loop key."""
    half = len(keys) // 2
    return loop(keys[half:] + keys[:half])


def cycle(degrees):
    return rx(*loop(degrees)), rx(*shifted(degrees))


def bob(base, depth, keys):
    """Hips translation dipping twice per cycle (once per step)."""
    return [(0, base - depth * (0.5 + 0.5 * math.cos(4 * math.pi * i / keys)), 0) for i in range(keys + 1)]


def stride(length):
    """Root translation moving forward `length` over a cycle (root motion)."""
    return [(0, 0, 0), (0, 0, length)]


# Idle (2 s): breathing and a slight sway.
model.clip('Idle', 2.0, [('Chest', 'rotation', rx(0, -3, 0, 2, 0)), ('Spine', 'rotation', [(Z, d) for d in (0, 1.5, 0, -1.5, 0)]),
                         ('LeftUpperArm', 'rotation', [(Z, d) for d in (6, 9, 6, 4, 6)]),
                         ('RightUpperArm', 'rotation', [(Z, -d) for d in (6, 9, 6, 4, 6)]),
                         ('Hips', 'translation', [(0, 0.95, 0), (0, 0.94, 0), (0, 0.95, 0)])])

# Walk (1 s, 8 keys): the mannequin's walk. Negative X rotation swings a leg forward (the rig faces +Z).
legL, legR = cycle([-25, -12, 2, 15, 25, 12, -5, -18])
kneeL, kneeR = cycle([5, 3, 8, 20, 55, 50, 20, 8])
armL, armR = cycle([18, 10, 0, -10, -18, -10, 0, 10])
model.clip('Walk', 1.0, [('LeftUpperLeg', 'rotation', legL), ('LeftLowerLeg', 'rotation', kneeL),
                         ('RightUpperLeg', 'rotation', legR), ('RightLowerLeg', 'rotation', kneeR),
                         ('LeftUpperArm', 'rotation', armL), ('RightUpperArm', 'rotation', armR),
                         ('LeftForeArm', 'rotation', rx(*[-12] * 9)), ('RightForeArm', 'rotation', rx(*[-12] * 9)),
                         ('Hips', 'translation', bob(0.95, 0.025, 8)), ('Root', 'translation', stride(1.5))])

# Run (0.6 s, 8 keys): longer strides, high knees, pumping arms, leaning forward.
legL, legR = cycle([-50, -30, 0, 25, 35, 10, -25, -45])
kneeL, kneeR = cycle([30, 15, 20, 45, 100, 110, 80, 50])
armL, armR = cycle([40, 25, 0, -25, -40, -25, 0, 25])
model.clip('Run', 0.6, [('LeftUpperLeg', 'rotation', legL), ('LeftLowerLeg', 'rotation', kneeL),
                        ('RightUpperLeg', 'rotation', legR), ('RightLowerLeg', 'rotation', kneeR),
                        ('LeftUpperArm', 'rotation', armL), ('RightUpperArm', 'rotation', armR),
                        ('LeftForeArm', 'rotation', rx(*[-70] * 9)), ('RightForeArm', 'rotation', rx(*[-70] * 9)),
                        ('Spine', 'rotation', rx(*[12] * 9)), ('Hips', 'translation', bob(0.9, 0.05, 8)),
                        ('Root', 'translation', stride(2.4))])

# Jump (0.4 s, once): knees tuck up and the arms go up and out.
model.clip('Jump', 0.4, [('LeftUpperLeg', 'rotation', rx(0, -35, -60, -65, -65)), ('LeftLowerLeg', 'rotation', rx(0, 50, 95, 100, 100)),
                         ('RightUpperLeg', 'rotation', rx(0, -20, -35, -40, -40)), ('RightLowerLeg', 'rotation', rx(0, 30, 60, 70, 70)),
                         ('LeftUpperArm', 'rotation', [(Z, d) for d in (5, 60, 120, 140, 140)]),
                         ('RightUpperArm', 'rotation', [(Z, -d) for d in (5, 60, 120, 140, 140)]),
                         ('Spine', 'rotation', rx(0, -5, -8, -8, -8))])

# Fall (0.6 s, loops): legs reach for the ground, arms out and paddling.
model.clip('Fall', 0.6, [('LeftUpperLeg', 'rotation', rx(-25, -15, -25)), ('LeftLowerLeg', 'rotation', rx(35, 20, 35)),
                         ('RightUpperLeg', 'rotation', rx(-5, -15, -5)), ('RightLowerLeg', 'rotation', rx(15, 30, 15)),
                         ('LeftUpperArm', 'rotation', [(Z, d) for d in (95, 115, 95)]),
                         ('RightUpperArm', 'rotation', [(Z, -d) for d in (115, 95, 115)]),
                         ('Spine', 'rotation', rx(5, 5, 5))])

# Land (0.35 s, once): the hips drop into a crouch and rise again.
model.clip('Land', 0.35, [('Hips', 'translation', [(0, 0.95, 0), (0, 0.8, 0), (0, 0.86, 0), (0, 0.95, 0)]),
                          ('LeftUpperLeg', 'rotation', rx(-10, -45, -30, 0)), ('LeftLowerLeg', 'rotation', rx(10, 80, 55, 0)),
                          ('RightUpperLeg', 'rotation', rx(-10, -45, -30, 0)), ('RightLowerLeg', 'rotation', rx(10, 80, 55, 0)),
                          ('Spine', 'rotation', rx(0, 20, 12, 0)),
                          ('LeftUpperArm', 'rotation', [(Z, d) for d in (40, 25, 15, 6)]),
                          ('RightUpperArm', 'rotation', [(Z, -d) for d in (40, 25, 15, 6)])])

model.write(Path(__file__).resolve().parent / 'Assets/Models/Hero.gltf', 'Basalt Platformer sample',
            [('Hero', [0.25, 0.55, 0.95, 1]), ('Hair', [0.35, 0.18, 0.08, 1]), ('Cape', [0.85, 0.12, 0.15, 1])])
