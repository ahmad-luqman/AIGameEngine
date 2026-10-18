# Roadmap Sessions

The open `ROADMAP.md` items grouped into single-session milestones, sorted in order of implementation. The order
follows the roadmap's priorities (P1 → P2 → P3) and is adjusted so that work a later session depends on comes first.
Based on `main` at `fdc70d7` (October 2026, after PR #35 shipped the engine-internals items).

Size estimates: **S** = a day or less, **M** = a few days, **L** = a week or more.

| # | Session | Items (size) | Why here |
|---|---------|--------------|----------|
| 1 | **Character movement** ✅ | Inverse kinematics: two-bone legs and arms, look-at/aim chains, foot placement (M); animation layers, per-joint masks, additive clips (M) | Shipped. |
| 2 | **3D platformer sample** | Platformer built through the automation API with a ctest gameplay test (M); spring bones on its hair or cape (S) | Uses session 1 together with the character controller, so it checks whether the API is enough for real character movement. |
| 3 | **Diagnostics** | Play-mode stats overlay (S); timing for the editor's ImGui pass and the runtime blit (S); benchmark scene with frame-time thresholds on CI (M) | The roadmap says the GPU timings belong in the overlay. The benchmarks record a baseline before session 4. |
| 4 | **Throughput** | GPU instancing and draw batching (M); crowd performance: cached joint bounds, animation LOD, poses evaluated on the job system (M) | Session 3's benchmarks measure the gains. The crowd work finally uses the new job system. |
| 5 | **Game UI** | UI follow-ups: `UI.MeasureText`, clipping, rect rotation/scale, sprite sheets, viewport handles (S); more widgets: text input, dropdown, scroll view, vertical slider, explicit navigation, input-blocking panels, focus ring (M) | Clipping has to land first because scroll views need it. |
| 6 | **Effects polish** | Particle follow-ups: soft particles, rotation, random start values, multi-key curves, lit particles, edit-mode preview (M); audio effect follow-ups: one-shot options, reverb zone component, high-pass/echo/distortion (S) | Two independent polish items of similar size. |
| 7 | **Asset pipeline I** | Stable asset IDs with `.meta` UUIDs (M); mesh optimization and LODs with meshoptimizer (S) | Material assets and the packed archive both reference assets by UUID, so IDs come first. |
| 8 | **Asset pipeline II** | Texture compression to KTX2/BC7 (M); glTF extensions (M) | `KHR_texture_basisu` needs the KTX2 path. Clearcoat and transmission extend the material model before session 9. |
| 9 | **Material assets** | `.bmaterial` files with thumbnails (L) | Needs stable IDs and the extended material model from sessions 7 and 8. |
| 10 | **Animation content** | Morph targets (M); retargeting, e.g. sharing a Mixamo library across characters (M) | Both change skinning and import. Doing them before TAA means motion vectors handle deformed meshes from the start. |
| 11 | **TAA** | Temporal anti-aliasing (M) | Needs previous-frame data for instanced draws (session 4) and for skinned and morphed meshes (session 10). |
| 12 | **Animation authoring** | Animation graph editor for `.banimgraph` (M); property animation through the `ComponentRegistry` (M) | Both are authoring tools built on the automation and registry paths. |
| 13 | **Ragdolls** | Ragdolls from the skeleton, with a powered-ragdoll follow-up (M) | Physics-heavy: it moves the `PhysicsDeterminism` hashes, so it's best kept in its own PR. |
| 14 | **Prefabs** | Prefab editing and per-instance overrides (L); native file dialogs (S) | The large P2 editor item. The dialogs fit its open and save flows. |
| 15 | **Shipping I** | Project icon, version and company (S); platform packaging for `.app`, `.exe` and AppImage (M); tag-triggered release pipeline (S) | Packaging uses the icon and version, and the pipeline uses the packaging. |
| 16 | **Shipping II** | Packed `.bpak` archive (M); crash reporting (M) | The archive indexes compressed textures by UUID, which needs sessions 7 and 8. |

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
