# Breccia Engine — Maturity Assessment and Gap Analysis

Assessed on 2026-11-07 at `main` @ `4f823ab` (0.1.0, untagged). This replaces the 2026-10-21 report, which was a survey
of `abffa75` with a delta on top. Scores read `new (old)`.

## 1. Verdict

**Breccia is an early alpha with exceptional engineering process and a real agent interface, not yet a production
engine.** General score **34.8 / 100 (was 46.5, printed as 46)**, agent-authoring score **48.1 / 100 (was 63)**. On the old
dimensions and weights, for a like-for-like comparison, the fall is 46.5 → 35 and 63 → 50. Breccia did not get worse. In fact it gained
sessions 11–24 since the last report. The scores fell for three reasons:

1. **The old scores were feature-list scores.** This pass read the code, built it from a clean clone and ran it past the
   sizes its own benchmark uses. Breadth held up; depth did not. Audio, UI, particles and scene scale are thinner than
   their feature lists imply.
2. **Testing and a source audit found real defects:** physics silently fails at scale, a script loop hangs an agent's
   session, the validator accepts a format the engine cannot play, and the editor's automation port accepts commands
   wrapped in HTTP (section 4).
3. **Competitors moved.** Unity and Unreal now ship official agent tooling and Godot's animation stack overtook Breccia's,
   so two of the old claims of superiority no longer stand. Godot's IK family, spring bones, retargeting and animation
   editor beat Breccia's authoring, and Breccia's lead on the agent interface narrowed from 3 points over Unity to 1.

**What Breccia can ship today:** a contained, stylized desktop 3D game with a Latin-only UI and no multiplayer, no NPC
pathfinding and no open world, by a team that can fix engine problems itself. **What it cannot ship without new engine
systems:** anything on console, mobile or web; dense levels; large worlds; localized releases; multiplayer.

**What is genuinely strong:** process rigor (section 3), deterministic replay for agents, which none of the established
engines documents, strict data validation, and tiny outputs (24 MB exported game, sub-second startup).

## 2. Method and confidence

Unlike the last report, this one ran the engine.

- **Measured:** clean clone from GitHub, documented build, full ctest, the repo's benchmarks, an external content test
  (Khronos Sponza), stress scenes past the repo's own budgets, export, and reproductions of the suspected defects, all on
  an Apple M4 Max under MoltenVK with validation on. Nothing was measured on Windows, Linux or a discrete GPU.
- **Read:** two independent source audits (runtime; tooling and governance), each started without this document's
  scores. Their worst findings were then checked against the source or reproduced.
- **Researched:** official release notes and docs for each competitor, on 2026-11-07. GitHub figures come from the
  GitHub API that day. Claims that could not be confirmed are in section 13, not stated as facts.
- **Scores are judgment calls applied to the anchors in section 9,** not benchmarks. Treat Breccia, Godot, Unity and
  Unreal as ±1 per cell; Bevy ±1 to 2 and Flax, O3DE and Fyrox ±2, because those were assessed from docs alone. No
  competitor was built or run, so hands-on findings exist only for Breccia. That asymmetry is real: Breccia's defects were
  looked for, and competitors' were not. Their scale limits are not compared here.

## 3. Snapshot

| Metric | 2026-10-21 report | Now (measured) |
|---|---|---|
| Age | 13 days, 289 commits | 31 days (first commit 2026-10-07, last 11-05), 442 commits, 72 PRs |
| People | One developer plus AI agents | Same: 435 of 442 commits by one account |
| Public signal | not recorded | 0 stars, 0 forks, 0 external issues, 0 tags, 0 releases |
| Engine library | ~60.6K lines | 77.1K lines (Scene 9.9K, Core 9.8K, Animation 9.5K, Renderer 8.6K, Automation 7.3K, Physics 7.2K, Asset 6.5K, UI 5.0K, Scripting 4.8K, Project 3.5K, Audio 2.5K, Particles 1.0K) |
| Apps | Editor 7.6K, 6 panels | Editor 10.4K, 10 panels; Runtime 0.6K; CLI 0.2K |
| Shaders | ~1.8K lines, 38 files | 2.3K lines, 56 files |
| Tests | 41.6K lines, ~1,219 cases, 12 fuzz targets | 61.4K lines, 1,609 `TEST_CASE`s (1,606 run) + 158 subcases, 16 fuzz targets, 411 feature-test `Check(`s |
| Test run | not run | 82 of 82 ctest entries pass in 58 s, 34 of them GPU tests |
| Line coverage | not recorded | 85.05% lines, 91.43% functions (CI, 4f823ab); most modules 88–96%, Core 80%, **Renderer 29.5%** (the job is headless; GPU tests are not in it) |
| API surface | ~60 commands | 86 commands, 41 components |
| Docs | ~7.6K lines | 10.5K lines, reference only, no tutorial |
| Samples | 7 | 7, totalling about 1,700 lines of Lua and under 1 MB; Tetris and Breakout are small complete games, the rest feature showcases |
| CI | Per the report | 13 jobs green at HEAD (Windows and macOS Debug/Release, Linux Release with lavapipe GPU tests, ASan+UBSan, clang-tidy, coverage, fuzz), nightly fuzz green |
| Real GPUs | "rows empty" (wrong even then) | Apple M4 Max, NVIDIA RTX 4060 Laptop, Intel UHD 770 (Windows), 0 validation errors. AMD, desktop NVIDIA and native Linux still open; the interactive step is recorded for none |
| Scale reference | | Godot ≈ 1.5M+ lines, Unreal ≈ 10M+ (rough, from the last report) |

**Corrections to the old report.** Section 3a listed as open what has since shipped: TAA, ragdolls, property animation,
FABRIK, the graph editor, prefab overrides, file dialogs, archives, packaging, crash reporting and the release pipeline.
It also said the hardware rows were empty, and its A column silently normalized weights that summed to 92.

