# Joints (Basalt sample)

A showcase of the `Joint` component, built only through the automation API (`build_scene.json`):

- **Door** — a hinge on a static post, swung open and shut by a position motor.
- **Piston** — a vertical slider with limits, driven along a sine wave by a position motor.
- **Chain** — eight links joined by point joints, hanging from the world.
- **Wrecking ball** — a distance joint (fixed-length cable) released into a tower of blocks welded together
  with breakable fixed joints.
- **Rope ladder** — three rungs, each held by two distance joints. A body has one `Joint` of its own, so
  each rope is a child entity whose joint names the rung as its `BodyEntity`.
- **Ragdoll arm** — a six-DOF shoulder (twist and an elliptical swing cone), a hinged elbow whose stops
  are softened by a limit spring, and a cone joint at the wrist.
- **Bungee** — a distance joint without limits but with a `LimitSpringFrequency`, so the cord stretches
  and springs back.

`Assets/Scripts/Director.lua` sets the motor targets every frame with `SetComponent` (cheap: the live joint
is updated in place), knocks the ladder, arm and bungee, draws the cables and shows the HUD; `Weld.lua` counts `OnJointBreak` callbacks.

Play: `build/bin/BasaltRuntime --project Samples/Joints` (on macOS with Homebrew Vulkan, prefix with
`DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib` to get the validation layer).
Rebuild the scene: `build/bin/basalt --project Samples/Joints batch Samples/Joints/build_scene.json`.
Checks: `test_game.json` (run by ctest as `SampleJoints`).
