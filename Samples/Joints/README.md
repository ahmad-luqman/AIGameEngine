# Joints (Basalt sample)

A showcase of the `Joint` component, built only through the automation API (`build_scene.json`):

- **Door** — a hinge on a static post, swung open and shut by a position motor.
- **Piston** — a vertical slider with limits, driven along a sine wave by a position motor.
- **Chain** — eight links joined by point joints, hanging from the world.
- **Wrecking ball** — a distance joint (fixed-length cable) released into a tower of blocks welded together
  with breakable fixed joints.

`Assets/Scripts/Director.lua` sets the motor targets every frame with `SetComponent` (cheap: the live joint
is updated in place), draws the cables and shows the HUD; `Weld.lua` counts `OnJointBreak` callbacks.

Play: `build/bin/BasaltRuntime --project Samples/Joints` (on macOS with Homebrew Vulkan, prefix with
`DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib` to get the validation layer).
Rebuild the scene: `build/bin/basalt --project Samples/Joints batch Samples/Joints/build_scene.json`.
Checks: `test_game.json` (run by ctest as `SampleJoints`).