## 4. What the hands-on testing found

### Measurements (M4 Max, 1080p, validation on, no vsync; ms/frame from wall-clock deltas, so indicative)

| Test | Result | Reading |
|---|---|---|
| Clean clone to built tools | 9 s clone, 5 min 52 s build, first try, 1.9 GB build tree | Good, on a very fast machine. Needs the C++ toolchain, CMake, Ninja, `glslc` and Vulkan first; there is no binary alternative |
| Zero to rendered cube via CLI | 3 steps (`project.create`, a batch, the runtime), seconds of tool time, 0.38 s for runtime start plus 60 frames | Best-in-class for an agent. A new scene is a void with no ground |
| Repo benchmark (2,242 entities, 320×180) | CPU 0.68 ms, GPU 4.0 ms mean | Passes, but this workload proves little: one cube mesh, one rig, tiny resolution |
| Khronos Sponza import | 0.16 s to import, 2 s for load plus 300 frames, 245K tris, correct materials and alpha masks, 76 draws (+339 shadow), 2.3 ms GPU, 422 FPS | Very good. The picture is flat and dark, though: no bounce light, the plainest evidence of the GI gap |
| Static cubes | 10K 3.7 ms, 50K 14.2 ms, 200K 58.4 ms, 500K 145.7 ms; scene GPU only 0.7, 1.2, 5.5, 13.8 ms | GPU batching works. **CPU is about 90% of the frame and grows linearly** (~0.29 ms per 1,000 static entities), consistent with the audit's finding of uncached transforms and per-entity walks, though no profile was taken. About 50K entities fit a 60 FPS budget on one of the fastest laptop CPUs available |
| Rigid bodies, stacked | 1K 2.1 ms, 5K 4.6 ms, 10K holds | Fine up to the limit below |
| Rigid bodies past that | **20K: 16,157 of 20,000 boxes fell through the floor within 6 s, nothing logged.** 50K: 41,031 | Defect (below) |
| Point lights | Above 256 (documented in `Docs/Rendering.md`), the rest are dropped, reported as `lightsSkipped` in `--stats` | A hard per-scene cap with no importance selection; a scene of 1,024 lights skipped 768 of them |
| Lua `OnUpdate` | About 1 µs per script with a position read and write | Fine for gameplay at thousands of scripts |
| Export (Platformer) | 24 MB, about 1 s including a 60-frame smoke test; `.app`, folder and `.bpak` all work | Excellent: small and fast |

### Verified defects

These were reproduced or confirmed in source, and they carry more weight than any missing feature because they fail
silently or fatally. **Status:** PR #74 addressed all five after this assessment. Defects 1 and 2 are fixed. Defect 3 is
fixed in the editor and CLI for script code, by an instruction limit rather than a time budget, with the limits
`Docs/ScriptingAPI.md` lists, and remains in exported games. Defect 4
now refuses HTTP, with no authentication added. Defect 5 is now a validation error, and Vorbis still does not play. The
table and the scores below stand as measured at `4f823ab`.

| # | Defect | Evidence | Impact |
|---|---|---|---|
| 1 | **Physics fails silently at scale.** Bodies pass through each other and the floor, unreported. `System->Update`'s error result is discarded, so nothing says why; the likely path is the cap of `MaxContactConstraints = 16384` (`Physics/PhysicsWorld.cpp:205,772`) | Failure reproduced with the 20K-body test above; the cause is inferred from the code | A game past the limit breaks with nothing in the log |
| 2 | **Physics temp memory aborts the process.** Fixed 16 MB `TempAllocatorImpl`, which calls `abort()` when exhausted, with up to 65,536 bodies allowed (`PhysicsWorld.cpp:207`) | Confirmed in source; not triggered in testing | A crash with no fallback |
| 3 | **A script loop hangs the host.** No `lua_sethook` and no command cancellation | Reproduced: `while true do end` in `OnUpdate` froze `play.step` until killed at 20 s | Most likely fatal event in an agent's generate-test-repair loop; also hangs a shipped game |
| 4 | **The editor's TCP port accepts commands wrapped in other protocols.** No authentication, and non-JSON lines are skipped, so an HTTP request's body can run as a command | Reproduced on a local socket with a browser-shaped request and a foreign `Origin` header: a project file was written. Not tried from an actual browser; newer browsers may block loopback requests | Any local process, and possibly a web page, can write project files and run Lua while the editor is open. The Lua sandbox has no `io` or `os`, which limits the damage |
| 5 | **OGG passes validation and cannot play.** `.ogg` is in the validator and exporter lists, but no Vorbis decoder is compiled in | Confirmed in source; not played | Silent content failure on a common format |

### Other audit findings that drove scores

- **GPU memory:** one Vulkan allocation per resource, no suballocator, no budget, no eviction; `AssetManager` is not
  cleared by `Scene.Load`, so what a session touches stays resident.
- **Renderer:** one queue, one command list, no indirect draws, no bindless; `Render` is a single function of about 1,375
  lines; a benchmark that proves nothing about scale.
- **Scene:** one synchronous scene, 32-bit coordinates throughout, no streaming or additive scenes, no entity active flag,
  mass destruction is O(n²), physics sync is O(all bodies) per step, no fixed-step interpolation.
- **Debugging:** ordinary script errors carry no call stack; no Lua debugger or profiler; Tracy is a build flag.
- **Agent blind spot:** the CLI's `render.*` commands fail, so a headless agent cannot see the frame it stepped to; MCP
  returns paths, not images. Agents need the editor, or a separate runtime run that loses session state.
- **"The GUI cannot do anything the API cannot" is about 95% true,** with four exceptions: Simulate, Pause, Debug View,
  and folder create/delete.
