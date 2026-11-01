# Basalt Engine — Maturity Evaluation & Gap Analysis (updated 2026-10-21)

## Update 2026-10-21 (main @ f5c396a, 28 commits / +20K lines since abffa75)
Basalt scores show `new (old)`. Shipped since the first report:
- Stable asset UUIDs (`.meta` sidecars, `AssetRegistry`, reference migration on move/rename) — closes the path-reference blocker.
- Mesh LODs: meshoptimizer import + distance-based selection in `SceneRenderer` (`MeshComponent.LODDistance`).
- KTX2/BC7 compression (`asset.compress_texture`), Basis ETC1S/UASTC transcode, Zstandard.
- glTF extensions: texture_transform, emissive_strength, clearcoat, transmission, mesh_quantization, texture_basisu.
- `.bmaterial` material assets + Material Editor panel + lossless thumbnails.
- Morph targets (GPU, blended through clips/layers/fades, Lua weights) and retargeted clip libraries.
- Particles: soft, lit, rotation/angular velocity, random start size, size/color curves, edit-mode preview.
- Audio: high-pass/echo/distortion, effects on one-shots, `ReverbZoneComponent` blending.
- Size now ~60.6K engine lines, editor 7.6K (6 panels), tests 41.6K lines / ~1,219 cases, 12 fuzz targets.
Still open: occlusion culling, GI, fog, TAA/SSR, navigation, networking, terrain, Lua debugger, non-desktop platforms,
real-GPU verification, a shipped game. Note: the first report said `LODDistance` was only pose throttling — mesh LOD now exists.


Method: read-only survey of the repo at `abffa75` (branch `game-ui-followups`): source/test/doc line counts,
ROADMAP.md, Docs/, CI config, feature probes by grep. Nothing was built or run. Scores are judgment calls,
not benchmarks. Competitor facts are from general knowledge plus the sources at the end.

## 1. Snapshot

| Metric | Value |
|---|---|
| Age | 13 days (first commit 2026-10-07), 289 commits, effectively one developer + AI agents |
| Engine library | ~55K lines across 15 modules (Renderer 7.8K, Core 6.3K, Scene 6.3K, Automation 5.9K, Animation 5.2K, UI 5.0K, Physics 4.6K, Scripting 4.5K) |
| Apps | Editor 6.7K lines (5 panels + viewport/Game view), Runtime 0.6K, CLI 0.2K |
| Shaders | ~1.8K lines GLSL, 38 shader files |
| Tests | 36.5K lines, ~1,035 test cases/subcases, 600+ feature-test Lua checks (per ROADMAP; 334 `Check(` call sites), 11 libFuzzer targets, 7 sample games with scripted tests, GPU golden tests |
| Test:engine ratio | ~0.66 (very high; most engines are far lower) |
| CI | Win/macOS × Debug/Release, Linux Release + lavapipe GPU job, Linux Debug ASan+UBSan, clang-tidy, format, coverage (on `main`), nightly fuzz, benchmarks, determinism hash pinning |
| Docs | ~7.6K lines: Scripting API (116 KB), Automation commands (generated), Components (generated), MCP, Rendering, LuaLS defs |
| Scale reference | Godot ≈ 1.5M+ lines, Bevy ≈ 300K+, O3DE ≈ multi-million, Unreal ≈ 10M+ |

## 2. What exists

- **Rendering:** Vulkan 1.3 via nvrhi; clustered forward lighting; PBR + IBL from HDRIs; procedural sky; cascaded
  directional shadows + local-light shadow atlas; SSAO; bloom; histogram auto-exposure; tonemapping; FXAA; FSR 1;
  GPU skinning; CPU particles (billboards); batched/instanced opaque draws; frustum culling per view; async asset
  streaming; debug views; Tracy.
- **Physics:** Jolt (rigid bodies, box/sphere/capsule/mesh colliders, joints incl. six-DOF, character controller,
  raycasts/shape casts), multithreaded via the job system, cross-platform deterministic hashes.
- **Animation:** unusually deep for the age — state machines (`.banimgraph`), 1D/2D blend spaces with phase sync,
  crossfades, layers with masks and additive clips, root motion, events, attachments, two-bone/look-at IK, foot
  placement, spring bones, pose LOD throttling, parallel pose jobs.
