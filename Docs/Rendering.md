# Post-processing and graphics preferences

Add a `PostProcess` component to an entity through the inspector, `component.set`, or Lua. It controls
manual exposure, tonemapper (`None`, `Reinhard`, `ACES`, `AgX`), histogram auto exposure, SSAO, bloom,
FXAA and TAA. The enabled component with the highest `Priority` wins globally; ties use the lowest entity UUID.
`Priority` is a nonnegative integer (default 0); `Enabled` defaults true. There are no spatial volumes or blends. Without an enabled component, legacy `Renderer` exposure,
tonemapper and SSAO settings remain the fallback, preserving existing scenes and images. The editor
hides those fallback controls while a component is active. Shadows and FSR remain scene renderer settings.

The order is HDR opaque/sky → optional TAA resolve → transparency/particles → histogram metering and HDR bloom
→ exposure, tonemapping and sRGB encoding → optional FXAA → optional FSR → final dithering → overlays/UI.
SSAO remains a pre-lighting effect. Debug views bypass TAA (including jitter), bloom, exposure, FXAA and FSR. All filter taps
clamp to the active render area, including odd and one-pixel dimensions; dynamic resolution never samples
unused target pixels. FXAA works on perceptual color: before FSR when enabled, otherwise its filter taps
also resample directly to output resolution.

`Exposure` is a positive linear multiplier (1 leaves brightness unchanged). With `AutoExposure`, it
also multiplies the metered exposure. A 256-bin GPU histogram meters pre-bloom HDR luminance over -12 to
+16 EV; out-of-range finite luminance clamps into the edge bins, while black and nonfinite luminance are excluded. The geometric mean targets middle gray (0.18),
with `ExposureCompensation` in stops and the result clamped to `MinExposure`/`MaxExposure`.
An all-black frame targets 1 within those bounds. `AdaptationSpeed` is an exponential rate per second;
0 holds the previous exposure after initialization. Delta time is explicit and clamped to [0,1] seconds.
First use, scene replacement/load, component selection changes and re-enabling auto exposure start at the
metered target. History belongs to each renderer; editor screenshots and picks reset it for deterministic
captures. No histogram readback or CPU stall is used.

Bloom extracts values above `BloomThreshold` in linear HDR, downsamples through up to six half-sized
levels, then combines normalized tent upsampling with `BloomScatter` (0 keeps finer detail, 1 favors
broad glow). `BloomIntensity` scales its contribution before tonemapping. Bloom and FXAA default off;
auto exposure defaults off. New component values are strictly validated atomically; malformed values
written directly by C++ fail the frame with a diagnostic. Legacy fallback values retain their old ranges.
Enabling bloom, auto exposure, FXAA, or a TAA reset-frame FXAA pass allocates the shared HDR/exposure/bloom resources; FXAA
adds its own input and caches pipelines for both final and FSR output formats. Resources remain cached
until targets change. Allocation failure fails the frame/capture in every build, including Dist, instead
of silently changing the authored look. Failed resource paths log once until post-processing is disabled
and re-enabled, or the targets change. TAA reset-frame FXAA follows the same failure policy: toggle TAA
off/on or resize to retry after an allocation failure; it never silently removes AA. Invalid component diagnostics name the entity and repeat only
when the validation error or selected entity changes. Disabling that component recovers the legacy path. HDR work has a `PostProcess` GPU timing; FXAA is in `Tonemap`.

Component defaults and inclusive ranges:

| Field | Default | Range / values |
|---|---|---|
| Enabled / Priority | true / 0 | boolean / 0–4294967295 |
| Tonemapper | ACES | None, Reinhard, ACES, AgX |
| Exposure | 1 | 0.0001–65504 |
| AutoExposure | false | boolean |
| MinExposure / MaxExposure | 0.0625 / 16 | 0.0001–65504; min ≤ max |
| ExposureCompensation | 0 | −16–16 stops |
| AdaptationSpeed | 3 | 0–100 per second |
| SSAOEnabled | true | boolean |
| SSAORadius / SSAOIntensity / SSAOBias | 0.5 / 1 / 0.025 | 0.0001–100 / 0–10 / 0–1 |
| FXAAEnabled / TAAEnabled / BloomEnabled | false / false / false | boolean |
| BloomThreshold / BloomIntensity / BloomScatter | 1 / 0.1 / 0.7 | 0–65504 / 0–10 / 0–1 |

Project defaults include `VSync`, `RenderScale`, `ShadowQuality`, `SSAOQuality` and the existing dynamic
resolution settings. Set them with `project.set {"graphics": {...}}`, using those PascalCase keys.
`project.info.graphics` returns the complete defaults. Existing lower-case render-scale/dynamic-resolution
parameters remain supported; setting the same property in both forms is rejected atomically.
`scene.settings` warns when an active PostProcess entity overrides the changed legacy keys.
Quality levels are `Off`, `Low`, `High`: SSAO uses 0/8/24 samples, while shadows use no maps, a single
hardware comparison, or the existing directional PCSS/local 3×3 filters. Quality controls cap authored
settings; `High` does not force disabled shadows or SSAO on. Shadow-map resolution is unchanged.

