# Physics determinism test

`ctest -R PhysicsDeterminism` plays `Assets/Scenes/PhysicsPile.bscene` through `Determinism.batch.json` and
checks the state hash at each `expectHash` checkpoint (the request `id`s name them). The expected hashes are
the same on every platform and CI configuration; a mismatch fails with the actual hash.

The scene is busy on purpose: towers of boxes, falling spheres and capsules, a convex hull, a hinge chain,
a velocity-driven six-DOF limb held back by its own torque cap, a slider piston, kinematic pushers, a dynamic body
parented to another, a sled whose runner collider has its own friction, and a walking character that pushes
another one. A passive two-joint ragdoll falls into the pile; the hashes include its physical joints directly,
and bone-attachment markers additionally check the displayed pose (the Animator stays on its rest pose). Between checkpoints the batch teleports and moves bodies, rebuilds colliders, rescales a body,
destroys bodies and spawns new ones (whose physics is added in a different order than they were created, so
the rebuild order shows in the hash), changes a joint's axis and a character's collider.

After the original checkpoints, a Lua fixture spawns a jointed compound-body chain inside a trigger.
Its script creations (`OnCreate`) and trigger enter/exit callbacks fold their order into a marker transform; trigger callbacks
spawn additional scripted, jointed bodies. Further checkpoints enable collisions, rebuild colliders,
remove connected bodies and move the trigger away while waking the survivors. Thus hashes check both
physics state and callback order, with assertions that the callbacks actually ran.

Final checkpoints freeze the corpse, recover halfway and finish the blend. Ragdoll decomposition uses basic
arithmetic/sqrt and recovery uses Jolt's portable quaternion interpolation, so markers and blend weights agree
across platforms as well as the authoritative Jolt pose.

It avoids what is not portable: no platform-dependent Lua math, no rotations in the file or the batch
(Euler angles go through each platform's `sin`/`cos`), no six-DOF Position motors on rotation axes (their target is an Euler
orientation), and only colliders whose shapes need no trig (box, sphere, capsule, `builtin://Cube`).

If a deliberate change to physics, the runtime UUID stream, scene serialization or the component registry alters the result, run
the batch and copy the new hashes from the errors (the batch keeps going after a mismatch):

```bash
./build/bin/basalt --project Tests/Data/Determinism --no-save batch Tests/Data/Determinism/Determinism.batch.json
```

A hash after loading that differs means the scene loads differently (transforms, settings); later ones
mean the simulation diverged. The file format (`Format`, `Version`) is not hashed, so a format version bump
alone never moves the hashes; the physics step is hashed as the value the simulation uses (scene override or
project).