- **Editor daily-use gaps:** no search, no copy/paste, no OS file drop (the event is unread), no GUI rename or move, no
  dope sheet or curves; nodes on a fixed grid.
- **Audio, UI, particles:** no voice cap or priority, HRTF, occlusion or limiter; no text shaping, RTL, fallback fonts, IME
  or localization, and the game UI draws through ImGui; particles have no modules, collision, trails or GPU path.
- **Governance:** the README and `AGENTS.md` say "production-grade" for an untagged 0.1.0 with no users. No changelog,
  stability policy, CONTRIBUTING or SECURITY file; `main` is unprotected; exported games carry no third-party notices
  (a legal reading that needs checking); nothing is signed by default.
- **`ROADMAP.md` omits** navigation, networking, occlusion culling, GI, fog, terrain and a Lua debugger, the items that
  most limit the engine.

### What held up

- All 15 randomly sampled tests assert real behavior, and only 4 of 1,609 cases have no direct assert. All 6 sampled Lua
  API entries matched the code. No stubs or TODOs were found in the runtime.
- Error messages are excellent: wrong parameters list the expected names, and script errors give file and line.
- Crash handling, the `.bpak` archive, prefab merging, UUID asset identity and strict JSON handling are engineered above
  the rest of the codebase.
- Deterministic physics hashes hold across platforms in CI. Godot's Jolt integration explicitly gives no such guarantee,
  and none was found for Unity's 3D physics.

## 5. Competition as of 2026-11-07

| Engine | Current | Next | Licence | Activity |
|---|---|---|---|---|
| **Unreal** | 5.8 (June 2026, hotfix 5.8.4) | 5.9 preview announced for the week of 9 Nov; UE6 early access targeted Q4 2027 | 5% royalty above $1M lifetime per product | GDC 2026: primary engine for 42% of developers |
| **Unity** | 6.6 (Sep 2026); 6.3 is current LTS | 6.7 LTS in beta; Unity 7 preview Dec 2026, release Q1 2027 | Free below $200K; Pro $2,310 per seat per year; no royalty | GDC 2026: 30%; 49.4% of 2025 scanned Steam games |
| **Godot** | 4.7.2 (2026-08-18) | 4.8 beta 2 (10-29) | MIT | 119.7K stars; 7.1% of 2025 Steam games, 47% of GMTK 2026 entries |
| **Bevy** | 0.20.1 (2026-10-28) | No 0.21 RC | MIT or Apache-2.0 | 49.6K stars; 75 migration items from 0.19 |
| **O3DE** | 26.05 (2026-05-27) | 26.10 due 2026-11-11 | Apache-2.0 or MIT | 9.8K stars; Linux Foundation; robotics focus |
| **Flax** | 1.13.1 (2026-11-04) | | 4% royalty above $250K per quarter, source available | 7.1K stars |
| **Stride** | 4.4.1 (2026-10-17) | | MIT | 7.9K stars; editor Windows-only |
| **Fyrox** | 1.0.1 (March 2026) | 2.0 in progress | MIT | 9.7K stars; little funding |
| **Defold** | 1.14.0 (2026-10-12) | 1.14.1 beta | Defold License | 6.5K stars; 2D-first |
| **Wicked Engine** | 0.72.139 (2026-11-03) | | MIT | 7.3K stars; single lead |

### Rendering and scale

| Capability | Breccia | Godot 4.7 | Bevy 0.20 | Unity 6.6 | Unreal 5.8 | Flax 1.13 |
|---|---|---|---|---|---|---|
| Global illumination | **None** | SDFGI, VoxelGI, lightmaps, SSIL | Lightmaps, irradiance volumes, Solari (experimental) | Lightmaps, Adaptive Probe Volumes | Lumen (software and hardware RT) | DDGI |
| Reflections | IBL only | SSR, probes | SSR, parallax cubemaps | Probes; SSR in HDRP, URP in 6.7 | Lumen reflections | Unconfirmed |
| Occlusion culling | **None** | CPU (Embree, baked occluders) | GPU two-phase (experimental in 0.16) | GPU (Resident Drawer) | Nanite, hardware | HZB and hardware |
| Virtualized geometry | No | No | Meshlets (experimental) | No | Nanite | No |
| Upscaling | FSR 1 | FSR 2.2, FSR 1 | DLSS (NVIDIA) | STP, DLSS, FSR | TSR | TAAU, FSR 4, DLSS plugins |
| Fog and volumetrics | **None** | Volumetric fog | Volumetric fog, atmosphere | Fog; volumetrics in HDRP | Full set | Volumetric fog |
| Decals | **No** | Clustered | Forward, clustered | URP and HDRP | Yes | Yes |
| Terrain | **No** | Add-ons only | No first-party | Yes | Landscape, Mesh Terrain (exp.) | Yes, with foliage |
| GPU particles | **No** | Yes | Via `bevy_hanabi` | VFX Graph | Niagara | Yes |
| Custom shaders | **None for users** | Text and visual | WESL | Shader Graph | Material Editor, Substrate | Material graph |
| HDR display output | No | Since 4.7 | No (future work) | Yes | Unconfirmed | Unconfirmed |
| Texture streaming | **No** | In 4.8 | No first-party | Content Directories | World Partition, Nanite | Unconfirmed |
| Large worlds | No (float32) | Custom double build | Community `big_space` | Unconfirmed | World Partition | Unconfirmed |

Breccia has the same core as peers at the clustered PBR, cascaded-shadow, SSAO, TAA and bloom level. The gap is everything
above that core: Bevy, which has no editor, is ahead of it on rendering.

### Physics, animation, gameplay

