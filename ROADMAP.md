# Basalt Roadmap

This file lists what Basalt does not do yet, ordered by priority, with enough detail to start work on any
item. It is based on a review of the code as of commit `9dc130d` (October 2026). Every item follows the
normal workflow in AGENTS.md: unit tests, feature-test coverage for anything reachable from Lua or the
automation API, docs, and a code review before committing.

Size estimates: **S** = a day or less, **M** = a few days, **L** = a week or more.

## Current state (baseline)

- All requirements of the original specification are implemented: glTF import, gizmos, PBR, IBL from
  HDRIs, soft cascaded shadows, SSAO, HDR + tonemapping, Jolt physics, Lua scripting with
  creation/destruction/prefabs, an editor, export, and a full automation API (CLI + TCP).
- CI builds and tests on Windows, Ubuntu and macOS (Debug + Release) — but without a GPU. Rendering has
  only been verified on macOS (Apple M4 Max via MoltenVK) with zero validation errors.
- 70 unit test cases, a feature test with 308 Lua checks, and a Tetris sample with a scripted gameplay
  test.

---

## P0 — Verification and robustness

These close the gap between "works on the developer's machine" and "production-grade". Do them before
adding features.

### Golden-image comparison — M
The GPU test catches validation errors but not wrong pictures (a flipped shadow or a black SSAO buffer
passes today).
- Store reference PNGs per debug view (`Lit`, `SSAO`, `Normals`, `Depth`) for the feature scene, compare
  with a perceptual tolerance (per-pixel delta + percentage threshold, as FLIP-style metrics do), and
  write a diff image on failure.
- Keep separate references per driver (lavapipe vs. MoltenVK) or use a tolerance loose enough for both;
  document how to update them (`scripts/update_golden.sh`).
- Done when: deliberately breaking a shader convention fails the test.

### Fuzz the untrusted inputs — M
Scenes, prefabs, projects, glTF files, images and automation requests all come from outside the engine.
The strict JSON readers and cgltf wrappers should never crash or assert on bad input.
- libFuzzer targets for `SceneSerializer` (scene + prefab), `MeshImporter`, `TextureSource`,
  `CommandRegistry::Execute` and `LuaJson`; seed corpora from the test data.
- Run each target for a short time in CI, and longer runs locally or nightly.

### Determinism and replay tests — M
Physics and scripting are deterministic by design (fixed step, sorted contacts, seeded random), but only
spot-checked.
- Record an input stream (`input.key`/`input.mouse` per frame) during play, replay it twice, and compare
  the scene state hash after N frames. Add the hash as a `play.step` result field.
- Done when: Tetris replays to the same board on all three platforms.

### Real-GPU smoke tests on Windows and Linux — S (manual) / M (automated)
Lavapipe does not catch driver-specific bugs. Before a release, run the runtime with validation on at
least one NVIDIA, one AMD and one Intel GPU. Optionally add a self-hosted runner. Record results in a
`Docs/TestedHardware.md` table.

---

## P1 — Core engine features

The features most games need, roughly in order of impact.

### Physics

- **Mesh and convex-hull colliders — M.** New `MeshColliderComponent` (`Mesh`, `MeshIndex`, `Convex`
  flag). Static bodies use `JPH::MeshShape`, dynamic bodies a `JPH::ConvexHullShape` built from the
  vertices. Cache cooked shapes per mesh asset. Without this, imported levels cannot collide by their
  real geometry.
- **Character controller — M.** Wrap `JPH::CharacterVirtual` in a `CharacterControllerComponent` (slope
  limit, step height, max strength) with Lua `Move(velocity)`, `IsGrounded()`, `GetGroundNormal()`.
  Platformers and first-person games need it, and `FixedRotation` dynamic bodies are a poor substitute.
- **Shape queries — S.** `Physics.SphereCast`, `Physics.BoxCast`, `Physics.OverlapSphere`,
  `Physics.OverlapBox`, and a layer-mask parameter on `Raycast`. Return all hits optionally.