- **Audio:** miniaudio with buses, low-pass, Doppler, reverb sends.
- **Gameplay:** Lua 5.4 + sol2, hot reload keeping instance fields, timers, coroutines, events, save data, input
  actions with gamepad, in-place scene switching, persistent entities, seeded deterministic random streams.
- **Game UI:** retained components + immediate-mode `UI.*`, layout, transforms, clipping, scroll views, menus,
  dropdowns, text fields, TrueType fonts, mouse/keyboard/gamepad navigation.
- **Editor:** hierarchy, inspector (registry-driven), content browser with thumbnails, project settings, console,
  gizmos, undo/redo with transactions, Game view, play/simulate on a scene copy, file watching/auto-reload.
- **Pipeline/shipping:** glTF import, prefabs, project validator, exporter (host OS only) with smoke test.
- **Automation (differentiator):** ~60 commands with JSON schemas, CLI, TCP server, MCP server, scene snapshot/diff,
  `assert.*`, `render.compare`, event streams, input recording/replay. The editor itself uses this API.

## 3. Gap analysis

### 3a. Gaps the roadmap already tracks
Real-GPU verification on NVIDIA/AMD/Intel (P0 — rows in `Docs/TestedHardware.md` are empty), TAA, SSR and
reflection probes, decals, 3D text, GPU particle simulation, ragdolls, retargeting follow-ups (rest-pose matching),
property/keyframe animation, FABRIK/CCD, animation graph editor, prefab overrides,
native file dialogs, packed archives, platform packaging/signing, crash reporting, release
pipeline, HDR output, Wayland.

### 3b. Gaps NOT on the roadmap (the more important list)
These are things Godot/Unity/Unreal users assume exist:

