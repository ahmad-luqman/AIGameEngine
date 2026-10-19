# Diagnostics benchmark (session 3)

`BenchmarkScene.h` builds a fixed scene: 2,048 separately drawn cubes, 128 animated mannequins sharing
one 17-joint model and looping Walk clip, 64 overlapping point lights, one shadowed sun and one camera.
256 cubes have dynamic box bodies with sleeping and gravity disabled, zero velocity and no contacts.
They measure integration, broadphase and transform synchronization, not contact/constraint solving. IDs, placements and the 1/60-second timestep are fixed. The camera sees the field
at 320x180; default High SSAO and cascaded shadows stay enabled. This measures the code before session
4's instancing and crowd improvements. No assets are streamed during measurement.

Build and run (idle machine, plugged in, no concurrent builds):

```sh
scripts/build.sh Release
python3 -B Tests/Benchmarks/Run.py --binary build-release/bin/BasaltBenchmark --mode cpu --output build-release/Tests/Benchmark-cpu.json
python3 -B Tests/Benchmarks/Run.py --binary build-release/bin/BasaltBenchmark --mode gpu --output build-release/Tests/Benchmark-gpu.json
```

Both modes discard 60 warmup frames and measure 180 frames with a monotonic clock. CPU mode measures
`Scene::OnUpdate` including physics, animation and Lua services, without a window or GPU. GPU mode adds
scene rendering, blit, presentation bookkeeping and a GPU idle wait per frame. It measures complete-frame
latency, rather than asynchronous throughput; it has no window, VSync or ImGui. Vulkan/nvrhi validation
is required in GPU mode, and its overhead is included. GPU timing averages in `renderStats` describe the
last 30 resolved scene samples and are supplementary to wall-clock measurements.

Reports preserve every measured frame, live workload counts, compiler/build type, worker count, platform, adapter, mean,
nearest-rank p95, maximum, thresholds and pass/fail status. The runner refuses missing/invalid timings,
changed workload counts or failed processes. The executable also checks for script/engine errors, incomplete component workloads,
validation errors, missing GPU timings and exact rendered draw/triangle/skin/light counts. The native headless unit tests
check that the workload survives warmup and measurement and that dropping a light component fails the guard. `RunTests.py` verifies failure paths and
independent mean/tail gates, including under Python `-O`.

`Thresholds.json` contains absolute CI budgets (milliseconds): CPU mean 20 / p95 40; GPU mean 250 /
p95 350. CPU runs in the Release Windows, Ubuntu and macOS jobs; the GPU run uses Ubuntu lavapipe.
The CPU limits allow the first hosted macOS run's 4.87 ms mean / 10.30 ms p95 and scheduling noise.
The GPU limits were tightened after the initial lavapipe run measured 114.74 ms mean / 117.81 ms p95
(CI run 39242777070), leaving about 2–3x headroom for software rendering and shared-runner variation.
These gates catch large regressions; they do not assert a hardware-independent 60 FPS target. A
threshold change needs measured evidence and review.
The GPU run saves a final scene PNG beside its report, outside the measured interval.
Native stage failures preserve a JSON failure report, including any partial samples. The runner also
preserves process failures, timeouts, malformed reports and threshold failures with `passed: false`
and a reason. Unwritable output paths are reported as I/O errors; CI still keeps the ctest failure log.

To register gates locally, configure Release with `-DBASALT_BENCHMARK_TESTS=ON` (and
`-DBASALT_GPU_TESTS=ON` for GPU), build, then `ctest --test-dir build-release -L benchmark --output-on-failure`.
Other configurations reject this option; sanitizer/coverage runs exercise correctness, not performance.

`Baselines/` stores measured session-3 reports. Their `sourceRevision`, `recordedAtUTC`, `hardware`
and `conditions` fields were added by hand after recording; the executable writes compiler/build type,
adapter, worker count and samples. The Python runner adds platform, summary, thresholds and pass/fail.
A baseline preserves the budgets in force when recorded (1000/1500 ms for the original GPU report),
even when current CI budgets are tightened.
Compare a new run on the same machine/configuration before attributing gains to session 4. CI artifacts
provide the corresponding runner baselines. Do not regenerate the committed baseline when optimizing:
keep it as the before measurement. A moving or larger workload needs a new version and baseline. Session 4 must deliberately update
the pre-instancing exact draw/triangle/skin-count guard when batching or LOD changes those counts.

## Recorded baseline

Recorded on Apple M4 Max (16 CPU cores, 128 GiB), macOS 26.6.2, AppleClang 21, Release,
with no concurrent builds/tests, from implementation revision `36d7b3c`:

| Mode | Mean frame (ms) | p95 frame (ms) | Raw report |
|---|---:|---:|---|
| CPU update | 2.347 | 2.649 | [macos-arm64-cpu.json](Baselines/macos-arm64-cpu.json) |
| Complete GPU frame, validation on | 31.031 | 33.309 | [macos-arm64-gpu.json](Baselines/macos-arm64-gpu.json) |

[Final benchmark image](Baselines/macos-arm64-gpu.json.png). CPU and GPU modes measure different work;
their times are not additive. These local measurements are evidence, not the shared-runner CI budgets.
