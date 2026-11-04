# Roadmap Sessions

The `ROADMAP.md` items grouped into single-session milestones, sorted in order of implementation, with completed
sessions retained for reference. The order follows the roadmap's priorities (P1 → P2 → P3) and is adjusted so that
work a later session depends on comes first.
Updated on 4 November 2026: sessions 1–21 have shipped, and sessions 22–25 are planned.

Size estimates: **S** = a day or less, **M** = a few days, **L** = a week or more.

| # | Session | Items (size) | Why here |
|---|---------|--------------|----------|
| 1 | **Character movement** ✅ | Inverse kinematics: two-bone legs and arms, look-at/aim chains, foot placement (M); animation layers, per-joint masks, additive clips (M) | Shipped. |
| 2 | **3D platformer sample** ✅ | Platformer built through the automation API with a ctest gameplay test (M); spring bones on its hair or cape (S) | Shipped. |
| 3 | **Diagnostics** ✅ | Play-mode stats overlay (S); timing for the editor's ImGui pass and the runtime blit (S); benchmark scene with frame-time thresholds on CI (M) | Shipped: overlay, host GPU timers, Release CI frame-time gates and the pre-throughput baseline (`Tests/Benchmarks`). |
| 4 | **Throughput** ✅ | GPU instancing and draw batching (M); crowd performance: cached joint bounds, animation LOD, poses evaluated on the job system (M) | Shipped: opaque GPU instancing across depth/ID, lighting and shadow passes; cached joint bounds, opt-in distance animation LOD and pose jobs. Session 3's fixed benchmark measures the gains. |
| 5 | **Game UI** ✅ | UI follow-ups: `UI.MeasureText`, clipping, rect rotation/scale, sprite sheets, viewport handles (S); more widgets: text input, dropdown, scroll view, vertical slider, explicit navigation, input-blocking panels, focus ring (M) | Shipped in [PR #41](https://github.com/ahmad-luqman/AIGameEngine/pull/41): shared clipped/transformed layouts, menu widgets and viewport handles with snapping, undo and cancellation. |
| 6 | **Effects polish** ✅ | Particle follow-ups: soft/lit particles, rotation, random birth size/color, multi-key curves and edit preview (M); one-shot audio effects, reverb zones, high-pass/echo/distortion (S) | Shipped in [PR #42](https://github.com/ahmad-luqman/AIGameEngine/pull/42). |
| 7 | **Asset pipeline I** ✅ | Stable asset IDs with `.meta` UUIDs (M); mesh optimization and LODs with meshoptimizer (S) | Implemented: project-local `.meta` UUIDs, scene/prefab/start-scene identity references, move/rename commands, meshoptimizer import and opt-in static mesh LODs. |
| 8 | **Asset pipeline II** ✅ | Texture compression to KTX2/BC7 (M); glTF extensions (M) | Shipped in [PR #44](https://github.com/ahmad-luqman/AIGameEngine/pull/44): KTX2/BC7 imports and the six planned glTF extensions. |
| 9 | **Material assets** ✅ | `.bmaterial` files with thumbnails (L) | Shipped in [PR #45](https://github.com/ahmad-luqman/AIGameEngine/pull/45): `.bmaterial` files with the full material model and UUID references, `Model.glb#material/N` references, `material.create`/`material.extract`, lossless sphere thumbnails and swatches, and a Material Editor. |
| 10 | **Animation content** ✅ | Morph targets (M); retargeting, e.g. sharing a Mixamo library across characters (M) | Shipped in [PR #46](https://github.com/ahmad-luqman/AIGameEngine/pull/46): glTF morph targets blended in the vertex shader and animated by clips, layers and Lua, and `Animator.ClipLibraries` retargeted onto humanoid skeletons with recognised joint names (or explicit joint pairs). |
| 11 | **TAA** ✅ ([PR #47](https://github.com/ahmad-luqman/AIGameEngine/pull/47)) | Temporal anti-aliasing (M) | Needs previous-frame data for instanced draws (session 4) and for skinned and morphed meshes (session 10). |
| 12 | **Animation authoring** ✅ | Animation graph editor for `.banimgraph` (M); property animation through the `ComponentRegistry` (M) | Shipped in [PR #48](https://github.com/ahmad-luqman/AIGameEngine/pull/48): graph state/edge authoring through asset.write; numeric property timelines, key capture, scrubbing and runtime controls through ComponentRegistry. |
| 13 | **Ragdolls** ✅ | Passive skeleton ragdolls, joint overrides, scripted activation and get-up recovery (M). Powered ragdolls remain a follow-up. | Shipped in [PR #49](https://github.com/ahmad-luqman/AIGameEngine/pull/49), with physics determinism and recovery coverage. |
| 14 | **Prefabs** ✅ | Prefab editing and per-instance overrides (L); native file dialogs (S) | Shipped in [PR #50](https://github.com/ahmad-luqman/AIGameEngine/pull/50): isolated prefab editing, three-way merges that keep per-instance overrides (`prefab.overrides/apply/revert/refresh`), merges on prefab writes and scene open, and nativefiledialog-extended pickers. Nested prefabs and variants followed in session 19. |
| 15 | **Shipping I** ✅ | Project icon, version and company (S); platform packaging for `.app`, `.exe` and AppImage (M); tag-triggered release pipeline (S) | Shipped in [PR #52](https://github.com/ahmad-luqman/AIGameEngine/pull/52): `Version`/`Company`/`Icon` project settings, `project.export {"package": true}` (.app with signing and notarization script, Windows icon and version resources, AppImage), and `release.yml` publishing Dist tools and packaged samples on `v*` tags. Windows code signing, signed macOS tools and universal binaries remain follow-ups. |
| 16 | **Shipping II** ✅ | Packed `.bpak` archive (M); crash reporting (M) | Shipped in [PR #53](https://github.com/ahmad-luqman/AIGameEngine/pull/53): `project.export {"archive": true}` packs Assets into a zstd `Assets.bpak` whose index carries the asset UUIDs (sessions 7 and 8), mounted by the runtime under every runtime asset reader; crash reports with stack traces, the last log lines and Windows minidumps in the user data directory, plus a message box in the windowed runtime. |
| 17 | **Character animation I** ✅ | Powered ragdolls that follow the animated pose with motors (M); IK and layer follow-ups: multi-joint chains, edit-mode preview, reusable masks, state-driven layers, foot locking (S) | Shipped in [PR #66](https://github.com/ahmad-luqman/AIGameEngine/pull/66): ragdoll motor strength, frequency, damping and torque with scripted fades; Fabrik chains, FollowTarget targets and FootLock; `.banimmask` mask assets; StateMachine layers sharing the Animator's parameters; and an edit-mode Animator preview. |
| 18 | **Character animation II** ✅ | Spring bone follow-ups: collisions, per-chain colliders, angle limits, preview (S); retargeting follow-ups: rest-pose matching, shared joint maps (S); property animation follow-ups: stepped and nested tracks (S) | Shipped in [PR #67](https://github.com/ahmad-luqman/AIGameEngine/pull/67): spring chain `MaxAngle`, named per-chain colliders, `CollidePhysics` against physics bodies and spring bones in the edit preview; `.bjointmap` joint maps shared by clip libraries and reference clips for skeletons in other rest styles; integer, boolean, string and enum property tracks that always step, and track paths into list elements (`Layers[Upper].Weight`). |
| 19 | **Prefabs II** ✅ | Nested prefabs and variants (M) | Shipped in [PR #68](https://github.com/ahmad-luqman/AIGameEngine/pull/68): prefab files hold nested instances and variants as records with only their overrides, expanded on read into the flat records that the three-way merge from session 14 already handles, so an inner prefab's edit reaches every tracked instance holding it (in other scenes when they open) without rewriting files. `prefab.variant`, `prefab.info`, `prefab.create {"variant": true}`, tracked nested instances in `prefab.open`, cycle and size limits, and a `FuzzPrefab` target. |
| 20 | **Shipping III** ✅ | Crash report follow-ups: Linux symbolization, alternate stacks for job threads, watchdog minidumps, startup error message boxes, a send/open action in the crash dialog (S); archive follow-ups: stream stored audio from `Assets.bpak`, compress on the job system, leave out originals replaced by `.ktx2` (S) | Shipped in [PR #69](https://github.com/ahmad-luqman/AIGameEngine/pull/69): stored clips of 1 MiB or more stream from `Assets.bpak`; archive entries compress in parallel on the job system with byte-identical output; archived exports leave out images replaced by a `.ktx2` of the same name that nothing mentions (`keepOriginals` keeps them); crash reports gain `addr2line` source lines on Linux, a `ThreadStack` for engine threads, watchdog minidumps on Windows and an Open Report action; and the windowed runtime shows failed starts in a message box. Sending reports was left out because there is nowhere to send them. |
| 21 | **Unicode follow-ups** ✅ | Switch the roughly 75 older `std::error_code::message()` calls to `FileSystem::DescribeError`; skip and report NTFS names with unpaired surrogates in directory scans (asset registry, file watcher, exporter, content browser) (S) | Shipped in [PR #70](https://github.com/ahmad-luqman/AIGameEngine/pull/70): every system error reaches messages as UTF-8, with a test that fails on a raw call; scans skip names no key can hold (also names that are not UTF-8 off Windows) and report them in `project.validate`, the log, command `warnings`, archived exports and the editor. |
| 22 | **Character animation III** | Powered ragdoll follow-ups: partial ragdolls whose root follows the animation, per-bone motor strengths, active balance (M); spring bone follow-ups II: physics layer filtering, continuous collision, edit-mode collisions against static bodies (S) | The highest-impact P1 work left. Both items are physics-side follow-ups to sessions 17 and 18, and they need nothing outside the repository. |
| 23 | **Animation authoring II** | IK and layer follow-ups II: FABRIK joint limits, a mask editor, layer graph events and per-layer parameters, edit-mode ground placement (S each); retargeting follow-ups II: a joint map editor, reference poses from per-joint rotation offsets (S) | Mostly editor UI for the `.banimmask` and `.bjointmap` assets from sessions 17 and 18, plus small solver additions. |
| 24 | **Property animation III** | Stable Euler serialization at exact gimbal lock, physics configuration tracks that preserve contacts, asset reference tracks (texture flipbooks), tracks on other entities (S) | Can join session 23 if that session turns out light. |
| 25 | **Signing** | Windows code signing with `signtool` (S); signed and notarized macOS tools and universal (arm64 + x86-64) binaries (S) | Blocked until a Windows certificate and the CI secrets are available. |

## Status (4 November 2026)

- **Shipped outside the sessions:**
  - The fix for glTF primitives that share index accessors ([PR #64](https://github.com/ahmad-luqman/AIGameEngine/pull/64)).
  - UTF-8 command-line arguments and paths on Windows ([PR #65](https://github.com/ahmad-luqman/AIGameEngine/pull/65)).
- **Shipped in sessions:** sessions 17–21 ([PR #66](https://github.com/ahmad-luqman/AIGameEngine/pull/66),
  [#67](https://github.com/ahmad-luqman/AIGameEngine/pull/67), [#68](https://github.com/ahmad-luqman/AIGameEngine/pull/68),
  [#69](https://github.com/ahmad-luqman/AIGameEngine/pull/69), [#70](https://github.com/ahmad-luqman/AIGameEngine/pull/70)).
  `ROADMAP.md` lists only their follow-ups (sessions 22–24).
- **Next:** session 22 (Character animation III), the highest-priority work left.
- **Open checks after #69:** its Windows crash paths (deadline thread, watchdog timeouts, Open Report) are only
  compile-tested, so they belong with the real-GPU smoke tests on Windows hardware.

## Later (L, one session each)

- Reflection probes and SSR
- Decals
- Text in 3D
- HDR output
- Motion matching
- Procedural creatures

## Outside the sessions

- **Real-GPU smoke tests (P0).** This is the roadmap's top priority, but it needs NVIDIA, AMD and Intel hardware
  on Windows and native Linux. Run it whenever that hardware is available; an AWS g4ad instance covers AMD. The
  procedure is in `Docs/TestedHardware.md`.
- **Wayland testing.** This is a manual check on Ubuntu 24.04.
- **GPU particle simulation (M).** This is conditional: it gets a session only if particle counts start to demand it.
- **Soft six-DOF rotation limits.** This is blocked until Jolt adds springs to rotation limits.

## Ordering notes

- **Three items come early because later work depends on them:** benchmarks before instancing (so there's a
  baseline to compare against), clipping before scroll views, and stable IDs before material assets and the
  archive. Doing them early keeps later sessions from redoing work.
- **TAA comes late on purpose.** Motion vectors have to know where every vertex was in the previous frame,
  which affects the instancing, skinning and morph code. Doing those first means the reprojection code is
  written once.
- **Each session is sized like recent milestone PRs:** most pair one M item with one S or M item, and the L
  items run alone.
