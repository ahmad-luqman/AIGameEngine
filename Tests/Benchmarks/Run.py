"""Run the fixed Release workload, preserve raw samples, and fail on invalid data or exceeded budgets."""
import argparse
import json
import math
from pathlib import Path
import platform
import statistics
import subprocess


def summarize(report, mode):
    expected = {"version": 1, "buildType": "Release", "mode": mode, "warmupFrames": 60, "entities": 2242,
                "meshes": 2048, "animators": 128, "lights": 64, "physicsBodies": 256}
    for key, value in expected.items():
        if report.get(key) != value:
            raise ValueError(f"benchmark {key}: expected {value}, got {report.get(key)}")
    step = report.get("fixedStep")
    if type(step) not in (int, float) or not math.isfinite(step) or not math.isclose(step, 1 / 60, rel_tol=1e-6):
        raise ValueError(f"benchmark fixedStep must be 1/60, got {step}")
    samples = report.get("samplesMs")
    if not isinstance(samples, list) or len(samples) != 180:
        raise ValueError("benchmark needs exactly 180 measured frames")
    if any(type(value) not in (int, float) or not math.isfinite(value) or value <= 0 for value in samples):
        raise ValueError("benchmark samples must be finite and positive")
    return {"meanMs": statistics.mean(samples), "p95Ms": sorted(samples)[math.ceil(.95 * len(samples)) - 1],
            "maxMs": max(samples)}


def check_thresholds(summary, thresholds):
    failures = []
    for key in ("meanMs", "p95Ms"):
        limit = thresholds.get(key)
        if type(limit) not in (int, float) or not math.isfinite(limit) or limit <= 0:
            raise ValueError(f"invalid threshold {key}: {limit}")
        if summary[key] > limit:
            failures.append(f"{key} {summary[key]:.3f} exceeds {limit:.3f} ms")
    return failures


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True)
    parser.add_argument("--mode", choices=("cpu", "gpu"), required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--thresholds", type=Path, default=Path(__file__).with_name("Thresholds.json"))
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    # A failed or interrupted process must never leave a previous successful report looking current.
    args.output.unlink(missing_ok=True)
    Path(str(args.output) + ".png").unlink(missing_ok=True)
    subprocess.run([args.binary, args.mode, str(args.output)], check=True, timeout=270)
    report = json.loads(args.output.read_text())
    summary = summarize(report, args.mode)
    thresholds = json.loads(args.thresholds.read_text())[args.mode]
    failures = check_thresholds(summary, thresholds)
    report.update(summary=summary, thresholds=thresholds, passed=not failures,
                  platform=platform.platform(), machine=platform.machine())
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"{args.mode}: mean {summary['meanMs']:.3f} ms, p95 {summary['p95Ms']:.3f} ms; report: {args.output}", flush=True)
    if failures:
        raise SystemExit("; ".join(failures))


if __name__ == "__main__":
    main()