| Capability | Breccia | Godot 4.7 | Bevy 0.20 | Unity 6.6 | Unreal 5.8 | Flax 1.13 |
|---|---|---|---|---|---|---|
| Physics backend | Jolt 5.6 | Jolt (default for new 3D) | None built in; Avian, Rapier | PhysX; Unity Physics (ECS) | Chaos | PhysX 5.1 |
| Vehicles, cloth, soft bodies | **None** | Partial | Crates | Cloth | All, plus destruction, hair | Vehicles, cloth |
| Published determinism | **Pinned hashes across platforms** | No guarantee | Game's job | Partial, 2D | No guarantee found | Unconfirmed |
| State machines, blend spaces | Yes | Yes | **No first-party** | Yes | Yes | Yes |
| IK | Two-bone, look-at, FABRIK with limits, foot placement and locking | Two-bone, FABRIK, CCD, Jacobian, spline, look-at, constraints | **No first-party** | Animation Rigging | Control Rig, IK Rig | Yes |
| Retargeting | Yes, joint maps | `RetargetModifier3D` | No first-party | Humanoid (unconfirmed) | IK Retargeter | Yes |
| Ragdoll | Powered, active balance, kinematic root | `PhysicalBoneSimulator3D` | No first-party | Yes | Yes, plus Control Rig Physics | Yes |
| Spring bones | Yes, with continuous collision | `SpringBoneSimulator3D` | No first-party | No first-party found | Control Rig Dynamics | Yes, node in 1.12 |
| Motion matching | No | Not found | Not found | No first-party found | Pose Search | Not found |
| Curve/dope-sheet editor | **No** | Yes, with Bezier curves | No | Animation window, Timeline | Sequencer | Scene animations |
| Script languages | Lua 5.4 only | GDScript, C#, native extensions | Rust | C#, moving to CoreCLR | C++, Blueprint; Verse in UE6 | C#, C++, visual |
| Script debugger | **None** | Yes | Rust debuggers | Yes | Yes | Yes |
| Navigation | **None** | Yes | Crates | Yes | Recast, Mass, StateTree | Yes |
| Networking | **None** | High-level multiplayer | Crates (lightyear, replicon) | Netcode for GameObjects and Entities | Replication, Iris | Replication, RPC |

Breccia's runtime character stack remains its most impressive area: powered ragdolls with active balance and foot locking
stand up well against Godot. It lost the outright lead because Godot's IK solvers are broader and its animation editor
exists; Breccia's depth is also untested by any real character content.

### Platforms

| Target | Breccia | Godot | Bevy | Unity | Unreal | Flax | O3DE | Defold |
|---|---|---|---|---|---|---|---|---|
| Editor | Win, macOS, Linux | Win, macOS, Linux | None | Win, macOS, Linux | Win, macOS, Linux | Win, macOS, Linux | Win, Linux (macOS exp.) | Win, macOS, Linux |
| Windows, macOS, Linux | **Host OS only** | Yes, cross-export | Yes | Yes | Yes | Yes | Yes (macOS exp.) | Yes |
| iOS, Android | **No** | Yes | Yes | Yes | Yes | Yes | Android; iOS exp. | Yes |
| Web | **No** | WebGL 2 | WebGL 2, WebGPU | WebGL 2, WebGPU | Pixel Streaming; 5.9 target | Experimental | Not listed | Yes |
| PS5, Xbox, Switch | **No** | Paid third party | No route found | Yes | Yes | Yes | Not listed | With approval |
| XR | **No** | OpenXR, Android XR | Community | Quest, visionOS, Android XR | OpenXR | Unconfirmed | Unconfirmed | Not found |
| Graphics APIs | Vulkan only (MoltenVK) | Vulkan, D3D12, Metal, GL | wgpu | D3D, Vulkan, Metal, GL | D3D12, Vulkan, Metal | Unconfirmed; WebGPU on web | Unconfirmed | Metal, Vulkan, others |

Consoles are the hard gate for any new engine: even Godot goes through W4 Games ($800 per platform per year, or $2,000 for
all) or porting houses. Linux is 3.95% of Steam and macOS 2.35%, so Breccia's reach is effectively Windows PC plus Steam
Deck, which is a viable niche but a narrow one. Mobile is about 57% of the market by revenue (Newzoo, press-reported).

### Tooling and ease of use

| | Breccia | Godot | Bevy | Unity | Unreal |
|---|---|---|---|---|---|
| Install | Build from source (6 min on M4 Max); **no binary release** | 74–162 MB download, no install | Rust toolchain, `cargo add` | Reported 8.7–14 GB | 150–200 GB free advised (forum, Linux) |
| First-run path | Path prompt; empty project with no scene | Project manager | Code only | Hub | Home Panel |
| Tutorials | **None** | Large manual, demos | Quick start; book planned | Unity Learn | Epic Developer Community |
| Profiler | Stats overlay; Tracy as a build flag | CPU, GPU, Tracy, Perfetto | Tracy, frame graph | Profiler, Frame Debugger, Project Auditor | Insights family, Chaos Visual Debugger |
| Text scenes for VCS | Yes (JSON) | Yes | Yes | YAML | Binary; One File Per Actor |
| Test framework for games | `assert.*`, `render.compare`, batch | GUT, gdUnit4 | `cargo test`, BRP | Unity Test Framework | Automation, Gauntlet, Screenshot tests |
| Marketplace | None | Asset Store (new in 4.7) | 501 Bevy Assets | About 125K assets (third-party count) | Fab, 420K+ listings |

Breccia is unusual here: excellent for a machine and poor for a person. The agent path takes seconds; the human path has
no download, no tutorial and an editor that opens to a prompt.

### Agent tooling, the differentiator

