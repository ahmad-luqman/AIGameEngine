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

Engine modules (`Basalt/Source/Basalt/<Module>/`): `Core` (application, window, input incl. gamepads, log, file
system and file watching, command line), `Events`, `Math`, `Renderer` (device, scene renderer, IBL, debug lines), `ImGui`, `Scene`
(EnTT scene, components, component registry, serialization), `Asset` (glTF, images, primitives, asset
cache), `Physics` (Jolt), `Audio` (miniaudio: buses, per-source low-pass and Doppler, reverb sends into per-bus reverbs), `Particles` (CPU particle simulation), `Animation` (glTF TRS sampling, blends, crossfades, 1D/2D blend spaces with phase sync and velocity matching, animation events, bone transforms and attachments, data-driven state machines and per-state root motion, layers with joint masks and additive clips, two-bone/look-at IK and foot placement, spring bones, per-entity playback), `UI` (game UI: UI
component layout, widgets and their mouse/keyboard/gamepad interaction, immediate-mode `UI.*`, TrueType fonts;
drawn through ImGui after the scene), `Scripting` (Lua 5.4 + sol2; timers, coroutines, events), `Project`
(project files, exporter, save data), `Automation` (command API, session, TCP server, editor history and selection).

Other directories: `Docs/` (scripting API, rendering guide, generated command/component references), `Tests/Data/FeatureTest`
(the project that exercises every feature).

## Architecture

- **One data path.** Components are plain structs. `ComponentRegistry` gives each a strict JSON reader/
  writer that every consumer shares: scene and prefab files, Lua `GetComponent/SetComponent`, the
  automation commands and the editor inspector. Adding a component = one registry entry (see the
  `basalt-add-component` skill).
- **Runtime state lives in systems.** `PhysicsWorld`, `ScriptEngine`, `AudioSystem`, `AnimationSystem` and `ParticleSystem` exist
  only while a scene plays (physics, animation and particles also while it simulates) and key their state by entity
  UUID; they react to EnTT component signals. `UIInteraction` (widget hover, hold and focus) exists only in Play
  and re-reads the widgets every frame. Gameplay random streams (Lua's random numbers, particle
  emitters and runtime entity UUIDs) are seeded from the session's seed, `Scene::GetRandomSeed` (the project's `RandomSeed`).
- **Post-processing.** The highest-priority enabled `PostProcessComponent` (lowest UUID on ties) overrides legacy scene exposure/tonemapper/SSAO settings. Bloom and histogram exposure precede tonemapping; FXAA precedes FSR and overlays. Exposure history belongs to each renderer; captures reset it. `Graphics` Lua preferences are a session copy of project defaults and survive `Scene.Load` without saving the project.
- **Animation graphs and root motion.** `Animator.StateMachine` names a shared `.banimgraph` asset
  (`AnimationGraph`, cached/reloaded by `AssetManager`, authored through `asset.write/read`). Runtime
  parameters and triggers belong to each playback. Transitions use array priority, at most one per update;
  successful transitions consume their trigger conditions. `Animator.RootMotion` is the master gate and
  graph states also opt in; `RootJoint` explicitly names the extraction node. Animation advances after
  scripts and before physics; horizontal root displacement queues for character fixed steps, retaining
  gameplay vertical input (and, during fades to or from states without root motion, the faded share of gameplay
  horizontal input), or updates the world transform. Attachments and events run after physics and
  LateUpdate, before particles.
- **Layers, IK and spring bones.** A pose is built in stages: the base playback (`Playback::Local`, which fades, snapshots and root
  stripping use), then `Animator.Layers` in order (`Pose`; each layer's clip, mask and reference pose are resolved again
  only when its clip, blend, reference or mask changes, and the root-motion exclusion when the root-motion joint
  does), then the IK and spring bone overlays, composed into `Globals`. `InverseKinematicsComponent` chains solve in
  `AnimationSystem::LateUpdate` (after physics and OnLateUpdate, before attachments) from `Pose`, never from the last
  solution; the solved joints' local transforms are kept as an overlay so a pose evaluated again that frame (Play in a
  callback) keeps the IK. Foot rays use `PhysicsWorld::RaycastAll` and skip the entity, its descendants and its
  character controller's entity and descendants. IK targets and weights live in the system keyed by entity (they
  survive playback resets); layer state belongs to the playback. Solvers (`Animation/InverseKinematics.h`) are
  stateless free functions that handle mirrored (negative-determinant) parents. `SpringBoneComponent` chains swing right
  after IK, from `Pose` plus the IK overlay (never their own last solution), as world-space points
  (`Animation/SpringBones.h`, substeps of at most 1/120 s) kept in the system by entity; their joints go into a second overlay
  that `Finish` applies after the IK one.
