#!/usr/bin/env python3
"""End-to-end FSR 1 checks on MoltenVK and lavapipe; generated files stay in the build tree."""
import argparse
import copy
import json
from pathlib import Path
import re

from LocalShadows import capture, check, comparison_percent, read_png, run


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[1] / 'Data/FeatureTest'
    # The render fixture is static, unlike the gameplay scene, so different frame counts compare exactly.
    fixture = json.loads((source / 'Assets/Scenes/RenderTest.bscene').read_text())

    def render(name, enabled=True, scale=0.5, sharpness=0.2, dynamic=False, view='None', size=(321, 181), ssao=False, ceiling=1):
        scene = copy.deepcopy(fixture)
        scene['Renderer'].update(FSR1Enabled=enabled, FSR1Sharpness=sharpness, SSAOEnabled=ssao)
        scene_path = args.output / f'{name}.bscene'
        scene_path.write_text(json.dumps(scene))
        image = args.output / f'{name}.png'
        command = [args.runtime, '--project', str(source), '--scene', str(scene_path.resolve()),
                   '--offscreen', '--frames', '90' if dynamic else '6', '--width', str(size[0]),
                   '--height', str(size[1]), '--require-validation', '--screenshot', str(image),
                   '--debug-view', view, '--render-scale', str(ceiling) if dynamic else str(scale)]
        if dynamic:
            # An unreachable FPS budget forces the controller to the requested minimum scale.
            command += ['--dynamic-resolution', '1000000', '--min-render-scale', str(scale)]
        capture(command, image)
        log = image.with_suffix('.log').read_text()
        active = re.search(r'fsr1=(0|1)\b', log)
        expected_active = enabled and view == 'None' and scale < 1
        check(active is not None and bool(int(active[1])) == expected_active, f'{name}: FSR activity was not confirmed')
        if dynamic:
            dimensions = re.search(r'rendered at (\d+)x(\d+)', log)
            check(dimensions is not None, 'missing dynamic-resolution dimensions')
            # ComputeRenderSize rounds to the nearest pixel (positive lround), not down.
            expected = tuple(max(1, int(v * scale + 0.5)) for v in size)
            check(tuple(map(int, dimensions.groups())) == expected, f'dynamic resolution missed {expected}')
        return image

    def difference(a, b, threshold=0):
        result = json.loads(run([args.cli, 'image.compare', json.dumps({
            'actual': str(a), 'reference': str(b), 'pixelThreshold': threshold, 'maxPercent': 0
        })]).stdout)
        return comparison_percent(result)

    # Odd-size SSAO sampling is not bit-exact across backing sizes. Isolate EASU for these pairs.
    for scale in (0.25, 0.5, 0.67):
        fixed = render(f'fixed_{scale}', scale=scale)
        dynamic = render(f'dynamic_{scale}', scale=scale, dynamic=True)
        check(difference(fixed, dynamic) == 0, f'FSR reads outside the render area at {scale}')
    check(difference(render('ssao_fixed', size=(320, 180), ssao=True),
                     render('ssao_dynamic', dynamic=True, size=(320, 180), ssao=True)) == 0,
          'full-stack dynamic FSR differs with SSAO')
    check(difference(render('fixed_from_supersample'), render('dynamic_from_supersample', dynamic=True, ceiling=1.5)) == 0,
          'FSR did not activate when the current render area dropped below native')
    fsr = render('fsr')
    bilinear = render('bilinear', enabled=False)
    check(difference(fsr, bilinear, 2) > 1, 'FSR did not visibly change the scaled image')
    strong = render('strong', sharpness=0)
    weak = render('weak', sharpness=2)
    check(difference(strong, weak, 2) > 0.1, 'RCAS sharpness is ineffective')
    # An absolute error bound rejects corrupt output even if all pairwise differences are nonzero.
    native = render('native_reference', scale=1)
    width, height, native_pixels = read_png(native)
    fsr_width, fsr_height, fsr_pixels = read_png(fsr)
    check((width, height) == (fsr_width, fsr_height), 'FSR output dimensions differ from native')
    mean_error = sum(abs(a - b) for p, q in zip(native_pixels, fsr_pixels) for a, b in zip(p, q)) / (width * height * 3)
    check(mean_error < 5, f'FSR output is too far from native: mean channel error {mean_error}')

    def local_contrast(image):
        w, h, pixels = read_png(image)
        return sum(abs(4 * pixels[y*w+x][c] - pixels[y*w+x-1][c] - pixels[y*w+x+1][c]
                       - pixels[(y-1)*w+x][c] - pixels[(y+1)*w+x][c])
                   for y in range(1, h-1) for x in range(1, w-1) for c in range(3)) / ((w-2)*(h-2)*3)

    check(local_contrast(strong) > local_contrast(weak) * 1.02, '0 stops must sharpen more than 2 stops')
    for scale in (1, 1.5):
        check(difference(render(f'native_on_{scale}', scale=scale),
                         render(f'native_off_{scale}', enabled=False, scale=scale)) == 0,
              f'FSR should bypass native/supersampled output at {scale}')
    for view in ('SSAO', 'Normals', 'Depth'):
        check(difference(render(f'{view}_on', view=view), render(f'{view}_off', enabled=False, view=view)) == 0,
              f'FSR changed the {view} diagnostic view')
    print('FSR 1: render-area equivalence, sharpening, native/supersampling and debug bypass passed')


if __name__ == '__main__':
    main()