- **Joints — M.** Fixed, hinge, slider, distance and point constraints (`JointComponent` referencing a
  second entity by UUID), with breaking force and motor settings for hinges and sliders.
- **Continuous collision — S.** Expose Jolt's `EMotionQuality::LinearCast` as a `RigidBody.Continuous`
  flag so fast projectiles do not tunnel.
- **Physics materials and per-pair callbacks — S.** Combine modes for friction and restitution;
  contact point, normal and impulse in `OnCollisionBegin`.

### Rendering

- **Point and spot light shadows — L.** A shadow atlas (spot: one perspective map; point: cube faces or
  dual-paraboloid), allocated per frame by screen-space importance, with PCF filtering. Add
  `CastShadows` and `ShadowSoftness` to both light components.
- **Clustered forward lighting — M.** Today every pixel loops over all point and spot lights (up to 256).
  Build a froxel grid (for example 16×9×24) in a compute pass and give each cluster a light list. This
  keeps the cost per pixel bounded with hundreds of lights.
- **Anti-aliasing — M.** FXAA as a cheap default (one post pass), then TAA (jittered projection, motion
  vectors from the depth prepass, history reprojection with neighbourhood clamping), which also
  stabilizes SSAO and specular shimmer. MSAA is an option for the forward pass but complicates the SSAO
  prepass.
- **Bloom — S.** Downsample/upsample chain (Call of Duty: Advanced Warfare style) on the HDR buffer
  before tonemapping; makes emissive materials read as bright.
- **Auto exposure — S.** Luminance histogram in compute, with adaptation speed and EV clamps in a new
  `PostProcessComponent` (or scene settings) alongside the tonemapper choice.
- **Multiple directional lights — S.** Only the first casts shadows; the others should still contribute
  unshadowed light (today they are ignored).
- **Skinned meshes and animation — L.** Import glTF skins and animations (cgltf already parses them),
  skin on the GPU in a compute pass or in `Mesh.vert`, and add an `AnimatorComponent` with play, blend
  and crossfade from Lua. Extend the importer test with Khronos `CesiumMan`/`Fox` sample assets.
- **glTF extensions — M.** `KHR_texture_transform`, `KHR_materials_emissive_strength`,
  `KHR_materials_clearcoat`, `KHR_materials_transmission` (via the transparent pass),
  `KHR_mesh_quantization`, `KHR_texture_basisu`. Unknown required extensions should fail the import with
  a clear message.
- **GPU instancing and draw batching — M.** Group draws by mesh and material into instanced draws with a
  per-instance structured buffer. Needed for scenes with thousands of objects (voxel, puzzle and
  strategy games).
- **Reflection probes and SSR — L.** Local box-projected cubemap probes captured in the editor, then
  screen-space reflections for contact detail.
- **Render scale and dynamic resolution — S.** Render at a fraction of the window size, upscale in the
  tonemap pass.
- **Decals, particles, text in 3D — L each.** A GPU particle system (emitter component, compute
  simulation, sorted billboards) is the most useful for games.

### Scripting

- **Timers and coroutines — S.** `Timer.After(seconds, fn)`, `Timer.Every(seconds, fn)` and
  `self:StartCoroutine(fn)` with `Wait(seconds)` and `WaitFrames(n)`, owned by the instance and cancelled
  when it is destroyed.
- **Scene switching — S.** `Scene.Load(path)` deferred to the end of the frame, with an optional
  `DontDestroyOnLoad` for persistent managers.
- **Persistent save data — S.** `Storage.Set(key, value)`, `Storage.Get(key)`, `Storage.Save()` writing
  JSON to a per-game user-data directory (platform-specific app data). This keeps the file-system
  sandbox intact.
- **Events and messaging — S.** `Events.Emit(name, payload)` / `Events.On(name, fn)` for decoupled game
  logic, cleared when play stops.
- **Script hot reload — M.** Watch `.lua` files during play; on change, reload the class table and rebind
  existing instances (keeping their fields), then call an optional `OnReload`. Saves a lot of iteration
  time, including for AI agents.
