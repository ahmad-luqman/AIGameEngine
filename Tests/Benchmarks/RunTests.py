"""The CI timing gate must reject broken workloads, missing timings and regressions."""
import copy
import math
import unittest

from Run import check_thresholds, summarize


class TimingGateTests(unittest.TestCase):
    def setUp(self):
        self.report = {"version": 1, "buildType": "Release", "mode": "cpu", "warmupFrames": 60, "entities": 2242,
                       "meshes": 2048, "animators": 128, "lights": 64, "physicsBodies": 256,
                       "fixedStep": 1 / 60, "samplesMs": list(range(1, 181))}

    def test_percentile_and_mean(self):
        self.assertEqual(summarize(self.report, "cpu"), {"meanMs": 90.5, "p95Ms": 171, "maxMs": 180})

    def test_gate_checks_mean_and_tail_independently(self):
        summary = summarize(self.report, "cpu")
        self.assertFalse(check_thresholds(summary, {"meanMs": 90.5, "p95Ms": 171}))
        self.assertEqual(len(check_thresholds(summary, {"meanMs": 90, "p95Ms": 180})), 1)
        self.assertEqual(len(check_thresholds(summary, {"meanMs": 100, "p95Ms": 170})), 1)
        for bad in (0, -1, math.inf, math.nan, True, "20", None):
            with self.assertRaises(ValueError):
                check_thresholds(summary, {"meanMs": bad, "p95Ms": 180})

    def test_bad_samples_fail(self):
        for bad in (0, -1, math.inf, math.nan, True, "1", None):
            report = copy.deepcopy(self.report)
            report["samplesMs"][0] = bad
            with self.assertRaises(ValueError):
                summarize(report, "cpu")
        for samples in ([], [1] * 179, [1] * 181, None):
            with self.assertRaises(ValueError):
                summarize(dict(self.report, samplesMs=samples), "cpu")

    def test_changed_timestep_fails(self):
        for step in (None, math.nan, math.inf, 0, 1 / 30, True, "1/60"):
            with self.assertRaises(ValueError):
                summarize(dict(self.report, fixedStep=step), "cpu")

    def test_wrong_workload_fails(self):
        for key in ("version", "buildType", "animators", "mode", "warmupFrames", "entities", "meshes", "lights", "physicsBodies"):
            report = dict(self.report)
            del report[key]
            with self.assertRaises(ValueError):
                summarize(report, "cpu")


if __name__ == "__main__":
    unittest.main()
