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
- CI builds and tests on Windows and macOS (Debug + Release) and Ubuntu (Debug under ASan+UBSan; Release with the GPU
  tests on lavapipe, Mesa's CPU Vulkan driver). On a real GPU, rendering has only been verified on macOS (Apple M4 Max
  via MoltenVK) with zero validation errors.
- Over 800 unit test cases, a feature test with over 600 Lua checks, and Tetris, Joints, Marble Run, Breakout, First
  Person, Movement Demo and Platformer samples with scripted gameplay tests.

---

## P0 — Verification and robustness

These close the gap between "works on the developer's machine" and "production-grade". Do them before
adding features.

### Real-GPU smoke tests on Windows and Linux — S (manual) / M (automated)
Lavapipe does not catch driver-specific bugs. The procedure and results table are in
`Docs/TestedHardware.md` (`--offscreen`, `--gpu`, golden comparison, interactive checklist). Done for an
NVIDIA laptop GPU and Intel integrated graphics on Windows (steps 1–3). Remaining: the interactive step on
that laptop, an AMD GPU (e.g. AWS g4ad), a desktop NVIDIA GPU, and native Linux for all of them. Optionally
add an on-demand self-hosted GPU runner later.

---

## P1 — Core engine features

The features most games need, roughly in order of impact.

### Physics

- **Soft six-DOF rotation limits — S.** Jolt 5.6 softens only six-DOF translation limits. When it adds
  springs to rotation limits, apply `LimitSpringFrequency` to them too.

### Rendering

- **GPU particle simulation — M.** Move CPU particle simulation to a compute pass if particle counts demand it.
- **Reflection probes and SSR — L.** Local box-projected cubemap probes captured in the editor, then
  screen-space reflections for contact detail.
- **Decals and text in 3D — L each.**

### Animation

Skeletal playback, crossfades, two-clip blends, 1D/2D blend spaces with phase sync and velocity matching,
animation events, bone attachments, data-driven state machines and per-state root motion, layers with joint masks,
shared mask assets, additive clips and their own state machines, inverse kinematics (two-bone, look-at and FABRIK chains,
foot placement and foot locking), spring bones (angle limits, per-chain colliders, physics-body collisions), powered
ragdolls, morph targets, retargeted clip libraries (shared joint maps, reference poses for other rest styles), property
animation of numeric and discrete fields (nested in lists too) and an edit-mode preview (`AnimationSystem`) are implemented;
physics provides character controllers (`MoveCharacter`, `IsCharacterGrounded`, ground normal) and raycasts. The items
below extend character movement, in order of impact.

- **IK and layer follow-ups II — S each.** Joint limits for FABRIK chains (cones and twist ranges), a mask editor in the
  editor (masks are authored as `.banimmask` JSON through `asset.write` today), layer graph events and per-layer
  parameters that the base graph does not share, and edit-mode ground placement against static colliders.
- **Spring bone follow-ups II — S.** Physics layer filtering for `CollidePhysics` chains (today they collide with every
  layer), continuous collision so fast points cannot pass through thin bodies within one substep, and edit-mode
  collisions against static physics bodies.
- **Powered ragdoll follow-ups — M.** Partial ragdolls whose root follows the animation kinematically (an upper body
  reacting to hits while the legs keep walking and the character controller stays), per-bone motor strengths, and
  active balance that keeps a powered ragdoll upright.
- **Retargeting follow-ups II — S.** A joint map editor in the editor (`.bjointmap` files are authored as JSON through
  `asset.write` today) and reference poses built from per-joint rotation offsets, for libraries without a frame in the
  model's rest style.
- **Property animation follow-ups — S.** Stable Euler serialization at exact gimbal lock, physics configuration tracks
  that preserve contacts, asset reference tracks (texture flipbooks), and tracks on other entities; current authoring
  covers numeric, integer, boolean, string and enum fields of the same entity, including fields inside list elements.
- **Later: motion matching and procedural creatures — L each.** Motion matching (search a mocap database
  every few frames for the pose and future trajectory that best match the desired movement); procedural
  leg stepping for multi-legged creatures built on IK.

---

## P2 — Editor and asset pipeline

### Editor feel and visual quality

The Game view, asset thumbnails and material previews, the project settings panel, export validation with a
smoke test and material assets (`.bmaterial` files with lossless thumbnails and a Material Editor) have shipped.

### Editor workflow

Native file dialogs, prefab editing with per-instance overrides, nested prefabs and variants have shipped.


---

## P3 — Shipping and distribution

- **Windows code signing — S.** Sign the exported executable and the tools with `signtool` when a certificate is
  configured (the release pipeline has secrets for macOS notarization only), so SmartScreen does not warn.
- **Signed and universal tools on macOS — S.** The release signs and notarizes only the sample bundles: sign the
  editor, runtime and CLI (and their bundled Vulkan libraries) and notarize the tools archive too, and build universal
  (arm64 + x86-64) binaries for Intel Macs.
- **Unicode follow-ups — S.** About 75 older `std::error_code::message()` calls put ANSI text into UTF-8 messages on
  Windows: switch them to `FileSystem::DescribeError`. Directory scans (asset registry, file watcher, exporter, content
  browser) turn NTFS names with unpaired surrogates into keys with U+FFFD that no longer name the file: skip and report
  such entries.

---

## Platform notes

- **Wayland.** GLFW 3.4 supports Wayland; test the editor under a Wayland session on Ubuntu 24.04 and
  document any issues (window decorations, cursor capture, clipboard).
- **HDR output — L.** Use an HDR10 / scRGB swapchain where available (Windows, macOS EDR) with a
  display-mapped tonemapper.
