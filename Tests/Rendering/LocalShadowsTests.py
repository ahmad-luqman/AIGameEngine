#!/usr/bin/env python3
"""Headless regressions for the GPU harness's failure detection, also run with python -O."""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.dont_write_bytecode = True
import LocalShadows as shadows


class HarnessTests(unittest.TestCase):
    def test_size_mismatch_is_not_an_equal_image(self):
        with self.assertRaises(AssertionError):
            shadows.comparison_percent({'ok': True, 'result': {'sizeMatches': False, 'differingPercent': 0}})

    def test_success_without_a_new_screenshot_rejects_stale_file(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'stale.png'
            image.write_bytes(b'old screenshot')
            with patch.object(shadows, 'run', return_value=subprocess.CompletedProcess([], 0, '', '')):
                with self.assertRaisesRegex(AssertionError, 'fresh screenshot'):
                    shadows.capture(['runtime'], image)
            self.assertFalse(image.exists())

    def test_failure_logs_are_not_hidden_by_successful_exit(self):
        with tempfile.TemporaryDirectory() as directory:
            image = Path(directory) / 'error.png'
            for log in ('SceneRenderer: failed to create frame bindings', 'Script error: bad script'):
                def render(_):
                    image.write_bytes(b'new image')
                    return subprocess.CompletedProcess([], 0, log, '')
                with patch.object(shadows, 'run', side_effect=render):
                    with self.assertRaisesRegex(AssertionError, 'renderer failed'):
                        shadows.capture(['runtime'], image)

    def test_command_timeout_and_failure_propagate(self):
        with patch.object(shadows.subprocess, 'run', side_effect=subprocess.TimeoutExpired('runtime', 60)) as run:
            with self.assertRaises(subprocess.TimeoutExpired):
                shadows.run(['runtime'])
            self.assertEqual(run.call_args.kwargs['timeout'], 60)
        with patch.object(shadows.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, '', 'failure')):
            with self.assertRaisesRegex(RuntimeError, 'failure'):
                shadows.run(['runtime'])

    def test_checks_survive_python_optimization(self):
        with self.assertRaises(AssertionError):
            shadows.check(False, 'must fail under -O too')

    def test_png_filters_decode_rgb_and_rgba(self):
        # Two pixels per row exercise left, above and upper-left dependencies for every PNG filter.
        for channels in (3, 4):
            row = bytes(range(10, 10 + 2 * channels))
            previous = bytes(2 * channels)
            raw = bytearray()
            for filter_type in range(5):
                raw.append(filter_type)
                for x, value in enumerate(row):
                    left = row[x - channels] if x >= channels else 0
                    up = previous[x]
                    corner = previous[x - channels] if x >= channels else 0
                    # These monotonically increasing rows make Paeth choose up (or left in row zero).
                    paeth = up if up else left
                    raw.append((value - (0, left, up, (left + up) // 2, paeth)[filter_type]) % 256)
                previous = row
            def chunk(kind, data):
                return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
            png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 2, 5, 8, 2 if channels == 3 else 6, 0, 0, 0))
            png += chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b'')
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'filters.png'
                path.write_bytes(png)
                self.assertEqual(shadows.read_png(path), (2, 5, [tuple(row[:3]), tuple(row[channels:channels + 3])] * 5))


if __name__ == '__main__':
    unittest.main()
