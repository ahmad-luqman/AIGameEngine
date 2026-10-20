# Basalt Roadmap

This file lists what Basalt does not do yet, ordered by priority, with enough detail to start work on any
item. It is based on a review of the code as of commit `670c192` (October 2026), plus ideas from comparing
Basalt with a similar AI-built engine shown in The Cherno's "Can AI Make a Game Engine?" video. Every item follows the
normal workflow in AGENTS.md: unit tests, feature-test coverage for anything reachable from Lua or the
automation API, docs, and a code review before committing.

Size estimates: **S** = a day or less, **M** = a few days, **L** = a week or more.

**Execution.** Work in milestones rather than one item at a time (`ROADMAP-SESSIONS.md` groups the open items into
single-session milestones). Run each milestone as one long autonomous session at high effort
(or with multi-agent workflows), and check quality and token cost before starting the next.

## Current state (baseline)

- All requirements of the original specification are implemented: glTF import, gizmos, PBR, IBL from
  HDRIs, soft cascaded shadows, SSAO, HDR + tonemapping, Jolt physics, Lua scripting with
  creation/destruction/prefabs, an editor, export, and a full automation API (CLI + TCP).
- CI builds and tests on Windows, Ubuntu and macOS (Debug + Release) — but without a GPU. Rendering has
  only been verified on macOS (Apple M4 Max via MoltenVK) with zero validation errors.
- Over 800 unit test cases, a feature test with over 600 Lua checks, and Tetris, Joints, Marble Run, Breakout, First
  Person, Movement Demo and Platformer samples with scripted gameplay tests.

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

- **GPU particle simulation — M.** Move CPU particle simulation to a compute pass if particle counts demand it.
- **Temporal anti-aliasing — M.** TAA (jittered projection, motion
  vectors from the depth prepass, history reprojection with neighbourhood clamping), which also
  stabilizes SSAO and specular shimmer. Builds on the post-process stack’s FXAA. MSAA is an option for
  the forward pass but complicates the SSAO prepass.
- **Reflection probes and SSR — L.** Local box-projected cubemap probes captured in the editor, then
  screen-space reflections for contact detail.
- **Decals and text in 3D — L each.**

### Animation

Skeletal playback, crossfades, two-clip blends, 1D/2D blend spaces with phase sync and velocity matching,
animation events, bone attachments, data-driven state machines and per-state root motion, layers with joint masks and
additive clips, inverse kinematics (two-bone, look-at chains, foot placement) and spring bones (`AnimationSystem`) are implemented;
physics provides character controllers (`MoveCharacter`, `IsCharacterGrounded`, ground normal) and raycasts. The items
below extend character movement, in order of impact.

- **IK and layer follow-ups — S each.** Multi-joint chains (FABRIK or CCD) for tails and tentacles; an edit-mode
  animation preview (clips, layers and IK); reusable mask assets shared between Animators; layers driven by state machine states (an
  upper-body graph); foot locking during stance phases so planted feet do not slide.
- **Spring bone follow-ups — S.** Collide chains with physics bodies (a cape against walls, hair on the ground),
  colliders per chain instead of per component, angle limits for stiff appendages, and an edit-mode preview.
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
- **Property animation — M.** Keyframe tracks for any component field through the `ComponentRegistry`
  JSON path (doors, platforms, light flicker, UI transitions, cutscene cameras) with the same playback
  controls as skeletal clips, so non-character motion does not need per-frame Lua.
- **Later: motion matching and procedural creatures — L each.** Motion matching (search a mocap database
  every few frames for the pose and future trajectory that best match the desired movement); procedural
  leg stepping for multi-legged creatures built on IK.

---

## P2 — Editor and asset pipeline

### Editor feel and visual quality

The Game view, asset thumbnails and material previews, the project settings panel and export validation
with a smoke test have shipped. Follow-ups:

- **Material assets — L.** A `.bmaterial` file type that MaterialComponent can reference, with
  content-browser thumbnails (previews cover MaterialComponent and glTF materials today).

### Editor workflow

- **Animation graph editor — M.** A visual editor for existing `.banimgraph` states, parameters and transitions, using the automation asset commands.
- **Native file dialogs — S.** Add nativefiledialog-extended (pinned) for Open/Save/Import, keeping the
  text-box fallback for headless and Linux systems without a portal.
- **Prefab editing and overrides — L.** Open a prefab in an isolated scene, save, and propagate changes
  to instances while keeping per-instance overrides (track overridden fields in `PrefabComponent`).

### Asset pipeline

KTX2/BC7 texture compression and the session-8 glTF extensions have shipped; see `Docs/AssetPipeline.md`.

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

## Platform notes

- **Wayland.** GLFW 3.4 supports Wayland; test the editor under a Wayland session on Ubuntu 24.04 and
  document any issues (window decorations, cursor capture, clipboard).
- **HDR output — L.** Use an HDR10 / scRGB swapchain where available (Windows, macOS EDR) with a
  display-mapped tonemapper.
