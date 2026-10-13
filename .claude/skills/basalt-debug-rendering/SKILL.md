---
name: basalt-debug-rendering
description: Diagnose rendering problems in Basalt (black/inside-out/flipped images, missing shadows, wrong lighting, validation errors). Use when a screenshot looks wrong or the renderer reports errors.
---

# Debugging rendering

1. **Reproduce with a screenshot** (macOS/Homebrew needs the dyld path for validation layers):
   ```bash
   DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib build/bin/BasaltRuntime --project P --frames 30 \
       --screenshot /tmp/shot.png [--debug-view SSAO|Normals|Depth]
   ```
   Exit code 3 means GPU validation errors occurred: read the `[Vulkan]`/`[nvrhi]` log lines first.
2. **Isolate passes** with `--debug-view` and by toggling `scene.settings`
   `{"Renderer": {"SSAOEnabled": false}}` / `{"ShadowsEnabled": false}` in a copy of the scene. Check the
   project's `RenderScale` too (`project.info`; override with `--render-scale 1`): below 1 the scene renders
   smaller and is upsampled. Bilinear remains the default, so soft or stair-stepped edges are expected. Scene `Renderer.FSR1Enabled` selects FSR 1 below native size;
   `FSR1Sharpness` is RCAS attenuation in stops (0 strongest, 2 weakest, default 0.2). It runs after
   tonemapping and before overlays. The current render area controls the bypass: native-size or larger
   areas and debug views bypass FSR, even if the configured scale differs. The Statistics panel and
   `render.stats.fsr1Active` show whether it ran. A failed frame shows a viewport banner; toggling FSR
   off/on retries its failed allocation. With `DynamicResolution`
   on, the scale also changes with GPU load: turn it off (`--dynamic-resolution 0`) for stable images.
   For slow frames, `render.stats` (editor) and the runtime's `--frames` log give the GPU time per pass.
   Compare intermediate images before reasoning about shader math.
3. **Known conventions** (AGENTS.md "Rendering conventions"): clip space is +Y up (nvrhi flips the Vulkan
   viewport) — never flip projections; fullscreen UV (0,0) is top-left; meshes wind CCW; GLSL
   `set/binding` must match the C++ binding layouts (zero binding offsets, unique slots); push constants
   use `PushConstantSlot` in layouts with constant buffers. C++ GPU structs must match std140/std430
   (static_asserts in SceneRenderer.cpp).
4. **Inside-out or missing geometry**: winding/culling. **Mirrored passes**: a UV/NDC Y convention mix-up.
   **Black frame, no errors**: binding slot mismatch or a missing barrier/state.
5. Fix, rerun the screenshot, then `scripts/test.sh` (includes the GPU FeatureTestRender test locally).
