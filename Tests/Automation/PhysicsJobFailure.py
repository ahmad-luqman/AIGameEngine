"""A throwing Jolt job must terminate rather than leave its barrier stuck."""
import os
import subprocess
import sys

for mode in ("worker", "barrier"):
    env = dict(os.environ, BASALT_TEST_PHYSICS_FAILURE=mode)
    result = subprocess.run(
        [sys.argv[1], "--test-case=Throwing Jolt jobs fail fast on workers and barrier callers"],
        env=env, capture_output=True, text=True, timeout=30,
    )
    assert result.returncode == 86, (mode, result.returncode, result.stdout, result.stderr)
    assert "physics regression exception" in result.stdout + result.stderr, result