| Capability | Breccia | Unity CLI | Unreal (MCP, Python, automation) | Roblox Studio MCP | Godot (community tools) | Bevy BRP |
|---|---|---|---|---|---|---|
| Status | Works, no external users | Beta 1.0.0-beta.17, free | Experimental in 5.8 | Built in since Mar 2026 | No official server | First-party BRP; community MCP |
| Scene and asset edits | Yes, schema-validated | Yes, plus `unity eval` C# | Partial | Yes | Yes | Entities |
| Headless without GPU | **Yes, full game loop** | Batch mode | Commandlets | Not documented | `--headless` | Yes |
| Frame stepping | **Yes** | Not documented | Not documented | Not documented | In `godot-ai` | Yes |
| Input injection | Yes | Yes | Automation Driver | Yes | Community | Yes |
| Screenshots | Editor only, or separate runtime run | Yes | Yes | Yes | Needs a window | Unconfirmed |
| Render comparison | Yes, editor only | Partial | Screenshot tests | Not documented | In `godot-mcp-pro` (paid) | Pixel Eagle, engine CI only |
| **Deterministic replay** | **Yes** | Input trace only, no physics guarantee | Replay is replication-based | Not documented | Input replay in `godot-mcp-pro`, no guarantee | Not documented |
| Runaway script protection | **No** | Unconfirmed | Unconfirmed | Unconfirmed | Unconfirmed | Unconfirmed |
| Authentication | None | Unconfirmed | None (loopback HTTP) | Unconfirmed | Varies | Unconfirmed |
| Evidence agents succeed | **None** | UnityBench: best 72.5% | CraftBench-UE; CEGame-Bench | CEGame-Bench | GameLogicBench; CEGame-Bench | None |

Three things changed since October.

- **Mainstream engines now have official agent access.** Unity's CLI is free and can run C# without a domain reload, Epic
  says MCP will be integral to UE6, and Roblox ships the most complete first-party loop, including a playtest subagent.
- **Community demand is proven elsewhere:** `CoplayDev/unity-mcp` has 15.8K stars, `godot-mcp` 6.5K and Blender's server
  33.5K.
- **The first multi-engine benchmark complicates the "lightweight is better" assumption.** CEGame-Bench (preprint, 50
  tasks, 5 engines) found that same-game swaps favored Unreal and Unity over Godot, in the authors' words because
  "Heavyweight engines are harder to drive but easier to check." Only 16% of runs reached the ending and 4% were fully
  solved. The swap measures engine plus toolchain, not engine alone, but Breccia's strict validation should benefit from
  the same effect. That is a hypothesis: **Breccia has never been run through an agent benchmark, and no agent outside this
  repo has used it.** The same studies report that agents rarely look at their own game (under 8% of tool calls in
  UnityBench), so Breccia's headless blind spot may matter less than it appears, though visual checks remain the weakest
  link in scene construction.

Breccia's durable edge is deterministic replay plus headless assertions, which no competitor documents together. The rest of its lead is being competed away.

## 6. Gap analysis

### Blocking any production claim

| Gap | Severity | Status |
|---|---|---|
| The five verified defects (section 4) | **Critical** | New |
| A shipped game, external users, a binary release | **Critical** | Unchanged: none |
| Real-GPU verification | High | Better: three GPUs, AMD and native Linux open |
| Scene transform and per-entity costs | High | New: limits scale to about 50K static entities |
| GPU memory budget and suballocation | High | New: risks hitting allocation limits on content-heavy games |
| Occlusion culling | High | Unchanged; 0 hits in source |
| Navigation | High | Unchanged; 0 hits |
| Lua debugger, stack traces, profiler | High | Unchanged; raised in priority, new debugging dimension |
| Tutorials, editor manual, first-run experience | High | New as a scored gap |

### Blocking whole genres or markets

| Gap | Blocks |
|---|---|
| Networking | All multiplayer |
| GI, reflections, fog, decals, custom shaders | Any realistic or stylized-lit look beyond basic PBR; custom shaders are the most serious, since teams cannot work around them |
| Terrain, foliage, world streaming, large coordinates | Open worlds |
| Text shaping, RTL, fallback fonts, IME, localization | Global release, including every CJK and RTL market |
| Mobile, web, console | Most of the market by revenue |
| 2D pipeline | All 2D games (sprites, tilemaps, 2D physics are absent) |
| Formats beyond glTF: FBX, USD, per-asset import settings | Teams with existing art pipelines |
| Native gameplay path | Performance-critical gameplay; one Lua state is the only option |

### Lower priority

Vehicles, cloth, soft bodies (Jolt has some; not exposed), sequencer and video playback, accessibility, spatial audio
polish (HRTF, occlusion), motion matching, HDR output, Windows signing.

Every item in the old "not on the roadmap" list was re-probed at `4f823ab`: navigation, occlusion, GI, terrain, fog,
tilemaps, cloth, vehicles, decals and reflections still have no code at all.

## 7. Positioning

| Tier | Engines | Breccia against them |
|---|---|---|
| Mainstream | Unreal, Unity | Different category. A narrow technical lead in deterministic agent testing only |
| Mature open source | Godot | Behind on everything a team notices first. Comparable on character runtime depth; ahead on determinism |
| Complete but smaller | Flax, O3DE, Stride, Defold | Behind on features and shipping. Flax is the closest to a small Unreal; Defold has the best official agent tooling in this group, including an agent manual, OpenAPI and `llms.txt`, though it has no official MCP |
| Code-first | Bevy, Fyrox | Bevy leads on rendering and platforms; Breccia leads on editor, physics integration and animation. Fyrox has a released 1.0, an editor and navigation; Breccia has the better renderer |
| Single-lead renderers | Wicked Engine | Wicked has far more rendering (Surfel GI, DDGI, path tracing) and iOS and console code. Breccia leads on tests and tooling |
| AI-native creation tools | Roblox Assistant and similar | A different market. Roblox publishes only inside Roblox; Breccia exports desktop games with no platform lock-in |

