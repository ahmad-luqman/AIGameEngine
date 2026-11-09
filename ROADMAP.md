# Basalt Roadmap

This file states what Basalt is for and lists what it does not do yet, ordered by priority, with enough detail to start
work on any item. It follows the maturity assessment in `Docs/MaturityAssessment.md` (7 November 2026, `main` @
`4f823ab`) and the defect fixes that landed after it (PR #74). The scope and the order of the top tiers were settled on
8 November 2026. Every item follows the normal workflow in AGENTS.md: unit tests, feature-test coverage for anything
reachable from Lua or the automation API, docs, and a code review before committing.

Size estimates: **S** = a day or less, **M** = a few days, **L** = a week or more.

**Execution.** Work in milestones rather than one item at a time (`ROADMAP-SESSIONS.md` groups the open items into
single-session milestones). Run each milestone as one long autonomous session at high effort
(or with multi-agent workflows), and check quality and token cost before starting the next.

## Scope

**Basalt is a desktop 3D engine built for agent-driven development.** This statement is the authoritative one; the
README and AGENTS.md lead with it. Its parts mean:

- **Desktop:** games for Windows, macOS and Linux, authored on the same three. Desktop is where every roadmap item
  lands first.
- **3D:** the renderer, physics, animation and tools are built around 3D scenes.
- **Built for agent-driven development:** everything an editor user can do, an agent can do through the automation API,
  headless, deterministically and with checks it can assert on. A feature is not done until an agent can author and test
  it without the GUI.

What that settles:

| Area | Decision |
|------|----------|
| Networking (multiplayer) | **In scope, first priority** (P0 below). Desktop games are often multiplayer, and the headless runtime and deterministic physics are a good base for it. |
| Mobile (iOS, Android) | **Later**, after desktop is proven by a release and a shipped game. Needs MoltenVK on iOS, touch input, lifecycle handling and packaging. |
| Web (WebGPU, WebGL 2) | **Later**, on the same condition. Needs a renderer backend other than Vulkan and a Wasm build. |
| XR | Not planned, not ruled out. |
| 2D pipeline (sprites, tilemaps, 2D physics) | Not planned, not ruled out. 2D-style games can be made from 3D scenes today. |
| Consoles (PlayStation, Xbox, Switch) | **Non-goal.** Platform SDKs are under NDA and cannot live in an open MIT repository, and a one-developer project cannot carry the certification work. Even Godot reaches consoles only through third parties. |

**Status words.** Basalt is early alpha: version 0.1.0, no tagged release, no external users and no shipped game. The
code is held to production standards (the tests, fuzzing, sanitizers and pinned determinism in AGENTS.md); "production
ready" is a claim for after P1.

## Current state (baseline)

- All requirements of the original specification are implemented: glTF import, gizmos, PBR, IBL from
  HDRIs, soft cascaded shadows, SSAO, HDR + tonemapping, Jolt physics, Lua scripting with
  creation/destruction/prefabs, an editor, export, and a full automation API (CLI + TCP + MCP).
- CI builds and tests on Windows and macOS (Debug + Release) and Ubuntu (Debug under ASan+UBSan; Release with the GPU
  tests on lavapipe, Mesa's CPU Vulkan driver), plus clang-tidy, coverage and fuzzing. On real GPUs, rendering is verified
  with zero validation errors on an Apple M4 Max (MoltenVK), an NVIDIA RTX 4060 Laptop and Intel UHD 770 graphics on
  Windows (`Docs/TestedHardware.md`).
- Over 1,600 unit test cases, 16 fuzz targets, a feature test with 411 `Check` calls, and Tetris, Joints, Marble Run,
  Breakout, First Person, Movement Demo and Platformer samples with scripted gameplay tests.
- The assessment's five verified defects were fixed in PR #74: physics reports overflows and sizes its temporary memory
  from its capacity, the editor and CLI stop scripts that never return, the editor server closes connections that do not
  open with JSON, and `project.validate` refuses clips the engine cannot decode. Their residuals are in P1.
- The largest gaps, by the assessment: no networking, no navigation, no occlusion culling, global illumination, fog,
  decals, terrain or custom shaders, no Lua debugger, a CPU cost that grows linearly with entity count, no GPU memory
  management, and no release, tutorial or real game.

---

## P0 — Networking

Multiplayer is the first new system. Two sessions build the layer; a sample proves it.

### Networking I: transport, connections and replication — L
- **Transport.** UDP with reliable and unreliable channels, pinned in `cmake/Dependencies.cmake`. Choose it in the
  session (candidates: ENet, yojimbo, GameNetworkingSockets) by: builds on all three CI platforms without system
  packages, encryption or a path to it, and a small surface to fuzz. Every packet is untrusted input: parse with
  bounds-checked readers and add a `Tests/Fuzz` target (`FuzzNetPacket`).
- **Model.** Client-server with an authoritative host: a listen server inside a playing game, or `BasaltRuntime
  --headless` as a dedicated server (it already runs the full game loop without a GPU). Connections, timeouts,
  disconnect reasons and a protocol version check.
- **Replication.** A component (via the `basalt-add-component` skill) that marks an entity as networked, with owner and
  authority. Replicated fields are declared through `ComponentRegistry`, keeping the one data path, but sent in a compact
  binary encoding with snapshot deltas, not as JSON. Spawns and despawns of networked entities and prefabs, entity
  identity across peers (runtime UUIDs are already seeded per session), and a per-connection bandwidth budget.
- **Tests.** Unit tests run several peers in one process over a simulated link with fixed latency, loss and reordering
  driven by the fixed timestep, never the wall clock. Servers in integration tests take a free port.

### Networking II: gameplay API, prediction and tools — L
- **Lua.** A `Network` API: host, connect and disconnect; connection events; reliable and unreliable messages and RPCs
  that target the server, an owner or all clients; authority queries. Update `Docs/ScriptingAPI.md` and
  `Docs/basalt.d.lua`, and add `Check`s to the feature test.
- **Prediction and interpolation.** Client-side prediction with reconciliation for character controllers, interpolation
  of remote entities between snapshots (this also needs the fixed-step interpolation listed in P2), and lag compensation
  for raycasts.
- **Automation.** `net.*` commands to start a host, connect clients, inspect connections and replication state, and
  inject latency and loss, so an agent can test a multiplayer game headless. `scene.hash` per peer to assert that peers
  agree.
- **Later, not in this session:** interest management for large worlds; deterministic lockstep or rollback, which needs
  the whole simulation to be deterministic (today only physics is pinned: IK uses libm, and Lua is not checked).

### Multiplayer sample — M
A small arena or co-op game built through the automation API, with a ctest that runs a server and two clients headless
and asserts on the outcome. It goes in `Samples/` like the others.

---

## P1 — Release and proof

These close the gap between "works on the developer's machine" and an engine other people can use, and test the
agent claim that is Basalt's reason to exist.

### Release v0.1.0 — M
- Binary downloads of the editor, runtime and CLI from the existing release pipeline (signed once its certificates are
  set up; universal on macOS), on a `v0.1.0` tag.
- A getting-started tutorial (zero to a playable game, by hand and by agent), a starter project template, and a default
  scene with ground, light and camera instead of an empty void.
- A CHANGELOG, a stability policy (what may break before 1.0), CONTRIBUTING and SECURITY files, and branch protection on
  `main`.
- Third-party notices in exported games (the licenses of the libraries an export links), so exports meet those
  licenses.
- The name collision with the unrelated "Basalt" from The Cherno's video: decide before the release whether the name
  stays.

### Agent sight and parity — M
- `render.*` in the headless CLI (an offscreen device when a GPU is present), so an agent sees the frame it stepped to
  without the editor, and images returned over MCP, not only paths.
- The four GUI-only actions in the API: Simulate, Pause, Debug View, and folder create/delete. AGENTS.md promises the
  GUI cannot do anything the API cannot.
- A token for the editor's automation server (written to the user data directory, required in the first frame), as
  PR #74 closed non-JSON connections but added no authentication.

### Agent benchmark — M
Run a CEGame-Bench-style task set (game tasks scored by automated checks) through Basalt, and the same tasks through
Godot and Unity, with the same agent. Publish the tasks, the harness and the results. This is the only way to know
whether the agent-first design works; no agent outside this repository has used Basalt yet.

### One real game — L (2–4 weeks)
About 30 minutes of content with real art and characters, built mostly by an agent, exported and packaged on all three
operating systems. It is the cheapest way to find what unit tests cannot, and the first real content for the
animation stack. Log every engine problem it hits as a roadmap item.

### Real-GPU smoke tests on Windows and Linux — S (manual) / M (automated)
Lavapipe does not catch driver-specific bugs. The procedure and results table are in `Docs/TestedHardware.md`
(`--offscreen`, `--gpu`, golden comparison, interactive checklist). Done for an NVIDIA laptop GPU and Intel integrated
graphics on Windows (steps 1–3). Remaining: the interactive step on that laptop, an AMD GPU (e.g. AWS g4ad), a desktop
NVIDIA GPU, and native Linux for all of them; also the Windows crash paths from PR #69 (deadline thread, watchdog
timeouts, Open Report), which are only compile-tested. Optionally add an on-demand self-hosted GPU runner later.

### Defect residuals
- **Scripts that never return in exported games — S.** The runtime turns the instruction limit off on purpose (a count hook
  slows Lua up to 2.7 times; see AGENTS.md, "Stopping scripts"), so a shipped game still hangs on such a script. Decide
  whether a cheap watchdog (a hook installed only after a frame overruns) is worth it, and document the answer.
- **Ogg Vorbis and Opus playback — M.** Clips are WAV, FLAC or MP3 (and FLAC inside Ogg); `project.validate` refuses other
  clips an `AudioSource` names, and a script's `Audio.PlayOneShot` logs them at runtime. Use miniaudio's libvorbis and
  libopus backends (`extras/decoders`), which need those libraries pinned, and fuzz them through `FuzzAudio`. Not its
  bundled `stb_vorbis.c` 1.22: a 666-byte file stalled it for over a minute in `start_decoder`, and CVE-2023-45675, a
  heap overflow, is in the same function.

---

## P2 — Scale

The assessment measured about 50K static entities in a 60 FPS budget on an M4 Max, with the CPU at about 90% of the
frame and growing linearly. Fix that before adding rendering features.

- **Realistic benchmark scene — S.** Many meshes and materials, several rigs, real resolution, lights past the cluster
  budget; frame-time gates on CI next to `Tests/Benchmarks`. Profile it first: the audit's causes are inferred.
- **Cached transforms and per-entity costs — M.** Cache world transforms with dirty propagation, make mass destruction
  linear (it is O(n²)), sync only physics bodies that moved (it is O(all bodies) per step), and add fixed-step
  interpolation of rendered transforms.
- **GPU memory — M.** A suballocator (one Vulkan allocation per resource today), a budget with reporting in
  `memory.stats`, and eviction; `Scene.Load` should release what the next scene does not use (`AssetManager` keeps all of
  it).
- **Occlusion culling — L.** GPU two-phase culling against a hierarchical depth buffer, or CPU culling against baked
  occluders; pick after the benchmark shows where the cost is.
- **Lights — S.** Above 256 point lights the rest are dropped; select by importance (distance, intensity, screen size)
  instead of order.

---

## P3 — Debugging and editor workflow

- **Lua stack traces — S.** Script errors carry file and line but no call stack; add `traceback` to the error path for
  every protected call, in the log, `events.*` and the editor console.
- **Lua profiler — M.** Per-script and per-callback time in the stats overlay and an automation command; Tracy zones
  when `BASALT_PROFILE` is on.
- **Lua debugger — L.** A Debug Adapter Protocol server in the editor (breakpoints, stepping, locals), usable from VS Code
  and by agents through the same protocol.
- **Editor search, copy/paste, file drop, rename and move — M.** Hierarchy and content-browser search; entity copy and
  paste through the clipboard as registry JSON; OS file drop into the content browser (the event is unread today); GUI
  rename and move through `asset.rename`/`asset.move`.
- **Curve and dope-sheet editor — M.** For property animation and clip events; nodes in the graph editor off the fixed
  grid.

---

## P4 — Rendering

Basalt's core (clustered PBR, cascaded shadows, SSAO, TAA, bloom) matches its peers; everything above it is missing.
Custom shaders come first because teams cannot work around their absence.

- **Custom shaders — L.** User materials with their own GLSL fragment code against a documented interface (inputs,
  lighting helpers, the binding convention), compiled at import with errors reported through `project.validate`;
  embedded shaders stay for the engine's own passes.
- **Fog and volumetrics — M.** Height fog first, then froxel volumetric fog lit by the clustered lights and shadows.
- **Global illumination — L.** One technique, chosen for MoltenVK (no ray tracing): baked irradiance probes or DDGI-style
  probes updated by compute.
- **Reflection probes and SSR — L.** Local box-projected cubemap probes captured in the editor, then screen-space
  reflections for contact detail.
- **Decals — L.** Clustered decals with albedo, normal and roughness.
- **Text in 3D — L.** World-space text through the UI fonts.
- **GPU particle simulation — M.** Move CPU particle simulation to a compute pass if particle counts demand it; particle
  modules, collision and trails with it.
- **HDR output — L.** Use an HDR10 / scRGB swapchain where available (Windows, macOS EDR) with a display-mapped
  tonemapper.

---

## P5 — Gameplay systems and content

- **Navigation — L.** Recast/Detour behind a component: navmesh baking from static colliders in the editor and through
  automation, agents with avoidance, path queries from Lua, and debug drawing.
- **Terrain — L.** Heightfield terrain with a Jolt heightfield collider, splat-mapped materials and foliage instancing;
  large coordinates (camera-relative rendering, then origin shifting) with it.
- **Game UI text — M.** Text shaping (HarfBuzz), fallback fonts, right-to-left text and IME input, then string tables
  for localization.
- **Audio voices — M.** A voice cap with priorities, a limiter on the master bus, and occlusion by raycast.
- **Asset formats — M.** Per-asset import settings in `.meta` files; FBX through a converter step.
- **Vehicles and cloth — M each.** Jolt has both; expose them as components.

---

## P6 — Feature follow-ups

### Physics

- **Soft six-DOF rotation limits — S.** Jolt 5.6 softens only six-DOF translation limits. When it adds
  springs to rotation limits, apply `LimitSpringFrequency` to them too.

### Animation

Skeletal playback, crossfades, two-clip blends, 1D/2D blend spaces with phase sync and velocity matching,
animation events, bone attachments, data-driven state machines and per-state root motion, layers with joint masks,
shared mask assets and a mask editor, additive clips and their own state machines with events and private parameters,
inverse kinematics (two-bone, look-at and FABRIK chains with joint limits, foot placement and foot locking, in the edit
preview too), spring bones (angle limits, per-chain colliders, physics-body collisions with layers,
continuous collision and edit-mode static bodies), powered ragdolls (per-bone strengths, partial rigs on a kinematic root,
active balance), morph targets, retargeted clip libraries (shared joint maps with an editor, reference poses from clips or per-joint offsets), property
animation of numeric and discrete fields (nested in lists too) and an edit-mode preview (`AnimationSystem`) are implemented;
physics provides character controllers (`MoveCharacter`, `IsCharacterGrounded`, ground normal) and raycasts. The items
below extend character movement, in order of impact.

- **IK and retargeting follow-ups III — S.** Cone and twist gizmos in the viewport, joint limits for TwoBone chains and the end
  joint's target rotation, and a live skeleton preview of the Animation Mask and Joint Map windows (their offsets and
  weights are read from Animators in the scene view today).