Lua `Graphics.GetSettings()` and `Graphics.SetSettings({...})` read/patch a Play-session copy. Changes
apply on the next render, survive `Scene.Load`, and reset from project defaults on the next Play.
They do not save the project; games can persist preferences explicitly through `Storage`. Runtime CLI
scale/dynamic/VSync options initialize the session before scripts start and remain explicit host
overrides at rendering time. Lua may change the stored preferences, but cannot undo a supplied CLI flag.
Unspecified settings remain script-controlled. Editor VSync is restored after Play; project scale/dynamic menus are disabled during Play.
Offscreen/headless sessions retain the same preference values even though VSync has no display effect.

`PostProcessGPU` tests bloom spread and thresholding, FXAA edges, exposure adaptation/reset, debug bypass,
resize, tiny targets and dynamic allocations with/without FSR, with Vulkan synchronization validation.
Headless tests cover selection, serialization, atomic validation and Lua session lifetime.

## Temporal anti-aliasing

Enable `PostProcess.TAAEnabled` through the component API or inspector. TAA takes precedence over FXAA
once history is valid and runs before bloom, exposure, tonemapping and FSR 1. It operates at the current
render resolution, including supersampling and dynamic resolution. Transparency and particles composite
with the current jittered sample after the resolve; they do not enter temporal history. Overlays and game UI
are drawn after post-processing and never accumulate. Debug lines use the unjittered camera; selection
outlines follow the current jittered picking silhouette. Reset or environment-rebake frames (including captures) automatically
use FXAA until temporal samples become available, even when `FXAAEnabled` is false.

Each renderer has an eight-sample Halton projection jitter and its own HDR color and R32F depth histories.
The opaque/masked depth prepass writes previous-frame UV, depth and validity, using each entity/mesh node's
previous model transform, joint palette and strongest drawn morph weights. Instancing order does not identify
history. New or previously invisible geometry and replaced model assets reject history. Sky reprojection uses
camera rotation. The resolve rejects out-of-bounds and depth-mismatched history, clamps history to the current
3×3 color neighborhood and reduces history weight for fast motion. This reduces edge shimmer, including
SSAO and opaque specular highlights; it can soften small moving detail.

History resets on scene/play revision, primary camera identity changes, selected PostProcess entity identity changes, any projection matrix
change (including animated FOV), and active render dimension changes (including dynamic resolution),
TAA/debug toggles, failed frames, and camera jumps over 10 world units or turns over 60 degrees.
An environment rebake rejects that frame's accumulated color because lighting has changed. Editing other
fields on the selected PostProcess does not reset history.
Hosts should call `SceneRenderer::ResetTemporalHistory` for smaller intentional cuts within the same
camera; no Lua cut hook is provided. Primary scene-camera switches reset automatically. `ResetExposure`
also resets TAA for repeatable captures. Only successful submissions advance the sample sequence.
Changing TAA or its debug-view bypass rebuilds the scene targets and pipelines, waits for the device,
and resets exposure, picking and FSR resources. Disabling TAA releases its color/depth histories and motion target. Previous-pose vertex work runs only
in the TAA prepass; shared skins retain one palette snapshot and upload per entity skin. TAA resolve cost
is included in the `Lighting` GPU timing.
`render.stats` reports `taaActive` and `taaHistoryValid`. The optional `TemporalAAGPU` test checks previous
coordinates against independently moving instanced geometry and CPU-skinned/morphed triangles, numeric
resolve and rejection, gradient reprojection, history replacement, spatial AA fallback and shimmer reduction.
It also injects allocation failures through a test-only device proxy and verifies clean abort/recovery,
with Vulkan synchronization validation enabled. Headless `TemporalHistoryTests` cover the jitter and camera lifecycle.

# Clustered forward lighting

The PBR pass uses a 16×9×24 camera-frustum grid. Perspective cameras have logarithmic depth slices;
orthographic cameras have linear slices, including negative near planes. A compute workgroup builds
each cell's ordered light list before lighting. Point and spot lights use conservative range spheres;
additional directional lights appear in every cell. The primary sun retains its separate cascade path.

Opaque, masked and transparent materials use the same lists. Culling covers the entire frustum rather
than prepass depth, so transparent surfaces still receive their lights. Fragment lookup uses the current
render area (top-left origin), independent of backing target size, render scale and output size.
World-space planes also support translated, rotated and scaled camera transforms. Edge depth cells
are unbounded on their outer side, matching clamped fragment lookup even if camera metadata differs
from the projection. Invalid depth intervals are logged and replaced by [0.1, 1000].

