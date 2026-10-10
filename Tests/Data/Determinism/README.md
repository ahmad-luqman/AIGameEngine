# Physics determinism test

`ctest -R PhysicsDeterminism` plays `Assets/Scenes/PhysicsPile.bscene` through `Determinism.batch.json` and
checks the state hash at each `expectHash` checkpoint (the request `id`s name them). The expected hashes are
the same on every platform and CI configuration; a mismatch fails with the actual hash.

The scene is busy on purpose: towers of boxes, falling spheres and capsules, a convex hull, a hinge chain,
a velocity-driven six-DOF limb held back by its own torque cap, a slider piston, kinematic pushers, a dynamic body
parented to another, a sled whose runner collider has its own friction, and a walking character that pushes
another one. Between checkpoints the batch teleports and moves bodies, rebuilds colliders, rescales a body,
destroys bodies and spawns new ones (whose physics is added in a different order than they were created, so
the rebuild order shows in the hash), changes a joint's axis and a character's collider.

It avoids what is not portable: no scripts, no rotations in the file or the batch (Euler angles go through
each platform's `sin`/`cos`), no six-DOF Position motors on rotation axes (their target is an Euler
orientation), and only colliders whose shapes need no trig (box, sphere, capsule, `builtin://Cube`).

If a deliberate change to physics, scene serialization or the component registry alters the result, run
the batch and copy the new hashes from the errors (the batch keeps going after a mismatch):

```bash
./build/bin/basalt --project Tests/Data/Determinism --no-save batch Tests/Data/Determinism/Determinism.batch.json
```

A hash after loading that differs means the scene loads differently (file format, transforms); later ones
mean the simulation diverged.
