# Spatial upscaling (FSR 1)

Scene settings select the upscaler through the editor's Renderer settings or automation:

```sh
basalt --project MyGame scene.settings '{"Renderer":{"FSR1Enabled":true,"FSR1Sharpness":0.2}}'
```

`FSR1Enabled` defaults to false, preserving bilinear upsampling for existing scenes. When enabled,
FSR 1 replaces bilinear upsampling below native resolution. `FSR1Sharpness` is RCAS attenuation in
stops, from 0 (strongest) to 2 (weakest); default 0.2. It is not a 0–1 strength slider. Invalid values
are rejected by the shared scene reader. Both settings are saved with the scene and copied for Play.

Use the project's `RenderScale` or runtime `--render-scale`, together with the existing dynamic
resolution controls. Native-size output, supersampling and SSAO/Normals/Depth debug views bypass FSR.
UI, text, debug lines and gizmos are drawn afterward at output resolution. FSR 1 is a spatial upscaler;
it does not provide temporal antialiasing.

The pipeline is HDR lighting → exposure/tonemapping/sRGB encoding → EASU → RCAS → 8-bit dither → overlays.
Intermediate color is stored in RGBA16_FLOAT without sRGB decoding; FSR expects normalized perceptual
color. The extra targets are allocated on first use and retained until resize, including across dynamic
render-area changes. Their cost is included in the existing Tonemap GPU timing and dynamic-resolution
feedback. An allocation failure fails the frame and capture rather than silently selecting another filter.

Dynamic resolution renders into the top-left of larger textures. EASU receives both the current render
size and allocated texture size; every individual input tap is clamped to the render area, including
one-pixel dimensions. RCAS reads the full output-sized EASU image. Neither pass can read unused texels.

AMD's unmodified MIT-licensed FSR 1.0.2 headers are fetched at the exact commit in
`cmake/Dependencies.cmake`, with sample/media submodules disabled. They are build dependencies only;
compiled shaders remain embedded in the engine. See [AMD's FSR 1 source and license](https://github.com/GPUOpen-Effects/FidelityFX-FSR/tree/a21ffb8f6c13233ba336352bdff293894c706575)
and [integration guidance](https://gpuopen.com/fidelityfx-superresolution/).

`FSR1GPU` tests synthetic inputs, poisoned unused texels, tiny render areas, resource reuse and color
preservation with Vulkan validation. `FSR1Render` exercises scene settings, odd fixed/dynamic sizes,
RCAS strength and bypass paths on the static feature fixture. Both run in the lavapipe CI GPU job.
