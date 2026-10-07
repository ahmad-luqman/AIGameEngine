# AGENTS.md — Basalt Engine Development Guide

Basalt is a production-grade 3D game engine (C++20, CMake, Vulkan via nvrhi) for Windows, macOS and
Linux. This file is the source of truth for how to work in this repository. Read it fully before
changing code. Task-specific procedures live in `.claude/skills/*/SKILL.md`.

## Repository layout

| Path | Contents |
|------|----------|
| `Basalt/Source/Basalt/` | Engine static library (`Basalt`). One folder per module (see below). |
| `Basalt/Shaders/` | GLSL shaders, compiled to SPIR-V at build time and **embedded** in the binary. `Include/*.glsl` are shared headers. |
| `Basalt-Editor/` | Editor executable (`BasaltEditor`): ImGui UI, gizmos, automation server. Never shipped. |
| `Basalt-Runtime/` | Runtime executable (`BasaltRuntime`): plays an exported game. No editing code. |
| `Basalt-CLI/` | Headless command-line tool (`basalt`): project/scene authoring, validation, export. |
| `Tests/` | doctest unit tests (`BasaltTests`, headless), the feature-test project, and an optional GPU render test. |
| `Samples/` | Example games built through the automation API (e.g. `Samples/Tetris`), tested by ctest. |
| `cmake/` | Dependency pins (`Dependencies.cmake`), warnings, shader embedding, format check. |
| `scripts/` | `build.sh`, `test.sh`, `format.sh`. |

Engine modules (`Basalt/Source/Basalt/<Module>/`): `Core` (application, window, input, log, file
system, command line), `Events`, `Math`, `Renderer` (device, scene renderer, IBL, debug lines),
`ImGui`, `Scene` (EnTT scene, components, component registry, serialization), `Asset` (glTF, images,
primitives, asset cache), `Physics` (Jolt), `Audio` (miniaudio), `Scripting` (Lua 5.4 + sol2),
`Project` (project files, exporter), `Automation` (command API, session, TCP server).

Other directories: `Docs/` (scripting API, generated command/component references), `Tests/Data/FeatureTest`
(the project that exercises every feature).

## Architecture

- **One data path.** Components are plain structs. `ComponentRegistry` gives each a strict JSON reader/
  writer that every consumer shares: scene and prefab files, Lua `GetComponent/SetComponent`, the
  automation commands and the editor inspector. Adding a component = one registry entry (see the
  `basalt-add-component` skill).
- **Runtime state lives in systems.** `PhysicsWorld`, `ScriptEngine` and `AudioSystem` exist only while a
  scene plays and key their state by entity UUID; they react to EnTT component signals.
- **Play mode runs a copy.** `Scene::Copy` duplicates the edit scene (same UUIDs) for play/simulate;
  stopping discards it.
- **Automation first.** `CommandRegistry` (Automation/BuiltinCommands.cpp) is the API for tools and AI
  agents. The editor performs its edits through the same commands, so the GUI cannot do anything the
  API cannot. Hosts: `basalt` CLI (headless) and the editor's TCP server (127.0.0.1:7420, adds
  `editor.*` commands and screenshots). Docs: Docs/AutomationCommands.md (generated).
- **Headless core.** Everything except Renderer/ImGui/Window runs without a GPU; unit tests, the CLI and
  the feature test rely on that.

## Applications

| Binary | Purpose |
|--------|---------|
| `BasaltEditor [--project P] [--port N\|--no-server]` | Editor GUI + automation server |
| `BasaltRuntime [--project P] [--scene S] [--width W --height H] [--frames N [--screenshot out.png] [--require-validation]] [--debug-view V] [--no-vsync]` | Plays the project's start scene; exported games are this binary renamed |
| `basalt [--project P] [--scene S] [--no-save] [--verbose] <command> [json] \| batch <file> \| serve` | Headless automation CLI |
| `BasaltTests` | Unit tests |

## Building and testing

```bash
scripts/build.sh            # Debug build into build/
scripts/build.sh Release    # build-release/
scripts/test.sh             # build + ctest (unit tests, format check, ...)
./build/bin/BasaltTests     # run unit tests directly; add -tc="*name*" to filter
```

- Requirements: CMake ≥ 3.25, Ninja, a C++20 compiler, `glslc` (Vulkan SDK or shaderc), a Vulkan 1.3
  runtime (MoltenVK on macOS). All third-party code is fetched by CMake at pinned versions.
- Build configurations: `Debug` (asserts, validation), `Release` (optimized, asserts on),
  `Dist` (shipping: no asserts — defines `BS_DIST`).
- Warnings are errors (`BASALT_WARNINGS_AS_ERRORS=ON`). Do not disable warnings to make code compile.
- **macOS + Homebrew:** the Vulkan validation layer only loads when
  `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib` is set. Without it the engine logs a warning and runs
  without validation. Always set it when testing rendering changes.

## Code style (Hazel conventions)

Formatting is enforced by `.clang-format` (tabs, Allman braces for classes/functions/control flow,
namespace braces on the same line, namespace contents indented). Run `scripts/format.sh` before every
commit; the `CodeFormat` test fails on unformatted code.

Naming:

| Kind | Style | Example |
|------|-------|---------|
| Classes, structs, enums, namespaces, functions, member functions | PascalCase | `class SceneRenderer`, `void OnUpdate()` |
| Source files | PascalCase, named after the main type | `SceneRenderer.h/.cpp` |
| Local variables and parameters | camelCase | `float deltaTime` |
| Private/protected member variables | `m_` + PascalCase | `m_FrameIndex` |
| Static variables (class or file scope) | `s_` + PascalCase | `s_Instance` |
| Public struct data members (plain data, specs, components) | PascalCase, no prefix | `TransformComponent::Translation` |
| Enum values (`enum class`) | PascalCase | `LogLevel::Warn` |
| Macros | `BS_` + UPPER_SNAKE | `BS_CORE_ASSERT` |
| Compile-time constants | PascalCase | `constexpr uint32_t MaxLights = 16;` |