The scene buffer holds up to 256 lights, in addition to the primary sun. Each cell has 256 index slots plus a count: about
3.39 MiB for lists per renderer. Lists and the 16 KiB light-record prefix share a single storage
buffer, keeping the frame layout at four storage-buffer descriptors (Vulkan core's per-stage minimum). There is no smaller per-cell cap that could silently lose lights. Lists reference the scene light records, including their shadow-map indices; stable order preserves
floating-point accumulation for exact image comparisons. Empty counts are rewritten every frame; unused
indices are never read. Resources allocate during renderer initialization; creation failures log their
stage and fail initialization. The compute pass reads the record prefix and writes only the list suffix; nvrhi tracks compute-write
to fragment-read transitions.

Clustering is automatic and adds no serialized settings. Dense overlapping lights can cause a cell to visit all 256 lights; conservative spot spheres trade extra candidates for reliable coverage. The fixed grid avoids
per-pixel iteration over lights in unrelated cells, but cannot guarantee faster rendering for every
scene. The **Clusters** GPU timing appears alongside **Lighting** in the statistics and runtime
logs. Dynamic resolution includes this recurring cost in its frame budget, even though the grid size
does not change with render scale.
`SceneRenderer::SetClusteredLighting(false)` selects the all-lights reference path for C++ diagnostics
and image/performance comparisons. `SetVisualizeLightCount(true)` replaces PBR color with the fraction
of the light budget actually visited by each fragment, making list consumption observable.

Headless tests cover depth partitions and conservative cell boundaries. `LightClustersGPU` compares
GPU lists with CPU plane tests using a narrow floating-point tolerance band, checks full-capacity and empty/reused buffers, and compares rendered
point/spot/directional lighting with the all-lights reference. It exercises local shadows, transparent
materials, camera changes, odd viewports, reduced render areas, dynamic resolution and moving lights.
Sparse influence spheres test tile/depth boundaries and the per-fragment count diagnostic; a one-cell
sphere pins x/y culling tightness. Both builder and scene over-budget cases are tested. The GPU test
requests synchronization validation with `VK_LAYER_ENABLES` as well as core validation on MoltenVK
locally and lavapipe in CI; golden comparisons cover the feature scene.

# Spatial upscaling (FSR 1)

Scene settings select the upscaler through the editor's Renderer settings or automation:

```sh
basalt --project MyGame scene.settings '{"Renderer":{"FSR1Enabled":true,"FSR1Sharpness":0.2}}'
```

`FSR1Enabled` defaults to false, preserving bilinear upsampling for existing scenes. When enabled,
FSR 1 replaces bilinear upsampling below native resolution. `FSR1Sharpness` is RCAS attenuation in
stops, from 0 (strongest) to 2 (weakest); default 0.2. It is not a 0–1 strength slider. Invalid values
are rejected by the shared scene reader. Both settings are saved with the scene and copied for Play.
The selection stays a boolean while FSR 1 is the only optional upscaler; a temporal upscaler and its
history/jitter settings will need a separate design.

Use the project's `RenderScale` or runtime `--render-scale`, together with the existing dynamic
resolution controls. The **current render area** determines whether FSR runs: native or larger areas
and SSAO/Normals/Depth debug views bypass it. A dynamic run with `RenderScale: 1.5` can activate FSR
when its current scale drops below 1.
UI, text, debug lines and gizmos are drawn afterward at output resolution. FSR 1 is a spatial upscaler;
it does not provide temporal antialiasing (enable `PostProcess.TAAEnabled` for TAA).

The complete FSR path is shadows → depth/normals → SSAO → clustered lighting/sky → optional TAA → transparency/particles → optional
HDR bloom/auto exposure → tonemapping/sRGB → optional FXAA → EASU → RCAS → 8-bit dither → overlays.
Intermediate color is stored in RGBA16_FLOAT without sRGB decoding; FSR expects normalized perceptual
color. The extra targets allocate on first use and remain cached while FSR is off and across dynamic
render-area changes. Viewport, configured render-scale, active dynamic-mode, and TAA/debug-bypass changes release them.
The FSR input allocation is capped at output size even with a supersampled dynamic ceiling, since
FSR never reads a larger render area. The upscaler also checks its full configuration and rebuilds
automatically when dimensions, output format, device, vertex shader or sampler change. The tonemap pipeline belongs to `SceneRenderer`.
GPU cost is included in the existing Tonemap timing and dynamic-resolution feedback.

A resource failure fails the frame and capture rather than silently changing image quality. Its log
identifies the allocation stage and target/output sizes once per failed configuration. The editor shows
an error banner over the viewport and a warning in Statistics. Disabling and re-enabling FSR retries
failed preparation and reports any new failure; configuration changes and resize also permit retry.
`render.stats.fsr1Active`, the Statistics panel and the runtime's `fsr1=0|1` log field report whether EASU
and RCAS actually ran in the last frame; failed frames never claim active FSR.

Dynamic resolution renders into the top-left of larger textures. EASU receives both the current render
size and allocated texture size; every individual input tap is clamped to the render area, including
one-pixel dimensions. RCAS reads the full output-sized EASU image. Neither pass can read unused texels.

AMD's unmodified MIT-licensed FSR 1.0.2 headers are fetched at the exact commit in
`cmake/Dependencies.cmake`, with sample/media submodules disabled. The headers are needed at build
time, and AMD-derived SPIR-V ships embedded in every engine binary. Retain the AMD notice in `LICENSE`
when distributing those binaries. See [AMD's FSR 1 source and license](https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/a21ffb8f6c13233ba336352bdff293894c706575)
and [integration guidance](https://gpuopen.com/fidelityfx-superresolution/).

`FSR1GPU` tests synthetic inputs, poisoned unused texels, tiny render areas, resource reuse and color
preservation, cache reconfiguration, resize, failure retries and sharpness sanitizing with Vulkan
validation. `FSR1Render` exercises scene settings, odd fixed/dynamic sizes, native-image error bounds,
RCAS contrast direction, positive activity statistics and bypass paths on the static feature fixture.
Both run in the lavapipe CI GPU job.

# Skinned meshes and animation

`AnimatorComponent` renders all mesh nodes in the glTF default scene under one entity. The asset owns
its skeletons, inverse binds, and immutable animation clips; `AnimationSystem` owns independent
per-entity clocks, local poses and composed node transforms while a scene plays or simulates. The
component registry exposes setup through scene files, prefabs, the inspector, Lua and automation.
`asset.import_model` creates an Animator root for animated/skinned assets; static assets retain their
editable entity hierarchy. A Material on the root overrides imported materials (with its inline values or a material
asset; see [AssetPipeline.md](AssetPipeline.md#material-assets)). Editor picking,
focus and selection outlines use the current animated model bounds. The inspector and `scene.info`
report missing clips/assets and Mesh/Animator combinations that can draw unexpected geometry.

TRS tracks support STEP, LINEAR (shortest-arc quaternion slerp), and CUBICSPLINE (time-scaled Hermite
with normalized quaternion results). Missing channels retain authored node values. Blends and
crossfades mix local translation, rotation and scale before composing the hierarchy. The Lua API
section “Skeletal animation” defines playback and transition behavior.

Vertices carry four joint indices and normalized nonnegative weights. Vertex-stage linear blend
skinning uses `jointGlobal * inverseBind`; as glTF requires, the skinned mesh node transform does not
move the skin a second time. The Basalt entity world transform applies after skinning. Rigid animated
nodes use their evaluated node transform. Normals use a scale-normalized cofactor form of the inverse transpose of the blended linear
transform, preserving rotation even for very small rig units; tangents use its linear transform and preserve reflection handedness in the skin transform (negative entity scale retains the existing static-mesh tangent convention). Degenerate skin
transforms have a finite normal fallback. The same palette and position operation feed PBR, depth/
normal prepass, opaque and masked cascades, and point/spot shadows. Transparent meshes use skinning
but retain the renderer's no-transparent-shadow policy.

Palettes occupy records in the existing draw SSBO, with per-draw offset/count metadata. This keeps
the frame at four storage-buffer bindings and supports independent instances without mutable GPU
asset data. Mesh nodes sharing a skin reuse one palette per instance, uploaded only if a camera or shadow
pass needs it; `render.stats.skinMatrices` reports the uploaded matrix count. Vertex stride is 88 bytes for both rigid and skinned geometry. Palette upload storage grows
with demand and uses nvrhi resource lifetime tracking; allocation failure fails the frame explicitly.
Camera and shadow culling transform cached joint influence bounds in O(influencing joints); see
Draw throughput below. Exact per-vertex posed bounds remain for editor framing/selection and as
the reference/fallback for geometry without cached influences.

## Morph targets

glTF morph targets blend in the vertex shader (`Include/Morphing.glsl`) before skinning, in every mesh pass (PBR,
depth/normal prepass and picking, opaque and masked shadows, which read positions only). Each model's position and
normal deltas live in one RGBA32F texture (two texels per target and vertex, rows up to 4,096 texels wide, at most 2,048
rows) bound as set 2 with a point sampler, visible to the vertex stage only: a fifth storage buffer would exceed the
portable four per stage. Meshes without targets bind a one-texel empty texture, so every mesh pipeline has the same
layout; the opaque shadow pipelines leave set 1 empty (nvrhi fills the hole). A draw's nonzero weights are
`(target, weight)` pairs packed 16 to a record into the draw SSBO alongside the palettes, ahead of the draw records,
uploaded once per visible mesh node like a palette; `DrawData.Morph` holds the first record, the pair count, the delta
base (minus 2 x the submesh's base vertex in texels, which `gl_VertexIndex` adds back) and the texels per target, so
instanced draws of one mesh still batch with different weights. A draw blends its 32 strongest weights, warning once
per entity and mesh node until a frame is back within the limit; `render.stats.morphWeights` counts the uploaded pairs and
`morphWeightsDropped` the rest. Tangents are re-orthogonalized against the morphed normal. Culling widens each submesh's
bounds (or, for skinned meshes, each joint influence box) by the weighted per-target delta ranges cached at import, so
morphed geometry never pops; editor framing, selection boxes and vertex snapping morph vertices exactly (a MeshComponent
at its mesh's default weights). Morphed submeshes get no reduced LODs. Animators draw their pose's weights (see the Lua
API's Morph targets). The delta texture counts toward `memory.stats` GPU buffers, and retargeted clip sets toward its
assets.

Import rejects malformed weights/joints, invalid track shapes/times/quaternions, cyclic hierarchies,
TRS-animated matrix nodes and duplicate node/path targets. Morph weight channels import as weights tracks (their
widths match the node's target count; channels of meshes whose targets were dropped are skipped with a warning); pointer
channels are warned about and skipped; extra influence sets retain static geometry with a warning and disable
Animator playback for that asset. Sparse accessors, normalized unsigned byte/short weights and
normalized signed/unsigned byte/short rotation keys are supported. Animated/skinned models are limited
to 65,536 nodes per model; each skin is limited to 65,536 joints. Other limits are 4,096 skins
and clips, 65,536 skin-joint references summed over active mesh nodes, and 64 MiB of decoded skin/animation
floats (repeated sampler references count separately). The combined joint-reference limit bounds
work for assets that reuse a large skin across many nodes.
Non-finite evaluated hierarchies or palettes log once per failure episode and display the validated rest pose.
A skin may use the first N matrices of a larger inverse-bind accessor; absent inverse binds are
identity. Duplicate clip names get deterministic suffixes. Morph targets are described above; clip-library
retargeting is in Docs/ScriptingAPI.md ("Clip libraries and retargeting"). State machines, per-state root motion, bone attachments, animation events and
blend spaces are described in Docs/ScriptingAPI.md ("Skeletal animation").

`AnimationTests` covers import, interpolation, space conventions, playback, blending, blend spaces, animation
events, bone attachments, component/Lua
validation and lifecycle; the Khronos Fox fixture verifies Survey/Walk/Run from a real exported rig.
The feature-test driver exercises every Lua operation on an off-camera rig. `AnimationGPU` compares
GPU skinning with independently deformed CPU geometry for opaque/masked/blended materials, lit/
normal/depth outputs and directional/point/spot shadows, with Vulkan validation required. `MorphTargetTests` covers
morph import, limits, sampling, layers, script weights and bounds; `RetargetingTests` covers joint matching, baking,
clip sets, persistence and diagnostics. `AnimationGPU` also compares GPU morphing (skinned and static, past one weight
record and past the per-draw limit) with CPU-morphed geometry.

## Editor picking and selection

Editor renderers opt into `SceneRendererSpecification::Picking`; runtime renderers allocate no ID
or picking-depth targets. Opaque and alpha-masked meshes write an R32_UINT entity token beside normals
in the depth prepass, using the same material evaluation and vertex skinning as lighting. Tokens are
frame-local and map back to full 64-bit UUIDs; all submeshes and animated nodes of an entity share one
token. Zero is background. Transparent meshes then write the nearest surface with alpha at least 0.1
into the ID target, using a copy of scene depth so SSAO, lighting and overlays retain their original
depth. Particles and game UI do not write mesh IDs.

`PickSurface(x,y)` copies one ID texel and its D32 depth in one submission, then waits for the
whole device to become idle. `Pick(x,y)` returns just the UUID from that sample. Coordinates
are output-image pixels, top-left origin; they map to the active render area, including dynamic
resolution inside larger allocations. A failed/unrendered/resized frame yields no result, distinct
from background. `InvalidatePicking()` discards the mapping on scene replacement. The editor command
renders the current scene/camera on demand into the capture renderer at the viewport’s current
render scale (including dynamic reduction), preserving pending debug lines
and the displayed viewport texture. Thus same-frame mutations and minimized windows cannot use stale
IDs. A vanished nonzero UUID is an error, not background. The scene view keeps the editor camera in
every mode, so picking also works in Play.

Selection is a visible-surface outline: selected tokens carry a reserved high bit, stripped during
UUID readback. One 3x3 edge pass on that bit outlines the union of all selected surfaces, independent
of selection size, and writes an amber border
after tonemapping, FSR and debug lines, before the game/editor UI composite. It marks visible
silhouettes and occlusion boundaries, without an x-ray outline around fully hidden geometry. The
border is one output pixel on each side of an edge, independent of render scale. Samples outside
the viewport clamp to its edge so a clipped surface does not outline the image border. Editor screenshots
include it outside game Play mode; game-camera screenshots do not. Enabled light, camera and audio
helper bounds compete with the mesh surface depth using the exact sampled texel UV and camera ray;
helpers behind a mesh cannot intercept its pick. Mesh bounds no longer substitute for geometry.
The separate Selection Bounds overlay restores selected mesh AABB feedback when desired; it does
not participate in picking. Colliders, selected particle shapes, and audio gizmos have independent
switches. Selection is also shown in the hierarchy and inspector when all viewport feedback is off.

`ViewportGPU` verifies ID/UUID mapping, overlap/occlusion, transparent and masked surfaces, outlines,
odd and tiny viewports, fixed scales, dynamic render-area reuse, animated poses, resize/invalidation,
and runtime opt-out with Vulkan synchronization validation. `EditorViewport` drives the real TCP
host to check settings, picking, captures, vertex snapping and undo/redo. It needs a display (Linux CI
uses Xvfb) and writes its captures/log to `build/Tests/Viewport/`. Some older MoltenVK builds emit a
spurious integer-attachment blending warning even with blending disabled; this is separate from
Khronos validation errors. The [upstream MoltenVK fix](https://github.com/KhronosGroup/MoltenVK/commit/f92b64c3a5084c6cfa78f581e0b863b40d2d16fa)
checks `pCA->blendEnable` before rejecting an integer attachment.

# Editor views and previews

The editor runs up to four `SceneRenderer`s; each owns its render targets, a 64 MiB cascaded shadow map and
GPU copies of the assets it draws.

- **Scene view** (picking, GPU timers): the editor camera at the panel's size, with editor helpers.
- **Game view** (GPU timers, created when the Game tab is first shown): the primary camera at the project's
  window size (or a session override), letterboxed in the panel. Render scale, dynamic resolution and FSR
  follow the Play preferences as in the runtime; the debug view never applies. The scene's cameras take the
  game's aspect ratio every frame. Each view renders only while visible, so the default tabbed Scene/Game pair
  costs one view. Script debug lines are drawn in the Game view and, with Debug Lines on, the scene view; editor
  helpers only in the scene view. `editor.game_view` captures go through the capture renderer.
- **Capture renderer** (picking, created on first use): screenshots and picking.
- **Preview renderer** (`PreviewRenderer`, no timers or picking, created on first preview): content-browser
  thumbnails of models, prefabs and material files, and material spheres. Spheres render the full material model:
  Material components (the inspector's), material files, and materials by value (the content-browser tooltip's
  swatches of a model's imported materials, the Material Editor's unsaved draft). By-value materials go through
  `SceneRenderer::SetMaterialResolver`, which only the preview renderer sets. It renders a fresh studio scene
  (procedural sky, one key light) at 256x256 and averages it down to 128x128, then releases the GPU copies of what it
  drew. At most one preview is rendered per frame; each waits for the GPU (readback). Images and `.hdr` environments
  are previewed on the CPU (an HDR is tonemapped with the ACES fit). Previews are cached in `<project>/Cache/Thumbnails`
  (outside `Assets/`, so never exported; new projects ignore `Cache/` in git) and remade when the size or modification
  time changes of the source file or of a file its preview reads: a glTF's external buffers and images, a material
  file's textures (and their models' files), a prefab's models and materials (and theirs). Only then are the model,
  prefab or material file and those files read into `AssetManager` again (the open scene then shows the new version
  too), as with `editor.thumbnail` `refresh`; a preview the cache still matches reuses what `AssetManager` holds. A file
  that disappears and comes back is previewed again. Shown previews check their files every 2 seconds, and at once
  whenever changed files are reloaded (the file watcher, `asset.refresh` or `asset.write`); a material sphere is made
  again when a file it reads changes (a texture, a material file, a model with its buffers and images). Models,
  textures and materials a preview loads only for itself are released from `AssetManager` afterwards (what was loaded
  before stays; a model's material list is kept with its preview for the tooltip). Only rows on screen ask for
  previews, and at most 512 textures are kept. Material spheres update once a material stops changing for a frame.
  Clear Thumbnail Cache forgets every preview and deletes the cached files but does not reload models already in
  `AssetManager`.

`PreviewRendererGPU` checks framing, determinism, material colour and emission, failures and that pending
debug lines survive a preview, with Vulkan validation required. It also checks that a material file, a Material
component referencing it and the imported material it came from render identically (per-map transform included),
that MASK coverage applies on spheres, and that scenes fall back to the inline values when a material file is missing.

## CPU jobs, streaming and memory

`Core/JobSystem` supplies one shared work-stealing CPU pool. Each worker takes its own queued work
last-in-first-out and steals other queues first-in-first-out. `Submit` returns a completion fence;
`Wait` helps execute work so nested jobs also complete with one worker. Exceptions fail the fence
and log a diagnostic. Jolt job exceptions are fatal because Jolt cannot complete a barrier after a
job unwinds. `ParallelFor` divides disjoint output ranges and joins every range before returning,
including after a task fails. Callers retain captured data until completion and avoid cyclic waits.
`WaitIdle` must run outside the pool's jobs; stop producers first for a stable idle fence. Destroy
pools on the constructing thread after producers stop. Shutdown drains accepted jobs and their children.
Physics uses Jolt barriers/dependencies on this pool. The Jolt adapter lives until engine shutdown,
so stopping a world does not join unrelated asset jobs; queued references to completed physics jobs
remain valid until the pool releases them. Process exit waits for all submitted pool work, including
imports. Animation samples due poses through `ParallelFor` for crowds (32 or more due playbacks,
ranges of 16). Control, asset resolution, clocks, events and root motion run on the main thread;
pose work joins before physics and late animation passes. Culling remains serial.

`AssetManager::RequestMesh/RequestTexture` deduplicate CPU loads and decode on this pool. Requests
return the published source or null; `IsLoading` distinguishes pending requests from cached failures.
`PublishCompleted` publishes completed results on the main thread. Scene rendering and UI frame
building call this fence, including camera-less menus; each fence may expose additional completions,
so separate scene/UI views do not promise one shared snapshot for an entire host frame.
`WaitForLoads` joins and publishes current requests. Blocking `GetMesh/GetTexture` join and publish
only the requested key and its mesh dependency, including automatic retries; they leave unrelated
entries unpublished. Embedded-image work is scheduled on the main thread after its parent completes.
Workers capture paths and project root, never the active project or scene. Reload, clear, preview-scope
eviction and project changes discard entries; late worker completions cannot recreate them.
A change to a mesh's own file drops its entry immediately. Pending file meshes retain changes to
other files until their dependencies are known, then retry only if affected, preserving requested
images and logging the changed dependency. Unrelated writes preserve work; file refreshes do not
affect built-ins. Cache methods are main-thread-only. Published payloads and completed worker results
are read-only during dependent image loading; consumers must not mutate their vectors then.

Interactive editor and runtime views request static meshes, material textures and environments
asynchronously, then upload on the main thread after publication. Pending meshes display a unit
cube at their entity transform; textures use fallback colors. Failed meshes disappear and report a
cached diagnostic. Animator meshes retain blocking loads, including in Edit mode. Physics startup
and editor bounds, collider overlays, focus and vertex snapping can also join an import; selecting
a loading mesh can stall a frame. `asset.preload` can warm these assets before use. Captures,
previews and finite-frame runtime runs retain blocking loads for deterministic output.
`asset.preload {"paths":["Assets/Models/Ship.glb"],"wait":true}` also works headlessly. Without `wait`,
repeat in a persistent `serve`/`mcp`/editor session to poll pending/ready/failed status. A one-shot CLI
process does not retain cache state for later calls: it waits for submitted worker tasks at exit,
but deferred embedded-image work is never submitted without another publication or blocking call.
Use `wait=true` for one-shot warmup and diagnostics.

Configure with `-DBASALT_PROFILE=ON` to enable pinned Tracy 0.12.2 CPU zones for frames, jobs, scene,
scripts, animation, physics, glTF import, texture decode and rendering. Run a matching Tracy profiler
and connect locally; the client records on demand. Default builds do not fetch or link Tracy, and
Dist builds compile instrumentation out. The Linux Debug CI job covers the profiled configuration.
See the [Tracy integration manual](https://github.com/wolfpld/tracy/blob/v0.12.2/manual/tracy.tex).

`memory.stats` works headlessly, and `render.stats.memory` and the editor Statistics panel show the
same subsystem current bytes, lifetime peak bytes and live charge counts. These are logical owned
bytes, **not RSS or driver heap residency**: assets track AssetManager-loaded mesh geometry, metadata arrays,
animation buffers, embedded images and decoded texture capacities (excluding string backing stores,
allocator overhead and graphs); GPU buffers track requested sizes of mesh, scene, cluster, histogram, debug-line and ImGui buffers
(excluding textures and render targets); Lua tracks every live allocation in
script and authoring states through its allocator, including collection and close. Asset charges survive
cache eviction while another consumer retains the source. GPU charges end with cache ownership,
while nvrhi may defer actual destruction for in-flight frames. Mutating CPU source vectors outside the
loader requires `UpdateMemoryUsage()` to refresh their payload charge.

# Play diagnostics and frame-time benchmarks

The Game tab's **Stats** checkbox enables a passive overlay during Play (also while paused). Set it
through `editor.game_view {"stats": true}`; it lasts for the editor session. The overlay is clipped to
the game image and takes no pointer or keyboard input. Game captures keep the game image and game UI;
`editor.window_screenshot` includes the diagnostics. `BasaltRuntime --stats` draws the same overlay,
including its blit timing, and includes it in runtime screenshots. `--stats` requires a GPU.

The overlay reports the last 60 host frames' mean wall-clock frame interval and FPS, Game renderer draw calls,
color-pass triangles (meshes and particle billboards; no depth, shadow or fullscreen triangles), live
physics bodies and the process's accounted Lua heap. Frame latency includes waits, presentation and
editor overhead; it remains wall time during fixed-step runs. Before rendering or when rendering is
unavailable, the overlay says so rather than showing old counters.

Scene GPU passes and the ImGui and runtime blit passes use separate `GpuProfiler` instances: timer
readback never waits, results arrive at least four frames late, and each profiler averages its last
30 results. Missing timers, their disable reason, pending results and stalled averages are explicit. A stalled
profiler shows its consecutive dropped-frame count instead of presenting its old average as current. **Game + host GPU** adds those independently
averaged times; in the editor it includes all editor ImGui, but excludes the separate Scene view and
thumbnail/capture work. It is not a whole-editor GPU frame measurement. Dynamic resolution still uses
only scene timing. ImGui timing includes its atlas/buffer uploads; separate capture draw lists are not
samples. Runtime `--frames` logs scene, blit and ImGui timing even when the overlay is off.

`render.stats.triangles` returns the color-pass count. `editor.game_view` also returns `stats`,
`statsVisible`, `frameMs` (null before any complete host frame), `physicsBodies` (null without a physics
world), `luaBytes`, `renderStats` (the live Game renderer, or null), and `imguiGpu` (null without an
initialized profiler). GPU diagnostics include `timers`, `consecutiveDrops`, `stalled` and
`disabledReason`, alongside the timing samples. `frameMs` and pass `ms` are null while timers are
disabled, pending or stalled; `droppedFrames` is the session total. The same GPU schema is used by
`render.stats` and the benchmark. `PlayStatistics` collects one consistent snapshot for
formatting and JSON; its pure formatter and unavailable/zero distinctions have headless tests.

The fixed Release workload and pre-throughput baseline are described in
[Tests/Benchmarks/README.md](../Tests/Benchmarks/README.md). Release CI runs its mean and p95 frame-time
thresholds and uploads raw samples; the lavapipe job also times complete rendered frames. Timing gates
are separate from deterministic unit tests.

## Draw throughput

Compatible opaque mesh instances share indexed GPU draws in the depth/normal/ID, lighting,
and directional/local shadow passes. The draw buffer holds each instance's model and normal
matrices, material-buffer index, pick token and independent skin palette offset/count. Material
constants live in a separate buffer; palettes precede the contiguous instance records in the draw
buffer. Vertex shaders use `DrawIndex + gl_InstanceIndex` with `startInstanceLocation` fixed at zero.
Batch keys group in deterministic first-appearance order. Texture bindings, geometry range, alpha-test state and sidedness split batches. Each pass filters camera/shadow
visibility before forming contiguous instance ranges. Blended meshes and particle emitters keep
their stable back-to-front ordering and individual draws.

`render.stats.drawCalls` counts submitted color draws; `instances` counts color mesh instances
(excludes particle billboards), and `triangles` counts all rendered color triangles. These counters
allow benchmark workload guards to detect missing objects even as draw counts decrease.
`SceneRenderer::SetInstancingEnabled(false)` provides a reference path for image equivalence and
throughput checks. Opaque grouping can change the winner for exactly coplanar surfaces from different
batch keys; use a geometric offset when a particular surface must win. Transparency keeps its order.
Animation updates reuse model/graph handles per key, and static mesh gathering reuses model handles
per key within each render. These caches end with the call; direct queries resolve fresh handles.
Skin culling uses immutable per-submesh joint influence bounds built by `AddSubmesh` during import
or procedural creation. Posing bounds costs O(influencing joints), with conservative bounds covering
weighted vertices, nonuniform scale and mirrored transforms. Exact CPU skin bounds remain in use for
editor focus/selection, uncached geometry and verification.

Particle emitters can fade against a copy of opaque depth (`SoftDistance`, world units) and receive
unshadowed two-sided diffuse lighting (`Lit`). Lit billboards use the surface renderer's bounded
range attenuation, Lambert normalization, clustered local-light lists and diffuse IBL; they have no
specular response or shadow reception/casting. Rotation is a roll about the view axis. In Edit mode,
`Preview` runs only particle simulation, independently of PlayOnStart and without scripts/audio/physics.
