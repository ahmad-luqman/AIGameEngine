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

The pipeline is HDR lighting → exposure/tonemapping/sRGB encoding → EASU → RCAS → 8-bit dither → overlays.
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
focus and selection outlines use the current animated model bounds.

TRS tracks support STEP, LINEAR (shortest-arc quaternion slerp), and CUBICSPLINE (time-scaled Hermite
with normalized quaternion results). Missing channels retain authored node values. Blends and
crossfades mix local translation, rotation and scale before composing the hierarchy. The Lua API
section “Skeletal animation” defines playback and transition behavior.

Vertices carry four joint indices and normalized nonnegative weights. Vertex-stage linear blend
skinning uses `jointGlobal * inverseBind`; as glTF requires, the skinned mesh node transform does not
move the skin a second time. The Basalt entity world transform applies after skinning. Rigid animated
nodes use their evaluated node transform. Normals use the inverse transpose of the blended linear
transform; tangents use its linear transform and preserve reflection handedness. Degenerate skin
transforms have a finite normal fallback. The same palette and position operation feed PBR, depth/
normal prepass, opaque and masked cascades, and point/spot shadows. Transparent meshes use skinning
but retain the renderer's no-transparent-shadow policy.

Palettes occupy records in the existing draw SSBO, with per-draw offset/count metadata. This keeps
the frame at four storage-buffer bindings and supports independent instances without mutable GPU
asset data. Vertex stride is 80 bytes for both rigid and skinned geometry. Palette upload storage grows
with demand and uses nvrhi resource lifetime tracking; allocation failure fails the frame explicitly.
Exact submesh bounds are computed on the CPU from the current palette for camera and shadow culling.
This adds work proportional to animated vertex count, in addition to GPU skinning; large crowds would
benefit from cached joint influence bounds or GPU culling.

Import rejects malformed weights/joints, invalid track shapes/times/quaternions, cyclic hierarchies,
and unsupported extra influence sets or morph-weight animation. Sparse accessors and normalized
unsigned byte/short weights are supported. Limits are 65,536 nodes and joints per skin, 4,096 skins
and clips, and 64 MiB of decoded skin/animation floats (repeated sampler references count separately).
Non-finite evaluated hierarchies or palettes log once per failure episode and display the validated rest pose.
A skin may use the first N matrices of a larger inverse-bind accessor; absent inverse binds are
identity. Duplicate clip names get deterministic suffixes. There is no morph-target deformation,
retargeting, animation state machine, bone attachment API, or root-motion extraction.

`AnimationTests` covers import, interpolation, space conventions, playback, blending, component/Lua
validation and lifecycle; the Khronos Fox fixture verifies Survey/Walk/Run from a real exported rig.
The feature-test driver exercises every Lua operation on an off-camera rig. `AnimationGPU` compares
GPU skinning with independently deformed CPU geometry for opaque/masked/blended materials, lit/
normal/depth outputs and directional/point/spot shadows, with Vulkan validation required.
