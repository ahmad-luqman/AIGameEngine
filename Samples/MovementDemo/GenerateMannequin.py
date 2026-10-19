"""Rebuild Assets/Models/Mannequin.gltf: a boxy mannequin with human proportions that faces +Z, left side on +X,
arms down at the sides. Each box is skinned rigidly to one joint. Clips: Walk (a 1 s cycle covering about 1.5 m),
Wave (the right arm, for a masked layer) and Breathe (the chest, for an additive layer; frame 0 is the reference).
"""
import math, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from skinned_gltf import SkinnedModel, X, Z, cycle, rx

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
model = SkinnedModel(joints)
at, limb, clip = model.at, model.limb, model.clip
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

# Walk: 8 keys over 1 s (+ the loop key). Negative X rotation swings a leg forward (the rig faces +Z).
legL, legR = cycle([-25, -12, 2, 15, 25, 12, -5, -18])
kneeL, kneeR = cycle([5, 3, 8, 20, 55, 50, 20, 8])
armL, armR = cycle([18, 10, 0, -10, -18, -10, 0, 10])
elbow = rx(-12, -10, -8, -10, -12, -10, -8, -10, -12)
hips = [(0, 0.95 + 0.025 * math.cos(4 * math.pi * i / 8 + math.pi), 0) for i in range(9)]
twist = [((0, 1, 0), 6 * math.cos(2 * math.pi * i / 8)) for i in range(9)]
clip('Walk', 1.0, [('LeftUpperLeg', 'rotation', legL), ('LeftLowerLeg', 'rotation', kneeL),
                   ('RightUpperLeg', 'rotation', legR), ('RightLowerLeg', 'rotation', kneeR),
                   ('LeftUpperArm', 'rotation', armL), ('RightUpperArm', 'rotation', armR),
                   ('LeftForeArm', 'rotation', elbow), ('RightForeArm', 'rotation', elbow),
                   ('Hips', 'translation', hips), ('Spine', 'rotation', twist)])
# A whole-body wave: the right arm up beside the head with the forearm waving, standing with the legs spread and
# leaning to the left. The sample masks it to the right arm, so the legs keep walking: unmasked, they would stop
# spread apart.
clip('Wave', 1.0, [('RightUpperArm', 'rotation', [(Z, -160)] * 5),
                   ('RightForeArm', 'rotation', [(Z, -25), (Z, 5), (Z, 30), (Z, 5), (Z, -25)]),
                   ('LeftUpperLeg', 'rotation', [(Z, 22)] * 5), ('RightUpperLeg', 'rotation', [(Z, -22)] * 5),
                   ('LeftLowerLeg', 'rotation', [(X, 0)] * 5), ('RightLowerLeg', 'rotation', [(X, 0)] * 5),
                   ('Spine', 'rotation', [(Z, 10)] * 5)])
# Additive breathing: the chest rises and settles (frame 0 is the reference).
clip('Breathe', 3.0, [('Chest', 'rotation', rx(0, -3, -5, -3, 0)), ('Neck', 'rotation', rx(0, 2, 3, 2, 0))])

model.write(Path(__file__).resolve().parent / 'Assets/Models/Mannequin.gltf', 'Basalt MovementDemo sample', [('Mannequin', [0.92, 0.55, 0.22, 1])])
