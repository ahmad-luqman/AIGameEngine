"""Render the visible UI gallery with validation and check clipping and transformed text pixels."""
from pathlib import Path
import subprocess
import sys

from LocalShadows import read_png


def main():
    runtime, output = sys.argv[1:]
    output = Path(output)
    output.mkdir(parents=True, exist_ok=True)
    project = Path(__file__).resolve().parents[1] / "Data" / "FeatureTest"
    screenshot = output / "UIFollowups.png"
    command = [runtime, "--project", str(project), "--scene", "Assets/Scenes/UIFollowups.bscene",
               "--offscreen", "--frames", "6", "--width", "1280", "--height", "720",
               "--require-validation", "--fail-on-script-errors", "--screenshot", str(screenshot)]
    result = subprocess.run(command, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=90)
    (output / "UIFollowups.log").write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(result.stdout)
    width, height, pixels = read_png(screenshot)
    if (width, height) != (1280, 720):
        raise RuntimeError(f"unexpected screenshot size {width}x{height}")

    def pixel(x, y):
        return pixels[int(y * 2 / 3) * width + int(x * 2 / 3)][:3]

    def close(actual, expected):
        return all(abs(a - b) <= 3 for a, b in zip(actual, expected))

    # Solid interiors avoid edge antialiasing and driver-dependent font rasterization.
    checks = [("outer clip", (150, 350), (26, 204, 51)),
              ("nested clip", (250, 370), (230, 51, 26)),
              ("nested content", (250, 450), (26, 77, 230)),
              ("outside right clip", (550, 450), (20, 26, 36)),
              ("outside bottom clip", (250, 560), (20, 26, 36)),
              ("focus ring", (1101, 310), (255, 204, 26))]
    for name, position, expected in checks:
        actual = pixel(*position)
        if not close(actual, expected):
            raise RuntimeError(f"{name}: pixel {position} is {actual}, expected {expected}")
    # This text starts outside the viewport and a parent's rotation brings it into view.
    # CPU glyph culling before applying that transform used to drop the whole line.
    bright = sum(min(pixel(x, y)) > 180 for y in range(880, 940) for x in range(420, 920))
    if bright < 100:
        raise RuntimeError(f"transformed text lost its glyphs ({bright} bright samples)")


if __name__ == "__main__":
    main()