- **Lua type definitions — S.** Generate `Docs/basalt.d.lua` (LuaLS `---@class` annotations) from the
  bindings so editors and AI agents get completion and type checks. Add a ctest that keeps it in sync.

### Input

- **Gamepads — S.** GLFW already provides the SDL gamepad mapping database; expose
  `Input.IsGamepadButtonDown(index, button)`, `Input.GetGamepadAxis(index, axis)`, and connection
  events. Add `input.gamepad` to the automation API so tests can drive it.
- **Action mapping — M.** Named actions and axes (`"Jump"`, `"MoveX"`) bound to keys, mouse and gamepad
  in a project-level `Input.binput` file, read with `Input.GetAction("Jump")`. Lets players rebind
  controls without script changes.
- **Cursor control — S.** `Window::SetCursorLocked` exists but is not reachable from scripts; expose
  `Input.SetCursorMode("Locked" | "Hidden" | "Normal")` for first-person cameras.

### Audio

- **Mixer buses — S.** Master, Music, SFX and UI groups (`ma_sound_group`) with Lua volume control and
  an `AudioSource.Bus` field.
- **Streaming music — S.** Use `MA_SOUND_FLAG_STREAM` for long clips instead of decoding them fully into
  memory.
- **Effects — M.** Low-pass, reverb and doppler via miniaudio nodes; `AudioSource.Doppler`.
- **More tests — S.** Only one audio test case exists. Cover the source lifecycle, spatial attenuation
  with the deviceless engine, looping, and pitch.

### Game UI

- **Images and nine-slice panels — S.** `UI.Image(texture, x, y, w, h [, tint])`.
- **Interactive widgets — M.** `UI.Button`, `UI.Slider` and `UI.Checkbox` that return their state, with
  keyboard/gamepad navigation. Needed for menus and settings screens.
- **Fonts — S.** Load TTF fonts from the project (`UI.Text(..., { Font = "Assets/Fonts/X.ttf" })`); today
  there is one built-in font.
- **Anchors — S.** Anchor UI elements to screen edges so HUDs work at any aspect ratio.

---

## P2 — Editor and asset pipeline

### Editor

- **Native file dialogs — S.** Add nativefiledialog-extended (pinned) for Open/Save/Import, keeping the
  text-box fallback for headless and Linux systems without a portal.
- **Multi-select — M.** Select several entities in the hierarchy and viewport (box select), move them
  with one gizmo, edit shared fields in the inspector. The `editor.select` command takes a list.
- **Prefab editing and overrides — L.** Open a prefab in an isolated scene, save, and propagate changes
  to instances while keeping per-instance overrides (track overridden fields in `PrefabComponent`).
- **Asset thumbnails and material preview — M.** Render small previews of meshes, materials and HDRIs
  into a cache for the content browser.
- **Undo with diffs — M.** Snapshot undo copies the whole scene per edit. Store per-command JSON patches
  (`component.set` already knows old and new values) to bound memory on large scenes.
- **Grid, snapping and gizmo options — S.** World/local toggle, snap increments in the toolbar, and
  vertex snapping.
- **Editor preferences — S.** Persist camera speed, layout, recent projects and theme per user.
- **Play-mode stats overlay — S.** Frame time, draw calls, triangle count, physics bodies and Lua
  memory.

### Asset pipeline

- **Stable asset IDs — M.** Assets are referenced by path, so renaming or moving a file breaks scenes.
  Add `.meta` sidecar files with a UUID, resolve references by UUID with the path as a hint, and make
  `asset.move`/`asset.rename` update both.
- **Asynchronous loading — M.** Meshes and textures load synchronously on the main thread, so large
  glTFs stall the frame. Load and decode on a worker thread pool, upload on the main thread, and show a
  placeholder meanwhile. Requires a small job system.
- **Texture compression — M.** Import textures to KTX2 with BC7 (desktop) and keep the source; cuts VRAM
  use and load time roughly 4x. Use `basisu` or `bc7enc` at import time, and cache the results.
- **Mesh optimization — S.** Run meshoptimizer (vertex cache, overdraw, vertex fetch) at import time and
  generate LODs with its simplifier.
