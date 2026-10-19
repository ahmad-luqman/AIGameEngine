# Diagnostics benchmark (session 3)

`BenchmarkScene.h` builds a fixed scene: 2,048 separately drawn cubes, 128 animated mannequins sharing
one 17-joint model and looping Walk clip, 64 overlapping point lights, one shadowed sun and one camera.
256 cubes have dynamic box bodies with sleeping and gravity disabled, keeping physics work alive
throughout the run. IDs, placements and the 1/60-second timestep are fixed. The camera sees the field
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
validation errors, missing GPU timings and insufficient color geometry or missing skinned geometry/lights. The native headless unit test
checks that the workload survives warmup and measurement and that dropping a light component fails the guard. `RunTests.py` verifies failure paths and
independent mean/tail gates, including under Python `-O`.

`Thresholds.json` contains absolute CI budgets (milliseconds): CPU mean 20 / p95 40; GPU mean 1000 /
p95 1500. CPU runs in the Release Windows, Ubuntu and macOS jobs; the GPU run uses Ubuntu lavapipe.
These generous initial guardrails allow shared-runner scheduling noise and software rendering. They
catch large regressions; they do not assert a hardware-independent 60 FPS target. Tighten budgets using
several CI reports, never by copying fast local GPU numbers. A threshold change needs evidence and review.
The GPU run saves a final scene PNG beside its report, outside the measured interval.
Failures preserve the report as a CI artifact, and the executable's output remains in ctest's failure log.

To register gates locally, configure Release with `-DBASALT_BENCHMARK_TESTS=ON` (and
`-DBASALT_GPU_TESTS=ON` for GPU), build, then `ctest --test-dir build-release -L benchmark --output-on-failure`.
Other configurations reject this option; sanitizer/coverage runs exercise correctness, not performance.

`Baselines/` stores measured session-3 reports with their exact source revision and machine context.
Compare a new run on the same machine/configuration before attributing gains to session 4. CI artifacts
provide the corresponding runner baselines. Do not regenerate the committed baseline when optimizing:
keep it as the before measurement. A moving or larger workload needs a new version and baseline.
