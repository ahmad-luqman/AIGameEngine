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

macOS with Homebrew: Vulkan validation layers load only with `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib`.

## Use

```bash
build/bin/BasaltEditor --project Samples/Tetris          # editor (automation server on 127.0.0.1:7420)
build/bin/BasaltRuntime --project Samples/Tetris         # play a project
build/bin/BasaltRuntime --project P --frames 60 --offscreen --screenshot shot.png
build/bin/basalt help                                    # automation API from the command line
build/bin/basalt --project Samples/Tetris project.export '{"output": "Dist/Tetris"}'
```

## Documentation

- [AGENTS.md](AGENTS.md) — architecture, conventions and development workflow
- [Docs/ScriptingAPI.md](Docs/ScriptingAPI.md) — Lua API
- [Docs/AutomationCommands.md](Docs/AutomationCommands.md), [Docs/Components.md](Docs/Components.md) — generated references
- [ROADMAP.md](ROADMAP.md) — planned work and known gaps
- [Docs/TestedHardware.md](Docs/TestedHardware.md) — real-GPU smoke test procedure and results
- [.claude/skills/basalt-make-game](.claude/skills/basalt-make-game/SKILL.md) — building a game as an AI agent
- [Samples/Tetris](Samples/Tetris) — a game built entirely through the automation API

## License

MIT — see [LICENSE](LICENSE). Third-party libraries fetched at build time keep their own licenses.
