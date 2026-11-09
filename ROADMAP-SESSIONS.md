# Roadmap Sessions

The `ROADMAP.md` items grouped into single-session milestones, sorted in order of implementation, with completed
sessions retained for reference. The order follows the roadmap's priorities (P0 networking, then P1 release and proof,
P2 scale, P3 debugging and editor workflow, P4 rendering, P5 gameplay systems, P6 follow-ups), adjusted so that work a
later session depends on comes first. Sessions 1–24 were planned against the earlier roadmap; sessions 26 onward follow
the maturity assessment (`Docs/MaturityAssessment.md`) and the scope settled in `ROADMAP.md`.
Updated on 9 November 2026: sessions 1–25 have shipped, and session 26 is next.

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
| 22 | **Character animation III** ✅ | Powered ragdoll follow-ups: partial ragdolls whose root follows the animation, per-bone motor strengths, active balance (M); spring bone follow-ups II: physics layer filtering, continuous collision, edit-mode collisions against static bodies (S) | Shipped in [PR #71](https://github.com/ahmad-luqman/AIGameEngine/pull/71): `KinematicRoot` rigs that keep the entity's body or character and walk with the animation, per-bone `MotorStrength`, active balance (`IsRagdollBalanced`), and spring chain `Layer`, `Continuous` and static bodies in the edit preview. |
| 23 | **Animation authoring II** ✅ | IK and layer follow-ups II: FABRIK joint limits, a mask editor, layer graph events and per-layer parameters, edit-mode ground placement (S each); retargeting follow-ups II: a joint map editor, reference poses from per-joint rotation offsets (S) | Shipped in [PR #72](https://github.com/ahmad-luqman/AIGameEngine/pull/72): Fabrik chain `SwingAngle`, `TwistMin`, `TwistMax` and `JointLimits` measured from the rest pose; Animation Mask and Joint Map windows over one headless draft, with `animator.match_joints`; layer `Events` and `OwnParameters` with `SetAnimationLayerParameter`/`SetAnimationLayerTrigger`; `Ground` chains on static bodies in the edit preview; and pair `Offset`s that stand in for a reference pose. |
| 24 | **Property animation III** ✅ | Stable Euler serialization at exact gimbal lock, physics configuration tracks that preserve contacts, asset reference tracks (texture flipbooks), tracks on other entities (S) | Shipped in [PR #73](https://github.com/ahmad-luqman/AIGameEngine/pull/73): `EulerFromQuat` keeps the orientation at and near gimbal lock and writes one canonical form there; bodies and colliders update in place, keeping contacts and joints, so tracks can animate each component's live fields; `Keys[].Asset` flipbooks with preloading; `Tracks[].Target` on other entities, through list element asset and entity fields at any depth. |
| 25 | **Signing** ✅ | Windows code signing with `signtool` (S); signed and notarized macOS tools and universal (arm64 + x86-64) binaries (S) | Shipped in [PR #75](https://github.com/ahmad-luqman/AIGameEngine/pull/75): `scripts/sign-windows.py` (signtool by certificate thumbprint or a signing service's library) signs the Windows tools and samples when `WINDOWS_CERTIFICATE` is set; the macOS tools and their Vulkan libraries are signed and notarized with the samples; the macOS release is universal for macOS 13.3 and later, with universal Vulkan libraries and an x86-64 run under Rosetta; exports drop the runtime's signature (Windows) or sign ad hoc (macOS). The macOS secrets still have to be added; Windows signing in CI needs a signing service set up in place of the `.pfx` import (`WINDOWS_SIGNING_DLIB`, `Docs/Shipping.md`), since publicly trusted certificates no longer come as `.pfx` files. |
| 26 | **Networking I** | Transport, connections and replication: a pinned UDP library, an authoritative host (listen server or `BrecciaRuntime --headless`), a networked-entity component with registry-declared replicated fields, snapshot deltas, `FuzzNetPacket` (L) | P0, first by decision (8 November 2026). Everything multiplayer builds on it. |
| 27 | **Networking II** | `Network` Lua API with messages and RPCs, client prediction and reconciliation for characters, snapshot interpolation, lag-compensated raycasts, `net.*` automation commands with simulated latency and loss (L) | Needs session 26's replication. |
| 28 | **Multiplayer sample** | A small arena or co-op game built through the automation API, with a headless server-and-two-clients ctest (M) | Proves sessions 26–27 end to end, as the Platformer did for character movement. |
| 29 | **Release v0.1.0** | Binary downloads on a `v0.1.0` tag (signed once the certificates are set up); a getting-started tutorial, starter template and default scene; CHANGELOG, stability policy, CONTRIBUTING, SECURITY, branch protection; third-party notices in exports; a decision on the name (M) | P1. Outside agents and the benchmark (session 31) need something to install. |
| 30 | **Agent sight and parity** | Headless `render.*` in the CLI and images over MCP; Simulate, Pause, Debug View and folder create/delete in the API; a token for the editor server (M); a decision on runaway scripts in exported games (S) | P1. The benchmark should measure the engine with its blind spot and parity gaps closed. |
| 31 | **Agent benchmark** | A CEGame-Bench-style task set run through Breccia, Godot and Unity with the same agent; tasks, harness and results published (M) | P1. The test of the agent-first claim; after sessions 29–30 so it measures what users get. |
| 32 | **Audio formats and voices** | Ogg Vorbis and Opus through libvorbis and libopus, fuzzed through `FuzzAudio` (M); a voice cap with priorities, a master limiter and raycast occlusion (M) | Before the real game, which needs compressed music and more voices than the samples. |
| 33 | **One real game** | About 30 minutes of content with real art and characters, built mostly by an agent, packaged on all three OSes (L, 2–4 weeks, several sessions) | P1. Its engine problems become roadmap items, so it comes before the scale and rendering work they may reorder. |
| 34 | **Scale I** | A realistic benchmark scene with CI frame-time gates, profiled (S); cached world transforms, linear mass destruction, physics sync of moved bodies only, fixed-step interpolation (M) | P2. The benchmark comes first so there is a baseline to compare against, as in sessions 3–4. |
| 35 | **Scale II** | A GPU suballocator, memory budget and eviction, `Scene.Load` releasing unused assets (M); importance-based light selection past 256 point lights (S) | P2. Measured on session 34's scene. |
| 36 | **Occlusion culling** | GPU two-phase HZB culling or CPU culling against baked occluders, chosen from the session 34 profile (L) | P2. Needs the benchmark to show where the cost is. |
| 37 | **Script debugging** | Lua stack traces on every protected call (S); a per-script and per-callback Lua profiler with Tracy zones (M) | P3. |
| 38 | **Lua debugger** | A Debug Adapter Protocol server in the editor, for VS Code and agents (L) | P3. Builds on session 37's error paths. |
| 39 | **Editor workflow** | Search, copy/paste, OS file drop, GUI rename and move (M); a curve and dope-sheet editor and free graph node placement (M) | P3. |
| 40 | **Custom shaders** | User materials with their own GLSL fragment code against a documented interface, errors in `project.validate` (L) | P4, first among rendering items: teams cannot work around its absence. |
| 41 | **Fog** | Height fog, then froxel volumetric fog lit by the clustered lights and shadows (M) | P4. Volumetric fog can reuse the clustered light lists. |
| 42 | **Global illumination** | One MoltenVK-friendly technique: baked irradiance probes or compute-updated DDGI-style probes (L) | P4. Sponza showed the gap plainly. |
| 43 | **Reflection probes and SSR** | Box-projected cubemap probes captured in the editor, then screen-space reflections (L) | P4. After GI, so captured probes include bounce light. |
| 44 | **Navigation** | Recast/Detour behind a component: navmesh baking in the editor and through automation, agents with avoidance, Lua path queries, debug drawing (L) | P5. |
| 45 | **Character follow-ups** | IK and retargeting follow-ups III (S); spring bone follow-ups III (S); powered ragdoll follow-ups II (M) | P6. The follow-ups of sessions 17–24, which had no session. The real game (session 33) may reorder them. |

## Status (8 November 2026)

- **Shipped outside the sessions:**
  - The fix for glTF primitives that share index accessors ([PR #64](https://github.com/ahmad-luqman/AIGameEngine/pull/64)).
  - UTF-8 command-line arguments and paths on Windows ([PR #65](https://github.com/ahmad-luqman/AIGameEngine/pull/65)).
  - The five defects of the maturity assessment ([PR #74](https://github.com/ahmad-luqman/AIGameEngine/pull/74)): physics
    overflow reporting and temporary memory sized from capacity, an instruction limit for scripts in the editor and the
    CLI, non-JSON connections closed by the editor server, and undecodable clips refused by `project.validate`. Its
    residuals are in sessions 30 and 32.
- **Shipped in sessions:** sessions 1–25, the last eight in [PR #67](https://github.com/ahmad-luqman/AIGameEngine/pull/67),
  [#68](https://github.com/ahmad-luqman/AIGameEngine/pull/68), [#69](https://github.com/ahmad-luqman/AIGameEngine/pull/69),
  [#70](https://github.com/ahmad-luqman/AIGameEngine/pull/70), [#71](https://github.com/ahmad-luqman/AIGameEngine/pull/71),
  [#72](https://github.com/ahmad-luqman/AIGameEngine/pull/72), [#73](https://github.com/ahmad-luqman/AIGameEngine/pull/73)
  and [#75](https://github.com/ahmad-luqman/AIGameEngine/pull/75). Session 25's pipeline signs once the certificates are
  set up (`Docs/Shipping.md`); until then it signs macOS ad hoc and leaves Windows unsigned.
- **Next:** session 26 (Networking I).

## Later (one session each)

- Terrain and large coordinates (L)
- Decals (L)
- Text in 3D (L)
- HDR output (L)
- Game UI text: shaping, fallback fonts, right-to-left text, IME, localization (M)
- Asset import settings and FBX (M)
- Vehicles and cloth (M each)
- Clip compression (M)
- Cross-platform export (M)
- Networking III: interest management, and lockstep or rollback once the whole simulation is deterministic (L)
- Motion matching (L)
- Procedural creatures (L)
- Mobile (L) and web (L), only after the release (session 29) and the real game (session 33); see `ROADMAP.md`, Scope.

## Outside the sessions

- **Real-GPU smoke tests (P1).** This needs NVIDIA, AMD and Intel hardware on Windows and native Linux. Run it whenever
  that hardware is available; an AWS g4ad instance covers AMD. It also covers PR #69's Windows crash paths (deadline
  thread, watchdog timeouts, Open Report), which are only compile-tested. The procedure is in `Docs/TestedHardware.md`.
- **Wayland testing.** This is a manual check on Ubuntu 24.04.
- **GPU particle simulation (M).** This is conditional: it gets a session only if particle counts start to demand it.
- **Soft six-DOF rotation limits.** This is blocked until Jolt adds springs to rotation limits.

## Ordering notes

- **Networking comes first by decision.** It was settled with the scope on 8 November 2026, ahead of the assessment's
  own order, which put release and proof first. The sample (session 28) keeps it honest: a layer without a game is not
  done.
- **Release before the benchmark, and agent sight before both.** Outside agents need binaries, and a benchmark run
  with the headless render gap open would measure a known weakness rather than the engine.
- **The real game before scale and rendering.** It will find problems no plan lists, and those may reorder sessions
  34–45. Audio formats come just before it because real content needs compressed music.
- **Benchmarks before optimizations.** As session 3 came before session 4, session 34's scene comes before transforms,
  memory and occlusion culling, so every gain is measured.
- **Custom shaders before fog, GI and reflections.** Teams cannot work around missing shaders, and the later passes
  should be written once against the material interface that session 40 defines.
- **Three items came early in sessions 1–24 because later work depended on them:** benchmarks before instancing,
  clipping before scroll views, and stable IDs before material assets and the archive.
- **TAA came late on purpose.** Motion vectors have to know where every vertex was in the previous frame, which
  affects the instancing, skinning and morph code. Doing those first meant the reprojection code was written once.
- **Each session is sized like recent milestone PRs:** most pair one M item with one S or M item, and the L
  items run alone. The real game is the exception: it spans several sessions.
