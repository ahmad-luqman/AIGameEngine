# Basalt Roadmap

This file lists what Basalt does not do yet, ordered by priority, with enough detail to start work on any
item. It is based on a review of the code as of commit `670c192` (October 2026), plus ideas from comparing
Basalt with a similar AI-built engine shown in The Cherno's "Can AI Make a Game Engine?" video. Every item follows the
normal workflow in AGENTS.md: unit tests, feature-test coverage for anything reachable from Lua or the
automation API, docs, and a code review before committing.

Size estimates: **S** = a day or less, **M** = a few days, **L** = a week or more.

**Execution.** Work in milestones rather than one item at a time: P0 → particles and UI components →
P2 editor feel → the remaining agent-tested samples. Run each milestone as one long autonomous session at high effort
(or with multi-agent workflows), and check quality and token cost before starting the next.

## Current state (baseline)

- All requirements of the original specification are implemented: glTF import, gizmos, PBR, IBL from
  HDRIs, soft cascaded shadows, SSAO, HDR + tonemapping, Jolt physics, Lua scripting with
  creation/destruction/prefabs, an editor, export, and a full automation API (CLI + TCP).
- CI builds and tests on Windows, Ubuntu and macOS (Debug + Release) — but without a GPU. Rendering has
  only been verified on macOS (Apple M4 Max via MoltenVK) with zero validation errors.
- Over 600 unit test cases, a feature test with over 500 Lua checks, and Tetris, Joints, Marble Run and Breakout
  samples with scripted gameplay tests.

---

## P0 — Verification and robustness

These close the gap between "works on the developer's machine" and "production-grade". Do them before
adding features.

### Real-GPU smoke tests on Windows and Linux — S (manual) / M (automated)
Lavapipe does not catch driver-specific bugs. The procedure and results table are in
`Docs/TestedHardware.md` (`--offscreen`, `--gpu`, golden comparison, interactive checklist). Remaining:
run it on at least one NVIDIA, one AMD (e.g. AWS g4ad) and one Intel GPU, on Windows and native Linux, and
fill in the table. Optionally add an on-demand self-hosted GPU runner later.

---

## P1 — Core engine features

The features most games need, roughly in order of impact.

### Physics

- **Soft six-DOF rotation limits — S.** Jolt 5.6 softens only six-DOF translation limits. When it adds
  springs to rotation limits, apply `LimitSpringFrequency` to them too.

### Rendering

- **Particle follow-ups — M.** `ParticleEmitterComponent` (Particles/ParticleSystem) covers CPU simulation,
  bursts, shapes, start-to-end color and size, alpha/additive billboards and Lua control. Missing: soft
  particles (fade against scene depth; needs a depth copy, since the transparent pass has depth attached),
  per-particle rotation and random start size/color, color/size curves with several keys (needs an
  inspector widget for arrays of objects), lit particles, an edit-mode preview, and simulation in a compute
  pass if particle counts demand it.
- **Temporal anti-aliasing — M.** TAA (jittered projection, motion
  vectors from the depth prepass, history reprojection with neighbourhood clamping), which also
  stabilizes SSAO and specular shimmer. Builds on the post-process stack’s FXAA. MSAA is an option for
  the forward pass but complicates the SSAO prepass.
- **glTF extensions — M.** `KHR_texture_transform`, `KHR_materials_emissive_strength`,
  `KHR_materials_clearcoat`, `KHR_materials_transmission` (via the transparent pass),
  `KHR_mesh_quantization`, `KHR_texture_basisu`. Unknown required extensions should fail the import with
  a clear message.
- **GPU instancing and draw batching — M.** Group draws by mesh and material into instanced draws with a
  per-instance structured buffer. Needed for scenes with thousands of objects (voxel, puzzle and
  strategy games).
- **Reflection probes and SSR — L.** Local box-projected cubemap probes captured in the editor, then
  screen-space reflections for contact detail.
- **Decals and text in 3D — L each.**

### Animation

Skeletal playback, crossfades and a two-clip weighted blend (`AnimationSystem`) are implemented; physics
already provides character controllers (`MoveCharacter`, `IsCharacterGrounded`, ground normal) and
raycasts. The items below turn that into natural character movement (walk, run, jump, uneven ground), in
order of impact.