- **File watching — S.** Detect changed assets on disk and call `AssetManager::Reload` automatically in
  the editor.

---

## P3 — Shipping and distribution

- **Packed asset archive — M.** Export into a single `.bpak` file (index + compressed blobs, for example
  zstd) instead of loose files. Faster loading and simpler distribution.
- **Platform packaging — M.** macOS `.app` bundle with `Info.plist`, icon, codesigning and notarization
  (a script that takes a Developer ID); Windows `.exe` icon and version resource; Linux AppImage.
- **Project icon and version — S.** `ProjectConfig.Icon`, `Version` and `Company`, used by the exporter
  and the window title.
- **Graphics settings — S.** `ProjectConfig` options for VSync, resolution scale, shadow quality and SSAO
  quality, exposed to Lua so games can offer a settings menu.
- **Crash reporting — M.** Install crash handlers (minidumps on Windows, signal handlers with a stack
  trace on macOS/Linux), write the last log lines and a crash file next to the executable, and show a
  message box in the runtime.
- **Release pipeline — S.** A GitHub Actions workflow on tags that builds Dist on all three platforms,
  exports the samples, and attaches editor, runtime and CLI archives to a GitHub release.

---

## AI-agent workflow

The specification asks for an editor that an AI agent can fully control. These items make that faster and
more reliable.

- **MCP server — M.** Expose the `CommandRegistry` as a Model Context Protocol server (stdio transport in
  `basalt mcp`, plus the editor's TCP server), generating tool schemas from the command definitions.
  Agents then get typed commands without reading the docs first.
- **Command parameter schemas — S.** Give every command a JSON Schema for its parameters; validate
  requests against it and include it in `Docs/AutomationCommands.md` and `basalt help <command>`.
- **Event subscriptions — S.** `events.subscribe` on the TCP server to stream log lines, script errors,
  play-state changes and collision events, instead of polling `log.get`.
- **Scene diff and assertions — S.** `scene.diff` (between two saved scenes or snapshots) and
  `assert.*` commands (`assert.entity_exists`, `assert.component`) so game tests read as specifications.
- **Visual checks — S.** `render.compare` to compare a screenshot with a reference image and return a
  similarity score. This reuses the golden-image code from P0.
- **More samples — M each.** Breakout (physics + UI), a 3D platformer (character controller + camera),
  and a small first-person scene (mouse look + raycasts + audio). Each one is built through the
  automation API and adds a ctest gameplay test, which also checks the API is sufficient.

---

## Engine internals

- **Job system — M.** A small engine-wide thread pool with work stealing, used by asset loading,
  animation and culling. Physics currently owns a private `JPH::JobSystemThreadPool`; route it through
  the shared pool (Jolt's `JobSystemWithBarrier`) so the two do not oversubscribe the CPU.
- **GPU profiler — S.** nvrhi timer queries per pass, shown in the editor stats overlay and returned by a
  `render.stats` command.
- **CPU profiler integration — S.** Optional Tracy instrumentation behind `BASALT_PROFILE`, compiled out
  by default.
- **Memory tracking — S.** Track allocations per subsystem (assets, GPU buffers, Lua heap) and report them
  in the stats overlay and `render.stats`.
- **Performance benchmarks — M.** A benchmark scene with thousands of entities, many lights and physics
  bodies, run in Release on CI with frame-time thresholds to catch regressions.

---

## Platform notes

- **macOS validation layer path.** The Homebrew validation layer only loads with
  `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib`. The engine could add `/opt/homebrew/lib` to the layer
  search itself (or point `VK_ADD_LAYER_PATH` at a layer manifest with absolute library paths) when it
  detects a Homebrew installation in non-Dist builds.
- **Wayland.** GLFW 3.4 supports Wayland; test the editor under a Wayland session on Ubuntu 24.04 and
  document any issues (window decorations, cursor capture, clipboard).
- **HDR output — L.** Use an HDR10 / scRGB swapchain where available (Windows, macOS EDR) with a
  display-mapped tonemapper.
