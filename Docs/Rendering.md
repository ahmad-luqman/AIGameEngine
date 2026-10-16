# Post-processing and graphics preferences

Add a `PostProcess` component to an entity through the inspector, `component.set`, or Lua. It controls
manual exposure, tonemapper (`None`, `Reinhard`, `ACES`, `AgX`), histogram auto exposure, SSAO, bloom and
FXAA. The enabled component with the highest `Priority` wins globally; ties use the lowest entity UUID.
`Priority` is a nonnegative integer (default 0); `Enabled` defaults true. There are no spatial volumes or blends. Without an enabled component, legacy `Renderer` exposure,
tonemapper and SSAO settings remain the fallback, preserving existing scenes and images. The editor
hides those fallback controls while a component is active. Shadows and FSR remain scene renderer settings.

The order is HDR opaque/sky/transparent/particles → histogram metering and HDR bloom → exposure,
tonemapping and sRGB encoding → optional FXAA → optional FSR → final dithering → overlays/UI.
SSAO remains a pre-lighting effect. Debug views bypass bloom, exposure, FXAA and FSR. All filter taps
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
Enabling any of bloom, auto exposure or FXAA allocates the shared HDR/exposure/bloom resources; FXAA
adds its own input and caches pipelines for both final and FSR output formats. Resources remain cached
until targets change. Allocation failure fails the frame/capture in every build, including Dist, instead
of silently changing the authored look. Failed resource paths log once until all three effects are disabled
and one re-enabled, or the targets change. Invalid component diagnostics name the entity and repeat only
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
| FXAAEnabled / BloomEnabled | false / false | boolean |
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
it does not provide temporal antialiasing.

The complete FSR path is shadows → depth/normals → SSAO → clustered lighting/sky/transparency → optional
HDR bloom/auto exposure → tonemapping/sRGB → optional FXAA → EASU → RCAS → 8-bit dither → overlays.
Intermediate color is stored in RGBA16_FLOAT without sRGB decoding; FSR expects normalized perceptual
color. The extra targets allocate on first use and remain cached while FSR is off and across dynamic
render-area changes. Viewport, configured render-scale and active dynamic-mode changes release them.
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
editable entity hierarchy. A Material on the root overrides imported materials. Editor picking,
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
pass needs it; `render.stats.skinMatrices` reports the uploaded matrix count. Vertex stride is 80 bytes for both rigid and skinned geometry. Palette upload storage grows
with demand and uses nvrhi resource lifetime tracking; allocation failure fails the frame explicitly.
Exact submesh bounds are computed on the CPU from the current palette for camera and shadow culling.
This adds work proportional to animated vertex count, in addition to GPU skinning; large crowds would
benefit from cached joint influence bounds or GPU culling.

Import rejects malformed weights/joints, invalid track shapes/times/quaternions, cyclic hierarchies,
animated matrix nodes and duplicate node/path targets. Unsupported morph-weight and pointer channels
are warned about and skipped; extra influence sets retain static geometry with a warning and disable
Animator playback for that asset. Sparse accessors, normalized unsigned byte/short weights and
normalized signed/unsigned byte/short rotation keys are supported. Animated/skinned models are limited
to 65,536 nodes per model; each skin is limited to 65,536 joints. Other limits are 4,096 skins
and clips, 65,536 skin-joint references summed over active mesh nodes, and 64 MiB of decoded skin/animation
floats (repeated sampler references count separately). The combined joint-reference limit bounds
work for assets that reuse a large skin across many nodes.
Non-finite evaluated hierarchies or palettes log once per failure episode and display the validated rest pose.
A skin may use the first N matrices of a larger inverse-bind accessor; absent inverse binds are
identity. Duplicate clip names get deterministic suffixes. There is no morph-target deformation,
retargeting, animation state machine, bone attachment API, or root-motion extraction.

`AnimationTests` covers import, interpolation, space conventions, playback, blending, component/Lua
validation and lifecycle; the Khronos Fox fixture verifies Survey/Walk/Run from a real exported rig.
The feature-test driver exercises every Lua operation on an off-camera rig. `AnimationGPU` compares
GPU skinning with independently deformed CPU geometry for opaque/masked/blended materials, lit/
normal/depth outputs and directional/point/spot shadows, with Vulkan validation required.

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
  thumbnails of models and prefabs and material spheres. It renders a fresh studio scene (procedural sky,
  one key light) at 256x256 and averages it down to 128x128, then releases the GPU copies of what it drew.
  At most one preview is rendered per frame; each waits for the GPU (readback). Images and `.hdr`
  environments are previewed on the CPU (an HDR is tonemapped with the ACES fit). Previews are cached in
  `<project>/Cache/Thumbnails` (outside `Assets/`, so never exported; new projects ignore `Cache/` in git) and
  remade when the source file's size or modification time changes (a changed model is read again). Only rows on
  screen ask for previews, and at most 512 textures are kept. Material spheres update once a material stops
  changing for a frame. Model and prefab previews load the model into `AssetManager`, which keeps it; textures
  referenced by a material, and models inside a prefab, are not tracked, so editing one does not refresh the
  previews that use it (Clear Thumbnail Cache does).

`PreviewRendererGPU` checks framing, determinism, material colour and emission, failures and that pending
debug lines survive a preview, with Vulkan validation required.
