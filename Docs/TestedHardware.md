# Tested hardware

CI renders the feature scene on Mesa's lavapipe (a CPU Vulkan driver) on every push, and the developer
Mac renders it on MoltenVK. Neither catches bugs that only appear in a vendor's GPU driver, so before a
release the smoke test below is run on real NVIDIA, AMD and Intel GPUs, and the result is recorded in the
table at the end.

## Smoke test

Run all steps on each GPU. Steps 1–3 are automated; step 4 needs a person at the machine.

1. **Build Release with GPU tests.**
   ```bash
   # Linux / macOS
   cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBASALT_GPU_TESTS=ON
   cmake --build build-release
   ```
   ```bat
   :: Windows (Developer Command Prompt for VS, Vulkan SDK installed)
   cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DBASALT_GPU_TESTS=ON
   cmake --build build-release
   ```
2. **Pick the GPU and render the feature scene offscreen with validation.** The runtime lists every GPU
   at startup as `GPU N: <name> (<type>, ...)`; select one by index or by a unique part of its name.
   ```bash
   build-release/bin/BasaltRuntime --project Tests/Data/FeatureTest --frames 90 --offscreen \
       --require-validation --gpu nvidia --screenshot lit.png
   ```
   Exit code 0 means the validation layer was active and reported no errors (3 = validation errors,
   5 = validation layer missing, 4 = screenshot failed). Repeat with `--debug-view SSAO`, `Normals` and
   `Depth`. `ctest --test-dir build-release -L gpu` runs all views at once, but always on the automatically
   selected GPU (discrete first), so use the commands above for any other GPU.
3. **Compare with the golden images.** There are no references for vendor drivers yet. Compare against
   the MoltenVK set, which lavapipe also matches within tolerance:
   ```bash
   build-release/bin/basalt image.compare '{"actual": "lit.png", "reference": "Tests/Data/Golden/moltenvk/FeatureTest_Lit.png", "diff": "lit.diff.png"}'
   ```
   A small `differingPercent` (well under 1%) concentrated on texture edges is driver noise. Anything
   structural (flipped image, black or missing shadows, wrong normals) is a bug: open the diff image and
   follow the basalt-debug-rendering skill. For a driver family that will be tested regularly, add a
   reference set with `scripts/update_golden.sh <nvidia|amd|intel>` after checking the images by eye.
4. **Interactive check in the editor** (`BasaltEditor --project Samples/Tetris --gpu <G>`), with the
   validation layer installed:
   - resize the window, maximize, minimize and restore;
   - move, rotate and scale an entity with the gizmos;
   - play, pause and stop; play Tetris for a minute in `BasaltRuntime --project Samples/Tetris`;
   - the console (and the terminal) shows no `[Vulkan]` or `[nvrhi]` errors.

## Setting up each kind of machine

- **Windows:** install the latest GPU driver from the vendor and the [LunarG Vulkan SDK](https://vulkan.lunarg.com/)
  (includes the validation layer), plus Visual Studio with C++ and Ninja.
- **Linux (native, not WSL or a VM):** install the vendor driver (NVIDIA proprietary driver, or Mesa
  RADV/ANV for AMD/Intel) and `vulkan-validationlayers`. WSL2 and ordinary VMs do not expose the real GPU
  to Vulkan, so their results do not count; boot Linux natively (USB stick or dual boot).
- **Laptops with two GPUs (Intel + NVIDIA/AMD):** run the test once per GPU with `--gpu intel` and
  `--gpu nvidia`. Windows' "high performance / power saving" setting does not choose the Vulkan device.
- **Cloud GPUs (AWS):** `g4ad` instances have an AMD Radeon Pro V520 (Linux or Windows), `g5`/`g6` newer
  NVIDIA GPUs. `--offscreen` needs no display, so steps 1–3 run over SSH; step 4 needs a remote desktop
  with GPU access (Amazon DCV, not plain RDP). Use AWS's GRID driver on NVIDIA instances (the default
  datacenter driver can leave the GPU in compute-only mode on Windows) and AMD's driver or Mesa RADV on
  `g4ad`. New accounts usually need a quota increase for "Running On-Demand G and VT instances".
- **macOS:** Homebrew `molten-vk`, `vulkan-loader` and `vulkan-validationlayers`; run with
  `DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib` so the validation layer loads.

## Results

| Date | GPU | Driver | OS | Validation errors | Golden (vs. MoltenVK) | Interactive | Notes |
|------|-----|--------|----|-------------------|-----------------------|-------------|-------|
| 2026-10-07 | Apple M4 Max | MoltenVK 1.4.2 (Vulkan 1.3) | macOS 26.6.2 | 0 | reference set | not recorded | Developer machine |
| 2026-10-07 | lavapipe (CPU) | Mesa (Ubuntu 24.04) | Ubuntu 24.04, CI | 0 | match (max 0.04% px) | n/a | Every push, `gpu-lavapipe` job |
| | NVIDIA GeForce RTX 2080 Ti | | Windows | | | | |
| | NVIDIA GeForce RTX 2080 Ti | | Linux (native) | | | | |
| | NVIDIA laptop GPU | | Windows | | | | Optimus: `--gpu nvidia` |
| | Intel integrated GPU | | Windows | | | | `--gpu intel` |
| | AMD Radeon Pro V520 (AWS g4ad) | | Linux | | | | |
