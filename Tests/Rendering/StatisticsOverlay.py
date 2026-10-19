"""Exercise the runtime statistics overlay and its host GPU timers with Vulkan validation."""
import re
from pathlib import Path
import subprocess
import sys

from LocalShadows import read_png


def main():
    runtime, output = sys.argv[1:]
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    project = Path(__file__).resolve().parents[1] / "Data" / "FeatureTest"
    images = []
    for stats in (False, True):
        screenshot = output / ("stats.png" if stats else "plain.png")
        command = [runtime, "--project", str(project), "--offscreen", "--frames", "60",
                   "--width", "640", "--height", "480", "--require-validation", "--fail-on-script-errors",
                   "--screenshot", str(screenshot)]
        if stats:
            command.append("--stats")
        result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
        (output / ("stats.log" if stats else "plain.log")).write_text(result.stdout)
        if result.returncode:
            raise RuntimeError(result.stdout)
        for name in ("scene", "blit", "ImGui"):
            if not re.search(rf"Runtime: {name} GPU time [0-9]+\.[0-9]+ ms over [1-9][0-9]* frames", result.stdout):
                raise RuntimeError(f"missing {name} timing:\n{result.stdout}")
        width, height, pixels = read_png(screenshot)
        if (width, height) != (640, 480):
            raise RuntimeError(f"incorrect screenshot size {width}x{height}")
        images.append(pixels)
    # The frame-time text is in the first line; this checks that --stats actually draws, beyond logging timers.
    changed = sum(images[0][y * 640 + x] != images[1][y * 640 + x]
                  for y in range(14, 32) for x in range(14, 180))
    if changed < 100:
        raise RuntimeError("--stats did not draw the statistics overlay")


if __name__ == "__main__":
    main()
