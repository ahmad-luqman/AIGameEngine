# Physics determinism test

`ctest -R PhysicsDeterminism` plays `Assets/Scenes/PhysicsPile.bscene` (towers of boxes, falling spheres
and capsules, a convex hull, a hinge chain, a powered six-DOF limb, a slider piston and a character) and
checks the state hash after loading and after 1, 120 and 600 frames (with eight bodies rebuilt in one
step at frame 120, which must happen in the same order everywhere). The expected hashes are the same on
every platform and build type; a mismatch fails with the actual hash.

The scene avoids what is not portable: no scripts, no rotations in the file (Euler angles go through each
platform's `sin`/`cos`), and only colliders whose shapes need no trig (box, sphere, capsule, `builtin://Cube`).

If a deliberate physics change alters the result, run the batch and copy the new hashes from the errors:

```bash
./build/bin/basalt --project Tests/Data/Determinism --no-save batch Tests/Data/Determinism/Determinism.batch.json
```

A hash after loading that differs means the scene loads differently (file format, transforms); later ones
mean the simulation diverged.