**Name collision.** The engine Claude built in The Cherno's "Can AI Make a Game Engine?" video (October 2026) is also
called Breccia. It is a separate, unreleased project from that video. Nothing about it counts as evidence for this repo,
and the shared name is a discoverability and confusion risk at first release.

Hazel could not be assessed: its site was unreachable on the day and its 3D engine is gated, so the "Hazel-class" column
is dropped.

## 8. Maturity level

On a 1–5 scale (1 prototype, 5 proven in commercial production):

- **As a general engine: 1.5 (was 2).** Early alpha. One month old, no release, no users, several genre-blocking gaps,
  and failures at scale that nothing reports.
- **For its niche, agent-authored desktop 3D games: 2.5 (was 3).** The automation layer and determinism are real and unusual,
  but they are unproven outside this repo and the host can be hung by the kind of code agents write.
- **As an engineering artifact: 4.** Tests, fuzzing, sanitizers and determinism are better than the product's maturity
  suggests. They catch what they look for. They missed the physics overflow because nothing tested past the cap.

## 9. Scorecard (0–10)

Each score applies these anchors, within the dimension's scope:

| Score | Meaning |
|---|---|
| 0 | Absent |
| 1–2 | Stub or sample-level |
| 3 | Works for demos; a commercial title needs engine work |
| 5 | Can ship a small game with workarounds |
| 7 | Can ship a mid-size commercial title without engine changes |
| 9 | AA/AAA standard |
| 10 | Defines the field |

Weights: **G** = general-purpose production engine; **A** = desktop 3D engine for AI-agent-driven authoring. Each column
sums to 100. The total is the sum of weight × score, divided by 10.

Three dimensions are new: Debugging & profiling (split out of Editor & tooling), Ease of
use & onboarding, and Licence & cost. Fyrox, O3DE and Flax columns are new; Hazel-class is dropped.

| Dimension | G | A | Breccia | Godot 4.7 | Bevy 0.20 | Unity 6.6 | Unreal 5.8 | Flax 1.13 | O3DE 26.05 | Fyrox 1.0 |
|---|---|---|---|---|---|---|---|---|---|---|
| Rendering features | 9 | 8 | **4 (6)** | 8 | 7 | 9 | 10 | 8 | 8 | 4 |
| Render scalability | 7 | 3 | **3 (4)** | 6 | 6 | 8 | 10 | 7 | 6 | 3 |
| Physics | 5 | 5 | **5 (6)** | 7 | 5 | 8 | 9 | 7 | 7 | 5 |
| Animation | 5 | 5 | **6 (8)** | 8 | 4 | 8 | 10 | 6 | 6 | 5 |
| Audio | 3 | 3 | **4 (6)** | 6 | 3 | 7 | 9 | 5 | 5 | 4 |
| Scripting & gameplay | 7 | 8 | **5 (6)** | 8 | 7 | 9 | 9 | 8 | 6 | 6 |
| Game UI | 3 | 3 | **4 (6)** | 9 | 5 | 7 | 8 | 6 | 5 | 6 |
| Editor (authoring) | 9 | 3 | **3 (5)** | 9 | 2 | 9 | 10 | 8 | 7 | 6 |
| Debugging & profiling | 5 | 7 | **3 (new)** | 8 | 5 | 9 | 10 | 7 | 6 | 4 |
| Asset pipeline | 7 | 5 | **4 (5)** | 7 | 5 | 9 | 9 | 7 | 7 | 5 |
| Platform reach | 8 | 4 | **2 (3)** | 8 | 7 | 10 | 9 | 8 | 5 | 5 |
| Networking | 4 | 1 | **0 (0)** | 6 | 3 | 7 | 9 | 6 | 6 | 1 |
| AI / navigation | 3 | 3 | **0 (0)** | 7 | 2 | 8 | 9 | 6 | 6 | 5 |
| Engineering rigor | 3 | 8 | **8 (9)** | 7 | 7 | 7* | 7* | 5 | 6 | 4 |
| Automation / agent API | 1 | 18 | **8 (9)** | 6 | 5 | 7 | 6 | 3 | 5 | 2 |
| Docs & learning | 4 | 5 | **4 (6)** | 9 | 5 | 9 | 8 | 7 | 6 | 6 |
| Ease of use & onboarding | 5 | 4 | **3 (new)** | 9 | 4 | 7 | 5 | 7 | 3 | 5 |
| Production proof & ecosystem | 10 | 5 | **1 (1)** | 8 | 5 | 10 | 10 | 3 | 3 | 2 |
| Licence & cost | 2 | 2 | **9 (new)** | 10 | 10 | 6 | 7 | 6 | 10 | 10 |
| **Total G** | | | **34.8** | 77.7 | 51.1 | 85.4 | 90.5 | 66.0 | 58.7 | 45.0 |
| **Total A** | | | **48.1** | 74.8 | 53.4 | 81.3 | 83.3 | 59.5 | 58.2 | 42.4 |

\* Closed source, estimated. Bevy's physics and networking come from third-party crates, counted as partial. Breccia's
Licence & cost is 9 rather than 10 only because exports ship no third-party notices.

The new totals are not directly comparable with the old ones, which used other weights. For Breccia, the old dimensions on
the old weights give G 46.5 → 35 and A 63 → 50. Breccia's G rank is robust: it stays last even if every one of its cells
is raised by 1. On A it sits between Fyrox and Bevy; the same +1 would pass Bevy but not Flax or Godot.

**Why each Breccia score changed**

