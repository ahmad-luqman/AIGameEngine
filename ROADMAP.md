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
`Docs/TestedHardware.md` (`--offscreen`, `--gpu`, golden comparison, interactive checklist). Done for an
NVIDIA laptop GPU and Intel integrated graphics on Windows (steps 1–3). Remaining: the interactive step on
that laptop, an AMD GPU (e.g. AWS g4ad), a desktop NVIDIA GPU, and native Linux for all of them. Optionally
add an on-demand self-hosted GPU runner later.

### SSAO blur taps land on texel boundaries — S
`SSAOBlur.frag` point-samples at `i * 1.5` texels from the centre (the step is one target texel,
`SceneRenderer.cpp`), so odd taps sit exactly between two texels and the hardware's rounding picks one.
Results then depend on the GPU and the target size: on Windows `FeatureTestDynamicMatchesScaled` and
`FeatureTestDynamicSSAOMatchesScaled` fail (NVIDIA RTX 4060: one row, max delta 5; Intel UHD 770: 2–5% of
pixels, max delta 17), while MoltenVK and lavapipe happen to round consistently. Fix: take the taps with
`texelFetch` at whole-texel offsets (or sample linearly where the 1.5 offset is meant to merge two texels),
then refresh the MoltenVK goldens.

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
animation events, bone attachments, data-driven state machines and per-state root motion, layers with joint masks and
additive clips, inverse kinematics (two-bone, look-at chains, foot placement), spring bones, morph targets and retargeted
clip libraries (`AnimationSystem`) are implemented;
physics provides character controllers (`MoveCharacter`, `IsCharacterGrounded`, ground normal) and raycasts. The items
below extend character movement, in order of impact.

- **IK and layer follow-ups — S each.** Multi-joint chains (FABRIK or CCD) for tails and tentacles; an edit-mode
  animation preview (clips, layers and IK); reusable mask assets shared between Animators; layers driven by state machine states (an
  upper-body graph); foot locking during stance phases so planted feet do not slide.
- **Spring bone follow-ups — S.** Collide chains with physics bodies (a cape against walls, hair on the ground),
  colliders per chain instead of per component, angle limits for stiff appendages, and an edit-mode preview.
- **Powered ragdolls — M.** Track the animated pose with motors (`JPH::Ragdoll::DriveToPoseUsingMotors`)
  for active stumbles and hit reactions.
- **Retargeting follow-ups — S.** Rest-pose matching for libraries authored in a different rest style (an A-pose clip
  on a T-pose character) through a reference pose, and a reusable joint map asset shared by Animators.
- **Property animation follow-ups — S.** Stepped boolean/integer/string/enum tracks and nested registry
  field paths, stable Euler serialization at exact gimbal lock, and physics configuration tracks that preserve contacts; current authoring supports scalar/vector numeric fields on the same entity.
- **Later: motion matching and procedural creatures — L each.** Motion matching (search a mocap database
  every few frames for the pose and future trajectory that best match the desired movement); procedural
  leg stepping for multi-legged creatures built on IK.

---

## P2 — Editor and asset pipeline

### Editor feel and visual quality

The Game view, asset thumbnails and material previews, the project settings panel, export validation with a
smoke test and material assets (`.bmaterial` files with lossless thumbnails and a Material Editor) have shipped.

### Editor workflow

Native file dialogs and prefab editing with per-instance overrides have shipped.

- **Nested prefabs and variants — M.** Prefabs whose entities are instances of other prefabs (today they are saved
  unpacked), and variants that inherit a base prefab and store only their differences, merging through the same
  three-way path.


---

## P3 — Shipping and distribution

- **Windows code signing — S.** Sign the exported executable and the tools with `signtool` when a certificate is
  configured (the release pipeline has secrets for macOS notarization only), so SmartScreen does not warn.
- **Signed and universal tools on macOS — S.** The release signs and notarizes only the sample bundles: sign the
  editor, runtime and CLI (and their bundled Vulkan libraries) and notarize the tools archive too, and build universal
  (arm64 + x86-64) binaries for Intel Macs.
- **Unicode command-line arguments and asset keys on Windows — S.** The CLI and runtime read narrow `argv`, so
  non-ASCII paths in arguments (`--project`, `project.export` output) do not survive on Windows; read `GetCommandLineW`
  instead. Likewise `Project::ResolvePath` receives asset keys as `std::string` converted through the ANSI code page, so
  non-ASCII asset names do not load on Windows; take the UTF-8 keys through `FileSystem::PathFromUtf8`.
- **Archive follow-ups — S.** Stream stored (uncompressed) audio straight from `Assets.bpak` instead of reading clips
  into memory whole, compress large entries on the job system during export, and leave out originals that only a
  compressed `.ktx2` replacement references.
- **Crash report follow-ups — S.** Symbolize Linux frames (ship symbols or call `addr2line` at report time), report
  stack overflows on threads other than the one that installed the handlers (alternate stacks for job threads), write
  Windows minidumps from a watchdog thread, show fatal startup errors (a broken archive) in a message box in the
  windowed runtime, and offer to send or open the report from the crash dialog.

---

## Platform notes

- **Wayland.** GLFW 3.4 supports Wayland; test the editor under a Wayland session on Ubuntu 24.04 and
  document any issues (window decorations, cursor capture, clipboard).
- **HDR output — L.** Use an HDR10 / scRGB swapchain where available (Windows, macOS EDR) with a
  display-mapped tonemapper.
