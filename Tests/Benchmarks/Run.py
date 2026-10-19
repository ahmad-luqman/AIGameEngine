"""Run the fixed Release workload, preserve raw samples and failures, and enforce timing budgets."""
import argparse
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess

# Keep these in sync with BenchmarkScene.h. Tests also reject changed values and incomplete renders.
EXPECTED_WORKLOAD = {"version": 1, "buildType": "Release", "warmupFrames": 60, "entities": 2242,
                     "meshes": 2048, "animators": 128, "lights": 64, "physicsBodies": 256}
SAMPLE_FRAMES = 180


def summarize(report, mode):
    for key, value in dict(EXPECTED_WORKLOAD, mode=mode).items():
        actual = report.get(key)
        if type(actual) is not type(value) or actual != value:
            raise ValueError(f"benchmark {key}: expected {value}, got {actual}")
    step = report.get("fixedStep")
    if type(step) not in (int, float) or not math.isfinite(step) or not math.isclose(step, 1 / 60, rel_tol=1e-6):
        raise ValueError(f"benchmark fixedStep must be 1/60, got {step}")
    samples = report.get("samplesMs")
    if not isinstance(samples, list) or len(samples) != SAMPLE_FRAMES:
        raise ValueError(f"benchmark needs exactly {SAMPLE_FRAMES} measured frames")
    if any(type(value) not in (int, float) or not math.isfinite(value) or value <= 0 for value in samples):
        raise ValueError("benchmark samples must be finite and positive")
    return {"meanMs": statistics.mean(samples), "p95Ms": sorted(samples)[math.ceil(.95 * len(samples)) - 1],
            "maxMs": max(samples)}


def check_thresholds(summary, thresholds):
    if not isinstance(thresholds, dict):
        raise ValueError("benchmark thresholds must be an object")
    for key in ("meanMs", "p95Ms"):
        limit = thresholds.get(key)
        if type(limit) not in (int, float) or not math.isfinite(limit) or limit <= 0:
            raise ValueError(f"invalid threshold {key}: {limit}")
    if thresholds["p95Ms"] < thresholds["meanMs"]:
        raise ValueError("p95 threshold must be at least the mean threshold")
    return [f"{key} {summary[key]:.3f} exceeds {thresholds[key]:.3f} ms"
            for key in ("meanMs", "p95Ms") if summary[key] > thresholds[key]]


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--mode", choices=("cpu", "gpu"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--thresholds", type=Path, default=Path(__file__).with_name("Thresholds.json"))
    args = parser.parse_args(argv)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.unlink(missing_ok=True)
    Path(str(args.output) + ".png").unlink(missing_ok=True)
    report = {}
    try:
        subprocess.run([args.binary, args.mode, str(args.output)], check=True, timeout=270)
        report = json.loads(args.output.read_text())
        if not isinstance(report, dict):
            raise ValueError("benchmark report must be an object")
        if report.get("passed") is False:
            raise ValueError(report.get("failure", "native benchmark reported failure"))
        summary = summarize(report, args.mode)
        limits = json.loads(args.thresholds.read_text())
        if not isinstance(limits, dict):
            raise ValueError("threshold configuration must be an object")
        thresholds = limits[args.mode]
        failures = check_thresholds(summary, thresholds)
        report.update(summary=summary, thresholds=thresholds)
        if failures:
            raise ValueError("; ".join(failures))
    except (ValueError, KeyError, OSError, subprocess.SubprocessError) as error:
        # The native executable may have preserved stage diagnostics and partial samples before exiting.
        if not isinstance(report, dict) or not report:
            try:
                partial = json.loads(args.output.read_text())
                report = partial if isinstance(partial, dict) else {}
            except (ValueError, OSError):
                report = {}
        report.update(passed=False, failure=report.get("failure") or str(error), mode=args.mode,
                      platform=platform.platform(), machine=platform.machine())
        args.output.write_text(json.dumps(report, indent=2) + "\n")
        raise SystemExit(report["failure"]) from error
    report.update(passed=True, platform=platform.platform(), machine=platform.machine())
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{args.mode}: mean {summary['meanMs']:.3f} ms, p95 {summary['p95Ms']:.3f} ms; report: {args.output}", flush=True)


if __name__ == "__main__":
    main()