| Dimension | Move | Reason |
|---|---|---|
| Rendering | 6 → 4 | Solid core, but no GI, reflections, fog, decals, custom shaders or texture streaming. Sponza shows it plainly. Peers shipped HDR output, SSR and path tracing |
| Scalability | 4 → 3 | LOD and batching work, but linear CPU cost per entity, no occlusion culling, no GPU memory management |
| Physics | 6 → 5 | Good integration and characters; silent contact loss, an aborting allocator, no heightfields, vehicles or interpolation |
| Animation | 8 → 6 | Runtime depth is real, but no clip compression or curve editor, and no real content has exercised it. Godot is ahead |
| Audio | 6 → 4 | Tidy mixing and effects, but no voice management, no OGG decode, no HRTF or occlusion |
| Scripting | 6 → 5 | Wide API and hot reload; one interpreted language, JSON round trips, hang exposure |
| Game UI | 6 → 4 | Good widgets over a debug-UI renderer, with no shaping, RTL, fallback fonts or IME |
| Editor | 5 → 3 | Clean undo and selection, but no search, copy/paste, file drop, timeline or standard tools. Debugging now scored apart |
| Asset pipeline | 5 → 4 | Excellent identity and archives; glTF only, no import settings, no cook step |
| Platform reach | 3 → 2 | Host-only export, Vulkan only, nothing signed, no release to download |
| Rigor | 9 → 8 | Still excellent and ahead of Godot on determinism, but real GPUs and the editor are untested in CI, and a whole class of scale failure went unnoticed |
| Automation | 9 → 8 | Hangs, no headless pixels, no auth, four GUI-only actions. Still the best deterministic loop, but unproven with outside agents |
| Docs | 6 → 4 | Accurate reference, zero tutorials and no editor manual |

Scores that moved elsewhere: Godot rendering (7 → 8), animation (7 → 8) and automation (4 → 6); Unity automation (6 → 7);
Bevy automation (4 → 5). Godot's platform score fell a point, as did Bevy's: both lack official console routes.

## 10. Recommended priorities

This order is deliberately different from `ROADMAP.md`, which should adopt these items, since it omits most of them. New
rendering features come after correctness and proof.

1. **Fix the five defects (days).** Report physics overflow and raise or configure the limits, use a growable temp
   allocator, add a Lua instruction hook with a time budget, make the editor server reject non-JSON connections and require
   a token, and add an OGG decoder or drop the format. Add a test past each limit.
2. **Publish `v0.1.0`, unsigned, with a tutorial (days).** Binary downloads, a getting-started guide, a starter template and
   a default scene with ground and light. Replace "production-grade" in the README with what is true today.
3. **Prove the agent claim (a week).** Run a CEGame-Bench-style task set on Breccia against Godot and Unity. It is the only
   way to learn whether the differentiator works. Add headless rendering to `render.*` and return images over MCP first.
4. **Ship one real 3D game end to end (2–4 weeks).** About 30 minutes of content with real art, exported on all three
   OSes. This is still the cheapest way to find what unit tests cannot, and it would exercise the animation stack for the
   first time.
5. **Fix scale before adding features.** Cached transforms, a GPU suballocator and budgets, a realistic benchmark scene,
   then occlusion culling.
6. **Close the two workflow gaps:** Lua stack traces, debugger (DAP) and profiler; editor search, copy/paste and file drop.
7. **Custom shaders, then fog and one GI technique, then reflection probes and SSR.** Custom shaders first, since teams
   cannot work around their absence.
8. **Navigation** (Recast/Detour behind a component, exposed to Lua and automation).
9. **Decide scope in writing:** networking, mobile, web and consoles are either roadmapped or declared non-goals, so that
   the positioning is honest. A recommended scope statement: "a desktop 3D engine built for agent-driven development".

**Adopted on 2026-11-08.** `ROADMAP.md` and `ROADMAP-SESSIONS.md` now carry these items and the scope statement, with two
decisions of the owner: networking is in scope and comes first, ahead of item 2; consoles are a non-goal, mobile and web
come after desktop is proven, and XR and a 2D pipeline are not planned. Item 1 shipped in PR #74.

## 11. What would change these scores

| Event | Effect |
|---|---|
| Items 1–2 done | Automation back to 9, ease of use 5, docs 5: A total up about 3 |
| A shipped game | Production proof 3; every runtime score gains evidence, likely up or down by 1 |
| An agent benchmark showing a clear win | Maturity 3.5 in the niche, and a real public claim |
| Items 5–7 done | Rendering 6, scalability 5, debugging 6, editor 5: G total up about 6.5 |

## 12. Dated items

These will age within weeks: O3DE 26.10 (11 Nov), Unreal 5.9 preview (week of 9 Nov, with a web target and AI workflows
reported), Godot 4.8, Unity 6.7 LTS and the Unity 7 preview (December). UE6 slides reported from Unreal Fest Tokyo (3 Nov)
mark Blueprints and Actors for removal in favor of Verse and Scene Graph; that is secondary reporting, not an Epic
document, and would be a large migration for Unreal users if confirmed. Unity's built-in pipeline and HDRP deprecations
point at a similar transition.

## 13. Unconfirmed

Not stated as fact above, or stated with the caveat shown:

- **Unreal:** `unrealengine.com` blocks fetches, so licence terms and news are search snippets of Epic's pages; Xbox GDK
  version; DLSS, FSR and XeSS plugin details; HDR output; AI Assistant status in 5.8; install size (forum reports only).
  Unreal 5.8's date differs by source (17 or 23 June).
- **Unity:** install size (community reports only); 6.7 LTS date; whether a 6.8 exists; humanoid retargeting at source;
  first-party spring bones and motion matching; large-world support.
- **Godot:** Steam counts; Jolt vehicle behaviour; console-provider Switch 2 coverage.
- **Bevy:** whether occlusion culling has left experimental status.
- **Fyrox, Flax, O3DE:** several negatives rest on source-tree checks, not on full searches, and Flax's HDR, XR and
  marketplace status is unknown. Their columns are the least certain here.
- **Agent matrix:** "not found" means not found in the docs read. The Unity CLI notes were read only in part, and hang
  and authentication behavior of competitors was not tested.
