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