- **Animation events — S.** Named markers on a clip's timeline (footsteps, attack hit frames, VFX cues),
  declared per clip on the `Animator` (needs the array-of-objects inspector widget, see Particles) and
  delivered to scripts as `OnAnimationEvent(name, clip)`. Fire every marker crossed in a frame, including
  across a loop wrap, a seek and during crossfades (outgoing clip events optional).
- **Bone attachments — S.** `e:GetBoneTransform(joint)` (world space) and a component that keeps an
  entity attached to a joint of another entity's `Animator` (weapon in hand, hat on head), updated after
  the pose is sampled and before transforms are consumed by physics and rendering.
- **Blend spaces and speed sync — M.** Generalize `Blend` to 1D and 2D blend spaces: N clips placed on
  parameter axes (speed for idle/walk/jog/run, speed × direction for strafing), weighted by the parameter
  value. Synchronize the clips by normalized time (phase) so feet stay in step, and add an Animator
  playback-speed mode that matches the clip to the character's actual velocity to prevent foot sliding.
- **Animation state machine — L.** A data-driven graph (project asset, edited through automation commands
  so agents can author it) of states (a clip or blend space) and transitions with conditions on named
  parameters (`Speed`, `Grounded`) and triggers (`Jump`), crossfade durations and exit times. Lua:
  `e:SetAnimationParameter(name, value)`, `e:SetAnimationTrigger(name)`, `e:GetAnimationState()`. Jumps
  split into takeoff, an in-air loop and landing because airtime comes from physics. A visual graph editor
  is a P2 follow-up.
- **Root motion — M.** Extract the root joint's horizontal translation and yaw delta per frame, remove
  them from the displayed pose, and apply them through `MoveCharacter` (or the transform when the entity
  has no character) when the Animator enables `RootMotion`. Per-state opt-in once the state machine exists
  (attacks, vaults, climbing), so locomotion can stay physics-driven.
- **Inverse kinematics — M.** Analytic two-bone IK (legs, arms), look-at/aim chains (head and spine) and
  foot placement: raycast under each foot, solve leg IK so the foot lands on the surface, lower the
  pelvis by the largest offset and align feet to the ground normal, with smoothed weights so feet don't
  pop. Runs after sampling and before skinning; targets and weights settable from Lua.
- **Layers, masks and additive clips — M.** Ordered animation layers with per-joint masks (upper body
  aims or waves while the legs run) and additive layers (breathing, recoil, lean) applied as deltas from a
  reference pose on top of the base pose.
- **Ragdolls — M.** Build a Jolt `Ragdoll` from the skeleton (a capsule per bone with swing-twist limits,
  generated from the joint hierarchy with editable sizes and limits), switch an Animator to it on death or
  a big hit, and blend back to animation with a get-up clip. Follow-up: powered (active) ragdolls that
  track the animated pose with motors (`Ragdoll::DriveToPoseUsingMotors`) for stumbles and hit reactions.
  Add a ragdoll to the determinism batch.
- **Morph targets — M.** glTF morph targets (facial expressions, corrective shapes): blend position and
  normal deltas in the skinning shader, animate weights from glTF `weights` channels and from Lua.
- **Retargeting — M.** Play clips authored for one skeleton on another with different proportions: a
  humanoid joint map (name mapping plus rest-pose rotation correction) and translation scaled by limb
  length, so a shared clip library (e.g. Mixamo) can drive any character.
- **Spring bones — S.** Procedural secondary motion for hair, tails, ears and capes: damped springs on
  selected joint chains, applied after the animated pose, with optional sphere/capsule colliders.
- **Property animation — M.** Keyframe tracks for any component field through the `ComponentRegistry`
  JSON path (doors, platforms, light flicker, UI transitions, cutscene cameras) with the same playback
  controls as skeletal clips, so non-character motion does not need per-frame Lua.
- **Crowd performance — M.** Replace per-vertex CPU bounds with cached joint influence bounds or GPU
  culling, update distant animators at a reduced rate (animation LOD), and evaluate poses on the job
  system (see Engine internals).