- **Market:** all benchmark papers are preprints. The widely quoted 41/31/26 Video Game Insights "revenue" split is units
  sold, per its PDF. No measured time-to-first-playable data exists for any engine.
- **Breccia:** nothing here was measured on Windows, Linux or a discrete GPU, and a browser never exercised defect 4.

## Sources

Breccia: this repository at `4f823ab`; CI run 42032205003; [Tested hardware](TestedHardware.md).

**Unity:** [6.6 announcement](https://discussions.unity.com/t/unity-6-6-is-now-available/1735357) ·
[6.7 beta](https://discussions.unity.com/t/unity-6-7-beta-is-now-available/1736830) ·
[Unity 6 support](https://unity.com/releases/unity-6/support) · [Unity 7](https://unity.com/releases/unity-7) ·
[Seoul keynote recap](https://unity.com/blog/unite-seoul-keynote-2026-recap) ·
[Single render pipeline](https://discussions.unity.com/t/the-path-to-a-single-render-pipeline-in-unity-7/1737908) ·
[System requirements](https://docs.unity.com/en-us/engine/6000.6/manual/get-started/install-and-upgrade/getting-started-installing-unity/system-requirements) ·
[Pricing](https://unity.com/products/pricing-updates) · [Unity AI](https://unity.com/features/ai) ·
[Unity CLI](https://docs.unity.com/en-us/unity-cli) ·
[MCP deprecation](https://docs.unity.com/en-us/unity-cli/replace-mcp-server-unity-cli) ·
[GPU culling](https://docs.unity3d.com/6000.0/Documentation/Manual/urp/gpu-culling.html)

**Unreal:** [5.8 release notes](https://dev.epicgames.com/documentation/en-us/unreal-engine/unreal-engine-5-8-release-notes) ·
[Hardware specifications](https://dev.epicgames.com/documentation/en-us/unreal-engine/hardware-and-software-specifications-for-unreal-engine) ·
[Licensing](https://www.unrealengine.com/license) ·
[Automation tests](https://dev.epicgames.com/documentation/en-us/unreal-engine/automation-test-framework-in-unreal-engine) ·
[Gauntlet](https://dev.epicgames.com/documentation/en-us/unreal-engine/gauntlet-automation-framework-in-unreal-engine) ·
[Puget Systems MCP hands-on](https://www.pugetsystems.com/blog/2026/07/09/unreal-engine-mcp-hands-on-testing-ai-inside-the-editor/) ·
[Forum: MCP connection report](https://forums.unrealengine.com/t/5-8-experimental-modelcontextprotocol-mcp-server-instantly-drops-connections/2729488)

**Godot:** [Releases](https://godotengine.org/releases/4.7/) ·
[4.8 beta 1](https://godotengine.org/article/dev-snapshot-godot-4-8-beta-1/) ·
[Feature list](https://docs.godotengine.org/en/stable/about/list_of_features.html) ·
[Priorities](https://godotengine.org/priorities/) ·
[Occlusion culling](https://docs.godotengine.org/en/stable/tutorials/3d/occlusion_culling.html) ·
[`SkeletonModifier3D`](https://docs.godotengine.org/en/stable/classes/class_skeletonmodifier3d.html) ·
[Command line](https://docs.godotengine.org/en/stable/tutorials/editor/command_line_tutorial.html) ·
[Community poll 2026](https://godotengine.org/storage/data/GodotCommunityPoll2026.csv) ·
[4.7.2 downloads](https://github.com/godotengine/godot/releases/tag/4.7.2-stable)

**Bevy and Fyrox:** [Bevy 0.20](https://bevy.org/news/bevy-0-20/) ·
[Migration guide](https://bevy.org/learn/migration-guides/0-19-to-0-20/) ·
[Sixth birthday](https://bevy.org/news/bevys-sixth-birthday/) ·
[Examples](https://github.com/bevyengine/bevy/blob/main/examples/README.md) ·
[Fyrox 1.0](https://fyrox.rs/blog/post/fyrox-game-engine-1-0-0/) · [Fyrox book](https://fyrox-book.github.io/introduction/introduction.html)

**Others:** [O3DE 26.05 notes](https://www.docs.o3de.org/docs/release-notes/2605-0-release-notes/) ·
[O3DE platforms](https://docs.o3de.org/docs/welcome-guide/supported-platforms/) ·
[Flax 1.13](https://flaxengine.com/blog/flax-1-13-released/) · [Flax platforms](https://docs.flaxengine.com/manual/platforms/index.html) ·
[Flax licensing](https://flaxengine.com/licensing/) ·
[Stride platforms](https://doc.stride3d.net/latest/en/manual/platforms/index.html) ·
[Defold 1.14](https://defold.com/2026/10/12/Defold-1-14-0/) ·
[Wicked Engine](https://github.com/turanszkij/WickedEngine) ·
[Can AI Make a Game Engine?](https://www.youtube.com/watch?v=R_uf5OfMGio) ·
[Open-source engine list](https://github.com/bobeff/open-source-engines)

**Agent tooling and research:** [Roblox Studio MCP](https://create.roblox.com/docs/studio/mcp.md) ·
[CEGame-Bench](https://arxiv.org/html/2610.26098v1) · [UnityBench-Mini3D](https://arxiv.org/html/2610.35980v1) ·
[CraftBench-UE](https://arxiv.org/abs/2609.23142) · [GameLogicBench](https://arxiv.org/abs/2609.21562) ·
[unity-mcp](https://github.com/CoplayDev/unity-mcp) · [godot-mcp](https://github.com/Coding-Solo/godot-mcp) ·
[godot-ai](https://github.com/hi-godot/godot-ai) · [mcp-for-blender](https://github.com/ahujasid/mcp-for-blender)