- **Play mode runs a copy.** `Scene::Copy` duplicates the edit scene (same UUIDs) for play/simulate;
  stopping discards it. In the editor the game runs in the Game tab (primary camera at the project's window
  size, letterboxed; the game UI takes the pointer while the Game image is hovered and navigation while the Game
  tab has focus); the
  scene view keeps the editor camera in every mode.
- **Editor caches.** Content-browser previews are cached in `<project>/Cache/Thumbnails`, outside `Assets/`
  (never exported; new projects ignore `Cache/` in git). Editor preferences (camera speed, theme, recent projects,
  auto reload) and the window layout are per user (`Automation/EditorPreferences`, `<user data>/BasaltEditor/`).
- **One file watcher.** `AutomationSession` owns a polling `FileWatcher` (Core) on the active project's `Assets/`;
  `ReloadAssets` drops what changed files affect from `AssetManager` (models through `MeshSource::ExternalFiles`) and,
  while the game runs, hot reloads scripts (`ScriptEngine::ReloadScripts`: instances keep their fields, then
  `OnReload`; Play only). The editor scans on a worker thread and applies the results each frame on the main thread
  (while autoReload is on); agents call `asset.refresh`; files the session writes itself (`OnFileWritten`) are
  acknowledged and never reported.
- **Input: host reads vs game reads.** `Input` holds every event (editor shortcuts and camera read it); the game
  (Lua's `Input`, input actions) reads `GameInput`, which sees only what the host routes to it: `Input::SetGameFocus`
  (keyboard, gamepads) and `SetGamePointer` (mouse), the same flags as `GameUI`'s navigation and pointer (the editor
  follows its Game view; runtime and headless route everything). Lua mouse coordinates are game-view units
  (`GameUI::ToGamePosition`: the editor maps to the game resolution). Input actions are the project's
  `Input.binput` (`InputActionMap`, `ProjectConfig::InputActions`, `project.set inputActions`); a play session reads
  a copy in `Scene` that Lua may rebind until play stops. `Input::SetCursorMode` only stores the game's request;
  the runtime and the editor's Game view apply it. The runtime sets the game's view (its window) before scripts run
  and every frame. The editor sets geometry, resolution and routing before every scene update and as Play starts
  (`AutomationSession::BeforeRuntimeStart`), and calls `AutomationSession::RecordViewChange` (as `play.step` does)
  so replays see routing changes; a recording begun before play marks play's start (`playStart`).
- **Gameplay services live in the ScriptEngine.** Timers and coroutines (`ScriptTasks`) and events
  (`ScriptEvents`) belong to the script instance that created them and end with it; `Storage` save data
  (`Project/SaveData`) goes to the per-user data directory (`Platform::GetUserDataDirectory`, overridden by
  `BASALT_USER_DATA_DIR`, which tests set so they never touch the real one). `Scene.Load` switches scenes
  in place at the end of the frame (`Scene::RequestLoad`), so the Lua state and persistent entities
  survive.
- **Automation first.** `CommandRegistry` (Automation/BuiltinCommands.cpp) is the API for tools and AI
  agents. The editor performs its edits through the same commands, so the GUI cannot do anything the
  API cannot. Edit-mode scene mutations must call `AutomationSession::MarkDirty` (or `MarkEdited` in
  built-in commands); its revision counter tells editor history which commands need capture, including
  partial edits before a failure. Commands that save/replace edit state or start/restart Play declare
  `CommandPolicy::EditBoundary`; undo/redo/transaction commands declare `HistoryControl`. Hosts:
  `basalt` CLI (headless) and the editor's TCP server (127.0.0.1:7420, adds
  `editor.*` commands and screenshots). Docs: Docs/AutomationCommands.md (generated), Docs/MCP.md (agent setup/protocol). CommandInfo declares
  InputSchema: validation, help and MCP tool definitions share it; editor TCP clients can use MCP too.
  `AutomationEventHub` (created by the session, shared with TCP connections, thread-safe) feeds `events.*`: the session's `SceneEventListener` on the
  play copy reports contacts, animation events and script errors, a `LogHistory` listener log lines; legacy TCP connections stream
  their subscriptions (`AutomationServer::ClientHandler::TakePending`). Agent checks: `scene.snapshot`/`scene.diff`
  (`Scene/SceneDiff`), `assert.*`, `render.compare`.
- **CPU jobs and streaming.** `Core/JobSystem` is the shared work-stealing pool, including Jolt physics.
  Cache APIs stay on the main thread; asset workers capture project context and publish isolated results
  through `AssetManager::PublishCompleted`. Blocking getters join requests. Interactive renderers opt into
  streaming; captures keep blocking loads. `BASALT_PROFILE` enables Tracy outside Dist. `memory.stats`
  reports logical asset payload, GPU buffer and Lua heap charges (see Docs/Rendering.md).
- **Headless core.** Everything except Renderer/ImGui/Window runs without a GPU; unit tests, the CLI and
  the feature test rely on that.