- **Later: motion matching and procedural creatures — L each.** Motion matching (search a mocap database
  every few frames for the pose and future trajectory that best match the desired movement) once the state
  machine and root motion exist; procedural leg stepping for multi-legged creatures built on IK.

### Scripting

- **Script hot reload — M.** Watch `.lua` files during play; on change, reload the class table and rebind
  existing instances (keeping their fields), then call an optional `OnReload`. Also refresh the
  inspector's cached script properties when the file is saved (today they stay cached until restart).
  Saves a lot of iteration time, including for AI agents.

### Audio

- **Effects — M.** Low-pass, reverb and doppler via miniaudio nodes; `AudioSource.Doppler`.

### Game UI

- **UI follow-ups — S.** Text measuring from Lua (`UI.MeasureText`), clipping a subtree to its rect (scroll
  lists), rotation and scale of UI rects, sprite sheets (UV rects) for `UIImage`, editor handles to drag
  and resize rects in the viewport.
- **More widgets — M.** Text input fields, dropdowns and scroll views; vertical sliders; explicit navigation
  neighbours (overriding the nearest-widget rule); panels that block pointer input to the widgets below them;
  a focus ring that only shows after keyboard/gamepad use.

---

## P2 — Editor and asset pipeline

### Editor feel and visual quality

The Game view, asset thumbnails and material previews, the project settings panel and export validation
with a smoke test have shipped. Follow-ups:

- **Material assets — L.** A `.bmaterial` file type that MaterialComponent can reference, with
  content-browser thumbnails (previews cover MaterialComponent and glTF materials today).
- **Preview invalidation — S.** Refresh model and material previews when a texture they use changes, and
  release preview-only models from `AssetManager`.

### Editor workflow

- **Native file dialogs — S.** Add nativefiledialog-extended (pinned) for Open/Save/Import, keeping the
  text-box fallback for headless and Linux systems without a portal.
- **Prefab editing and overrides — L.** Open a prefab in an isolated scene, save, and propagate changes
  to instances while keeping per-instance overrides (track overridden fields in `PrefabComponent`).
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
- **Crash reporting — M.** Install crash handlers (minidumps on Windows, signal handlers with a stack
  trace on macOS/Linux), write the last log lines and a crash file next to the executable, and show a
  message box in the runtime.
- **Release pipeline — S.** A GitHub Actions workflow on tags that builds Dist on all three platforms,
  exports the samples, and attaches editor, runtime and CLI archives to a GitHub release.

---

## AI-agent workflow

The specification asks for an editor that an AI agent can fully control. These items make that faster and
more reliable.

- **More samples — M each.** A 3D platformer (character controller + camera +
  an animated character whose walk/run/jump go through the animation state machine, foot IK and root motion),
  and a small first-person scene (mouse look + raycasts + audio). Each one is built through the
  automation API and adds a ctest gameplay test, which also checks the API is sufficient.

---

## Engine internals

- **Job system — M.** A small engine-wide thread pool with work stealing, used by asset loading,
  animation and culling. Physics currently owns a private `JPH::JobSystemThreadPool`; route it through
  the shared pool (Jolt's `JobSystemWithBarrier`) so the two do not oversubscribe the CPU.
- **GPU profiler follow-ups — S.** Scene passes are timed (`GpuProfiler`, editor statistics, `render.stats`);
  the editor's own ImGui pass and the runtime blit are not. Time them too, and show the totals in the
  play-mode stats overlay (see Editor) once it exists.
- **CPU profiler integration — S.** Optional Tracy instrumentation behind `BASALT_PROFILE`, compiled out
  by default.
- **Memory tracking — S.** Track allocations per subsystem (assets, GPU buffers, Lua heap) and report them
  in the stats overlay and `render.stats`.
- **Performance benchmarks — M.** A benchmark scene with thousands of entities, many lights and physics
  bodies, run in Release on CI with frame-time thresholds to catch regressions.

---

## Platform notes

- **Wayland.** GLFW 3.4 supports Wayland; test the editor under a Wayland session on Ubuntu 24.04 and
  document any issues (window decorations, cursor capture, clipboard).
- **HDR output — L.** Use an HDR10 / scRGB swapchain where available (Windows, macOS EDR) with a
  display-mapped tonemapper.