| Gap | Severity for a "production" claim | Notes |
|---|---|---|
| Navigation / pathfinding (navmesh, agents, avoidance) | High | No Recast/Detour or equivalent; any NPC game needs it |
| Networking / multiplayer | High | No transport, replication or rollback |
| Occlusion culling | High | Mesh LODs shipped 2026-10-20; culling is still frustum-only |
| Global illumination | Medium-High | No lightmaps, probes-based GI or SSGI; only IBL + SSAO |
| Terrain | Medium | No heightfield terrain, foliage or splatting (also no Jolt heightfield collider) |
| Fog / volumetrics / atmosphere | Medium | No distance/height fog or volumetric lighting |
| Script debugger & profiler for Lua | Medium | No breakpoints/stepping; errors only |
| Native gameplay path (C++/C# gameplay modules) | Medium | Lua is the only gameplay language |
| Shader/material graph, custom user shaders | Medium | Shaders are embedded engine shaders; no user shader authoring |
| 2D game pipeline (sprites, tilemaps, 2D physics) | Medium | 2D is only UI; 2D samples are built from 3D primitives |
| Localization | Medium | No string tables / font fallback per locale |
| Cloth, soft bodies, vehicles | Low-Med | Jolt supports them; not exposed |
| Sequencer / cinematics, video playback | Low-Med | Partly covered by planned property animation |
| Platform reach beyond desktop | Strategic | No mobile, web or console; Vulkan-only (MoltenVK on macOS); export only for the host OS |
| Accessibility (screen reader, remapping UI, colour-blind modes) | Low-Med | Input actions help remapping |
| Real-world validation | Critical | No shipped title, no external users, GPU verified on Apple M4 Max + lavapipe only |

## 4. Scorecard (0–10)

Weights: **G** = general-purpose production engine; **A** = desktop 3D engine for AI-agent-driven authoring.

| Dimension | G | A | Basalt | Godot 4.6 | Bevy 0.19 | Unity 6 | Unreal 5 | Hazel-class |
|---|---|---|---|---|---|---|---|---|
| Rendering features | 10 | 10 | 6 (5) | 7 | 7 | 8 | 10 | 4 |
| Render scalability (LOD/occlusion/GI/streaming) | 8 | 4 | 4 (2) | 6 | 5 | 8 | 10 | 2 |
| Physics | 6 | 6 | 6 | 7 | 5 | 8 | 9 | 4 |
| Animation | 6 | 6 | 8 (7) | 7 | 5 | 8 | 10 | 3 |
| Audio | 4 | 4 | 6 (5) | 6 | 3 | 7 | 9 | 2 |
| Scripting & gameplay | 8 | 8 | 6 | 8 | 7 | 9 | 9 | 5 |
| Game UI | 4 | 4 | 6 | 9 | 5 | 7 | 8 | 2 |
| Editor & tooling | 10 | 4 | 5 (4) | 9 | 2 | 9 | 10 | 5 |
| Asset pipeline | 8 | 6 | 5 (3) | 7 | 5 | 9 | 9 | 3 |
| Platform reach | 8 | 3 | 3 | 9 | 8 | 10 | 9 | 2 |
| Networking | 5 | 1 | 0 | 6 | 3 | 7 | 9 | 0 |
| AI / navigation | 4 | 2 | 0 | 7 | 2 | 8 | 9 | 0 |
| Engineering rigor (tests/CI/fuzz/determinism) | 4 | 10 | 9 | 6 | 7 | 7* | 7* | 4 |
| Automation / agent API | 1 | 15 | 9 | 4 | 4 | 6 | 6 | 1 |
| Docs | 4 | 6 | 6 | 9 | 6 | 9 | 8 | 3 |
| Production proof & ecosystem | 10 | 3 | 1 | 8 | 5 | 10 | 10 | 1 |
| **Weighted total (G)** | | | **46 (40)** | 75 | 51 | 85 | 92 | 28 |
| **Weighted total (A)** | | | **63 (58)** | 68 | 53 | 79 | 85 | 29 |

\* closed source, estimated. "Hazel-class" = single-developer C++ learning engines (Hazel, or the AI-built engine
from The Cherno's video cited in ROADMAP.md). Bevy's physics/networking come from third-party crates (Avian, Rapier, Lightyear), which
is counted as partial.

### Maturity level
On a 1–5 scale (1 prototype → 5 proven in commercial production):
- **As a general engine: ~2 / 5 ("advanced prototype / early alpha").** Feature breadth of a 1–2 year hobby engine,
  engineering rigor above many mature OSS engines, but zero field validation and several genre-blocking gaps.
- **For its niche (agent-authored desktop 3D games): ~3 / 5.** The automation/MCP API, determinism and test harness
  have no direct equivalent in mainstream engines.

## 5. Positioning vs the open-source field (bobeff/open-source-engines)

| Tier | Engines | Basalt vs them |
|---|---|---|
| Mature all-in-one | Godot, O3DE, Stride, Defold | Behind on editor, platforms, pipeline, ecosystem; comparable or ahead on animation depth and test rigor |
| Code-first / emerging | Bevy, Fyrox, Flax, Castle, NeoAxis | Roughly on par in features; ahead of Bevy on editor and physics integration; behind Bevy on platform reach (web) and community |
| Legacy / renderers | Irrlicht, Torque3D | Ahead on modern rendering, physics and tooling; behind on years of field use |
| Learning/framework engines | Hazel, Esoterica, raylib-class | Clearly ahead in breadth, tests and tooling |

## 6. Recommended priorities (beyond the current roadmap)

1. **Ship one non-trivial game end to end** (3D, ~30 min content, exported on all three OSes) — the cheapest way to
   surface pipeline and scale problems that unit tests can't.
2. **Finish P0 real-GPU verification** (NVIDIA/AMD/Intel, Windows + native Linux).
3. ~~Stable asset IDs~~ — shipped 2026-10-20.
4. **Scale:** occlusion culling (mesh LODs and texture compression shipped), measured with a large benchmark scene.
5. **Navigation** (Recast/Detour behind a component + `Physics`-style system, exposed to Lua and automation).
6. **Lua debugging** (DAP adapter so VS Code can break/step) — biggest gameplay-iteration gap.
7. **Fog + one GI technique** (baked light probes or SSGI) — the largest visual-quality gap versus Godot.
8. Decide strategically on **networking** and **non-desktop platforms**: either roadmap them or state them as
   non-goals so the "production-grade" claim is scoped honestly.

## Sources
- https://github.com/bobeff/open-source-engines
- https://www.cgchannel.com/2026/01/discover-5-key-features-for-cg-artists-in-godot-4-6/
- https://linuxiac.com/godot-4-6-open-source-game-engine-released-with-workflow-improvements/
- https://alternativeto.net/news/2026/1/godot-4-6-debuts-modern-theme-jolt-physics-node-ids-and-libgodot-embed
- https://bevy.org/news/bevy-0-19/
- https://gamefromscratch.com/bevy-0-18-released/