Other rules:

- `#pragma once` in every header. Include order: own header first (in .cpp), then `Basalt/...`
  headers, then third-party, then standard library, separated by blank lines.
- Engine includes use the full path from `Basalt/Source`: `#include "Basalt/Core/Log.h"`.
- Ownership: `Scope<T>` (unique) / `Ref<T>` (shared) from `Basalt/Core/Base.h`; raw pointers are
  non-owning. GPU objects use nvrhi handles (`nvrhi::TextureHandle`, ...).
- No exceptions across module boundaries. Fallible operations return `bool`/`std::optional`/a result
  struct and log the reason with `BS_CORE_ERROR`. Third-party exceptions (vulkan.hpp, nlohmann::json,
  sol2) are caught at the boundary that calls them.
- Asserts (`BS_CORE_ASSERT`) are for programmer errors only and vanish in Dist. Anything that can
  happen with bad user data (missing files, malformed scenes, script errors) must be handled and
  reported, never asserted.
- Logging: `BS_CORE_*` inside the engine, `BS_*` in applications/game code. Every message also lands
  in `Log::GetHistory()` (editor console, automation API).
- Comments explain *why*, not *what*. Every public class has a short comment stating its responsibility.

## Rendering conventions

- All rendering goes through nvrhi. Never call Vulkan directly outside `Renderer/GraphicsDevice.cpp`
  and `Renderer/VulkanLoader.cpp`, and never include `<vulkan/vulkan.hpp>` directly — use
  `Basalt/Renderer/VulkanHeaders.h`.
- The device requires Vulkan 1.3 (dynamic rendering, synchronization2, timeline semaphores). Stay within
  features MoltenVK supports: no geometry shaders, no tessellation, no ray tracing.
- **Shader binding convention:** every `nvrhi::BindingLayoutDesc` sets
  `bindingOffsets = ZeroBindingOffsets` (`Basalt/Renderer/ShaderUtils.h`), so GLSL
  `layout(set = S, binding = N)` equals nvrhi slot `N` for every resource type. Within one binding
  layout every slot number must be unique across resource types (a texture and a sampler cannot both
  use slot 0). A mismatch renders black without any validation error.
- **Clip space is +Y up (D3D convention), depth 0..1.** nvrhi's Vulkan backend flips the viewport, so
  projection matrices must *not* be Y-flipped, fullscreen passes map `ndc.y = 1 - 2 * uv.y`, and texture
  UV (0,0) is the top-left texel. Shadow/SSAO lookups convert with `uv = (ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5)`.
  Meshes wind counter-clockwise (front faces). Use the renderer's debug views (`--debug-view` on the
  runtime, View > Debug View in the editor) when something looks wrong.
- Binding layouts set `registerSpace = N` with `registerSpaceIsDescriptorSet = true`, so GLSL `set = N`
  matches. Push constants occupy a constant-buffer register in nvrhi: use `PushConstantSlot` in layouts that
  also contain constant buffers.
- Shaders are GLSL 450 in `Basalt/Shaders`, compiled with `glslc --target-env=vulkan1.2 -Werror`, and
  looked up by file name: `CreateEmbeddedShader(device, "ImGui.vert", nvrhi::ShaderType::Vertex)`.
- The swapchain is UNORM: the final pass writes display-referred (tonemapped, sRGB-encoded) color.

## Testing rules

- Every feature ships with unit tests in `Tests/Source/<Module>Tests.cpp`. Tests must be headless and
  deterministic (fixed timesteps, no wall-clock dependence, no network).
- New engine behaviour that is reachable from Lua or the automation API must also be exercised by the
  feature test (`Tests/Data/FeatureTest`: scene `Assets/Scenes/FeatureTest.bscene`, driver
  `Assets/Scripts/FeatureTest.lua`, run by ctest through `basalt batch FeatureTest.batch.json`). Add a
  `Check(...)` for every new API function and put every new component in the scene.
- GPU tests (`FeatureTestRender`) are registered by `scripts/build.sh` unless `CI` is set: they render the
  feature scene and fail on any validation error — and also when the Khronos validation layer is not
  installed (`--require-validation`). Install the Vulkan SDK (or Homebrew `vulkan-validationlayers`).
- Bug fixes come with a regression test.
- Rendering changes: run the editor or runtime with validation enabled and confirm zero validation
  errors (`GraphicsDevice::GetValidationErrorCount()`).

## Documentation

- Lua API: `Docs/ScriptingAPI.md` (update with every binding change).
- `Docs/AutomationCommands.md` and `Docs/Components.md` are generated: run
  `python3 scripts/generate_docs.py` after changing commands or components.
- Skills in `.claude/skills/`: build/test, code review, make-game (AI workflow), add-component,
  debug-rendering.

## Commit workflow

1. `scripts/format.sh` and `scripts/test.sh` — everything green.
2. Review the diff (use the code-review skill/agent): style compliance, error handling, tests present.
3. Commit with a descriptive message (imperative mood, `Module: summary` subject line). Push to
   `origin main` (https://github.com/ahmad-luqman/AIGameEngine).

## Dependencies

All third-party libraries are pinned in `cmake/Dependencies.cmake` to an exact tag or commit. To
upgrade one, change the pin, rebuild from a clean build directory, and run the full test suite.
Never vendor modified third-party code into the repository.