- **Spring bone follow-ups III — S.** Sweeping whole bones as well as their points, and kinematic bodies in the edit preview
  where they stay at their authored place.
- **Powered ragdoll follow-ups II — M.** Balance that steps (replanting feet toward the center of mass after a push), and
  several kinematic roots (both legs walking and a free spine between them).
- **Clip compression — M.** Keyframe reduction and quantized tracks for large clip libraries.
- **Later: motion matching and procedural creatures — L each.** Motion matching (search a mocap database
  every few frames for the pose and future trajectory that best match the desired movement); procedural
  leg stepping for multi-legged creatures built on IK.

### Shipping

- **Cross-platform export — M.** Export for another desktop OS than the host's, from runtime binaries the release
  publishes.

---

## Later — other platforms

Not before desktop is proven by the release and a shipped game (see Scope).

- **Mobile — L.** iOS (MoltenVK or a Metal path) and Android (Vulkan): touch input, lifecycle events, packaging and
  signing, and a reduced-cost render path.
- **Web — L.** A Wasm build and a WebGPU renderer backend (nvrhi has none), asset streaming over HTTP.

## Platform notes

- **Wayland.** GLFW 3.4 supports Wayland; test the editor under a Wayland session on Ubuntu 24.04 and
  document any issues (window decorations, cursor capture, clipboard).
