"""The CI timing gate rejects broken workloads and regressions, including under Python -O."""
import copy
import json
import math
from pathlib import Path
import random
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import Run


class TimingGateTests(unittest.TestCase):
    def setUp(self):
        self.report = dict(Run.EXPECTED_WORKLOAD, mode="cpu", fixedStep=1 / 60,
                           samplesMs=list(range(1, Run.SAMPLE_FRAMES + 1)))

    def test_unsorted_percentile_mean_and_max(self):
        random.Random(42).shuffle(self.report["samplesMs"])
        self.assertNotEqual(self.report["samplesMs"][-1], 180)
        self.assertEqual(Run.summarize(self.report, "cpu"), {"meanMs": 90.5, "p95Ms": 171, "maxMs": 180})
        self.report["mode"] = "gpu"
        self.assertEqual(Run.summarize(self.report, "gpu")["p95Ms"], 171)
        with self.assertRaises(ValueError):
            Run.summarize(self.report, "cpu")

    def test_gate_checks_mean_and_tail_independently(self):
        summary = Run.summarize(self.report, "cpu")
        self.assertFalse(Run.check_thresholds(summary, {"meanMs": 90.5, "p95Ms": 171}))
        self.assertEqual(len(Run.check_thresholds(summary, {"meanMs": 90, "p95Ms": 180})), 1)
        self.assertEqual(len(Run.check_thresholds(summary, {"meanMs": 100, "p95Ms": 170})), 1)
        for bad in (0, -1, math.inf, math.nan, True, "20", None):
            with self.assertRaises(ValueError):
                Run.check_thresholds(summary, {"meanMs": bad, "p95Ms": 180})
        with self.assertRaises(ValueError):
            Run.check_thresholds(summary, {"meanMs": 200, "p95Ms": 180})
        for thresholds in json.loads(Path(Run.__file__).with_name("Thresholds.json").read_text()).values():
            self.assertFalse(Run.check_thresholds({"meanMs": 0, "p95Ms": 0}, thresholds))

    def test_bad_samples_and_timestep_fail(self):
        for bad in (0, -1, math.inf, math.nan, True, "1", None):
            report = copy.deepcopy(self.report)
            report["samplesMs"][0] = bad
            with self.assertRaises(ValueError):
                Run.summarize(report, "cpu")
        for samples in ([], [1] * 179, [1] * 181, None):
            with self.assertRaises(ValueError):
                Run.summarize(dict(self.report, samplesMs=samples), "cpu")
        for step in (None, math.nan, math.inf, 0, 1 / 30, True, "1/60"):
            with self.assertRaises(ValueError):
                Run.summarize(dict(self.report, fixedStep=step), "cpu")

    def test_missing_wrong_and_wrongly_typed_workload_values_fail(self):
        for key, value in dict(Run.EXPECTED_WORKLOAD, mode="cpu").items():
            for actual in (None, "wrong", False, 2241, 2, float(value) if type(value) is int else 1):
                if type(actual) is type(value) and actual == value:
                    continue
                with self.subTest(key=key, actual=actual), self.assertRaises(ValueError):
                    Run.summarize(dict(self.report, **{key: actual}), "cpu")
            missing = dict(self.report)
            del missing[key]
            with self.assertRaises(ValueError):
                Run.summarize(missing, "cpu")
        with self.assertRaises(ValueError):
            Run.summarize(dict(self.report, version=True), "cpu")
        with self.assertRaises(ValueError):
            Run.summarize(dict(self.report, buildType="Debug"), "cpu")

    def test_main_deletes_stale_files_selects_mode_and_preserves_threshold_failure(self):
        with tempfile.TemporaryDirectory() as temp:
            output = Path(temp, "report.json")
            image = Path(str(output) + ".png")
            thresholds = Path(temp, "limits.json")
            thresholds.write_text(json.dumps({"cpu": {"meanMs": 100, "p95Ms": 200},
                                              "gpu": {"meanMs": 1, "p95Ms": 2}}))
            for mode in ("cpu", "gpu"):
                output.write_text('{"passed": true}')
                image.write_bytes(b"stale")

                def native(command, **kwargs):
                    self.assertFalse(output.exists())
                    self.assertFalse(image.exists())
                    self.assertTrue(kwargs["check"])
                    self.assertEqual(command[1], mode)
                    output.write_text(json.dumps(dict(self.report, mode=mode)))

                args = ["--binary", "native", "--mode", mode, "--output", str(output), "--thresholds", str(thresholds)]
                with patch.object(Run.platform, "platform", return_value="test"), patch.object(Run.subprocess, "run", side_effect=native):
                    if mode == "gpu":
                        with self.assertRaises(SystemExit):
                            Run.main(args)
                    else:
                        Run.main(args)
                report = json.loads(output.read_text())
                self.assertEqual(report["passed"], mode == "cpu")
                self.assertEqual(report["thresholds"], json.loads(thresholds.read_text())[mode])
                if mode == "gpu":
                    self.assertIn("exceeds", report["failure"])

    def test_main_preserves_malformed_threshold_failures(self):
        for limits in (None, [], {"cpu": None}, {"cpu": []}, {"cpu": "invalid"}):
            with self.subTest(limits=limits), tempfile.TemporaryDirectory() as temp:
                output = Path(temp, "report.json")
                thresholds = Path(temp, "thresholds.json")
                thresholds.write_text(json.dumps(limits))

                def native(command, **kwargs):
                    output.write_text(json.dumps(dict(self.report, passed=True)))

                with patch.object(Run.platform, "platform", return_value="test"), patch.object(Run.subprocess, "run", side_effect=native), self.assertRaises(SystemExit):
                    Run.main(["--binary", "native", "--mode", "cpu", "--output", str(output), "--thresholds", str(thresholds)])
                report = json.loads(output.read_text())
                self.assertFalse(report["passed"])
                self.assertIn("object", report["failure"])

    def test_main_preserves_native_failure_timeout_and_bad_json(self):
        for kind in ("native", "timeout", "bad-json", "bad-workload"):
            with self.subTest(kind=kind), tempfile.TemporaryDirectory() as temp:
                output = Path(temp, "report.json")

                def native(command, **kwargs):
                    if kind == "native":
                        output.write_text('{"passed": false, "failure": "BeginFrame failed", "samplesMs": [1]}')
                        raise subprocess.CalledProcessError(1, command)
                    if kind == "timeout":
                        raise subprocess.TimeoutExpired(command, 270)
                    output.write_text("bad json" if kind == "bad-json" else json.dumps(dict(self.report, version=2)))

                with patch.object(Run.platform, "platform", return_value="test"), patch.object(Run.subprocess, "run", side_effect=native), self.assertRaises(SystemExit):
                    Run.main(["--binary", "native", "--mode", "cpu", "--output", str(output)])
                report = json.loads(output.read_text())
                self.assertFalse(report["passed"])
                self.assertTrue(report["failure"])
                if kind == "native":
                    self.assertEqual(report["failure"], "BeginFrame failed")
                    self.assertEqual(report["samplesMs"], [1])


if __name__ == "__main__":
    unittest.main()