## Applications

| Binary | Purpose |
|--------|---------|
| `BasaltEditor [--project P] [--port N\|--no-server] [--gpu G]` | Editor GUI + automation server |
| `BasaltRuntime [--project P] [--scene S] [--width W --height H] [--frames N [--screenshot out.png] [--require-validation] [--fail-on-script-errors]] [--headless \| --offscreen] [--debug-view V] [--render-scale S] [--dynamic-resolution FPS] [--min-render-scale S] [--gpu G] [--no-vsync] [--stats]` | Plays the project's start scene; exported games are this binary renamed. `--headless` runs without a GPU or window. `--frames` runs exit 1 (start/render failure), 3 (validation errors), 4 (screenshot), 5 (validation layer missing), 6 (dynamic resolution did not run) or 7 (script errors, with `--fail-on-script-errors`) |
| `basalt [--project P] [--scene S] [--no-save] [--verbose] <command> [json] \| batch <file> \| serve \| mcp` | Headless automation CLI |
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
- CI also runs ASan+UBSan (`-DBASALT_SANITIZE="address;undefined"`), clang-tidy (`-DBASALT_CLANG_TIDY=ON`,
  checks in `.clang-tidy`), a coverage report (`scripts/coverage.sh`) and libFuzzer targets for every
  untrusted input (`scripts/fuzz.sh`, `Tests/Fuzz/`; nightly long runs). Parse untrusted JSON with
  `ParseJson` and read floats with `JsonToFloat` (`Basalt/Core/JsonUtils.h`). Tests carry ctest labels
  (`unit`, `integration`, `gpu`, `format`) for `ctest -L/-LE`. Details: the basalt-build-test skill.
- **macOS + Homebrew:** Homebrew's validation layer manifest names its library without a path, so non-Dist
  builds point `VK_ADD_LAYER_PATH` at a copy with the absolute path (`Renderer/ValidationLayerManifest`; prefixes
  `$HOMEBREW_PREFIX`, `/opt/homebrew`, `/usr/local`); no
  `DYLD_FALLBACK_LIBRARY_PATH` is needed. Check the log for `validation enabled` when testing rendering changes.

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
- `PhysicsDeterminism` (`Tests/Data/Determinism`) pins the state hashes of a physics-heavy scene, which
  must match on every CI platform and configuration (Debug, Release, ASan, coverage). Deliberate changes to
  physics, the runtime UUID stream, scene serialization or the component registry legitimately move them: update the expected
  hashes (see its README). An unexplained change is a determinism bug.
- GPU tests (label `gpu`) are registered by `scripts/build.sh` unless `CI` is set (CI registers them
  explicitly in its lavapipe job): they render the feature scene and fail on any validation error — and
  also when the Khronos validation layer is not installed (`--require-validation`). Install the Vulkan
  SDK (or Homebrew `vulkan-validationlayers`). `FeatureTestRender` runs `--offscreen` (no window or display
  server needed); `FeatureTestRenderWindowed` covers the swapchain path on developer machines.
- `--offscreen` renders into device-owned images instead of a window; `--gpu <index|name>` picks the
  GPU (indices and names are logged as `GPU N:` at startup; a name must match exactly one device).
- Bug fixes come with a regression test.
- Rendering changes: run the editor or runtime with validation enabled and confirm zero validation
  errors (`GraphicsDevice::GetValidationErrorCount()`).

## Documentation

- Rendering: `Docs/Rendering.md` (clustered lighting, FSR 1 settings, diagnostics and GPU tests).
- Lua API: `Docs/ScriptingAPI.md` and the LuaLS definitions `Docs/basalt.d.lua` (update both with every binding change;
  the `LuaTypeDefinitions` test checks declared names and component fields and types; signatures are kept by hand).
- `Docs/AutomationCommands.md`, `Docs/Components.md` and the component section of `Docs/basalt.d.lua` are
  generated: run `python3 scripts/generate_docs.py` after changing commands or components (the `GeneratedDocs`
  ctest fails while they are stale).
- Skills in `.claude/skills/`: build/test, code review, make-game (AI workflow), add-component,
  debug-rendering.
- `ROADMAP.md`: prioritized list of missing features and verification work. Remove an item when it ships.

## Commit workflow

1. `scripts/format.sh` and `scripts/test.sh` — everything green.
2. Review the diff (use the code-review skill/agent): style compliance, error handling, tests present.
3. Commit with a descriptive message (imperative mood, `Module: summary` subject line). Push to
   `origin main` (https://github.com/ahmad-luqman/AIGameEngine).

## Dependencies

All third-party libraries are pinned in `cmake/Dependencies.cmake` to an exact tag or commit. To
upgrade one, change the pin, rebuild from a clean build directory, and run the full test suite.
Never vendor modified third-party code into the repository.
