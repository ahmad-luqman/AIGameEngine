# Basalt

A simple, production-grade 3D game engine for Windows, macOS and Linux: C++20, Vulkan through
[nvrhi](https://github.com/NVIDIA-RTX/NVRHI), PBR with image-based lighting, soft shadows and SSAO, Jolt
physics, Lua scripting, an editor — and a complete JSON automation API so AI agents can build games
without touching the GUI.

## Build

Requirements: CMake ≥ 3.25, Ninja, a C++20 compiler, `glslc` (Vulkan SDK or shaderc) and a Vulkan 1.3
driver (MoltenVK on macOS). Dependencies are fetched by CMake at pinned versions.

```bash
scripts/build.sh            # Debug build into build/ (Release / Dist: scripts/build.sh Release)
scripts/test.sh             # unit tests, feature test, sample game, GPU render test
```

macOS with Homebrew: development builds load Homebrew's Vulkan validation layer by themselves (no environment setup).

## Use

```bash
build/bin/BasaltEditor --project Samples/Tetris          # editor (automation server on 127.0.0.1:7420)
build/bin/BasaltRuntime --project Samples/Tetris         # play a project
build/bin/BasaltRuntime --project P --frames 60 --offscreen --screenshot shot.png
build/bin/basalt help                                    # automation API from the command line
build/bin/basalt --project Samples/Tetris project.validate     # missing assets, broken scenes/scripts
build/bin/basalt --project Samples/Tetris project.export '{"output": "Dist/Tetris", "smokeTestFrames": 120}'
build/bin/basalt --project Samples/Tetris project.export '{"output": "Dist/Tetris", "package": true}'  # .app / .exe / AppImage
```

## Documentation

- [AGENTS.md](AGENTS.md) — architecture, conventions and development workflow
- [Docs/Rendering.md](Docs/Rendering.md) — FSR 1 settings, render-area handling and GPU tests
- [Docs/ScriptingAPI.md](Docs/ScriptingAPI.md) — Lua API
- [Docs/Shipping.md](Docs/Shipping.md) — icon, version and company, platform packages, signing, the release pipeline
- [Docs/MCP.md](Docs/MCP.md) — schema-driven agent tools over stdio or the running editor
- [Docs/AutomationCommands.md](Docs/AutomationCommands.md), [Docs/Components.md](Docs/Components.md) — generated references
- [ROADMAP.md](ROADMAP.md) — planned work and known gaps
- [Docs/TestedHardware.md](Docs/TestedHardware.md) — real-GPU smoke test procedure and results
- [.claude/skills/basalt-make-game](.claude/skills/basalt-make-game/SKILL.md) — building a game as an AI agent
- [Samples/Tetris](Samples/Tetris) — a game built entirely through the automation API
- [Samples/MarbleRun](Samples/MarbleRun) — a physics showcase (joints, triggers, particles, spatial audio, UI) whose
  test is written as `assert.*`, `events.poll` and `scene.diff` checks
- [Samples/Breakout](Samples/Breakout) — physics, game UI and input actions (keys, gamepad, mouse, rebinding from Lua)
- [Samples/FirstPerson](Samples/FirstPerson) — mouse look with a locked cursor, a character controller, raycast
  shooting and audio effects (low-pass occlusion, reverb zones, Doppler)
- [Samples/Joints](Samples/Joints) — every joint type
- [Samples/MovementDemo](Samples/MovementDemo) — a walking mannequin with foot placement, look-at and reach IK, a
  masked wave layer and additive breathing
- [Samples/Platformer](Samples/Platformer) — a 3D platformer: a character controller driven through an animation state
  machine with root motion, foot placement, spring-bone hair and cape, and a moving platform

## License

MIT — see [LICENSE](LICENSE). Third-party libraries fetched at build time keep their own licenses.
