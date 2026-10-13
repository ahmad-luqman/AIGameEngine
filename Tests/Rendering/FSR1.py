#!/usr/bin/env python3
"""End-to-end FSR 1 checks on MoltenVK and lavapipe; generated files stay in the build tree."""
import argparse
import copy
import json
from pathlib import Path
import re

from LocalShadows import capture, check, comparison_percent, run


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
    # SSAO's normalized sampling is not bit-exact across odd backing sizes. Isolate EASU here;
    # the even-size pair below covers the complete SSAO -> tonemap -> FSR path.
    fixture['Renderer']['SSAOEnabled'] = False

    def render(name, enabled=True, scale=0.5, sharpness=0.2, dynamic=False, view='None', size=(321, 181)):
        scene = copy.deepcopy(fixture)
        scene['Renderer'].update(FSR1Enabled=enabled, FSR1Sharpness=sharpness)
        scene_path = args.output / f'{name}.bscene'
        scene_path.write_text(json.dumps(scene))
        image = args.output / f'{name}.png'
        command = [args.runtime, '--project', str(source), '--scene', str(scene_path.resolve()),
                   '--offscreen', '--frames', '90' if dynamic else '6', '--width', str(size[0]),
                   '--height', str(size[1]), '--require-validation', '--screenshot', str(image),
                   '--debug-view', view, '--render-scale', '1' if dynamic else str(scale)]
        if dynamic:
            command += ['--dynamic-resolution', '1000000', '--min-render-scale', str(scale)]
        capture(command, image)
        if dynamic:
            log = image.with_suffix('.log').read_text()
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

    for scale in (0.25, 0.5, 0.67):
        fixed = render(f'fixed_{scale}', scale=scale)
        dynamic = render(f'dynamic_{scale}', scale=scale, dynamic=True)
        check(difference(fixed, dynamic) == 0, f'FSR reads outside the render area at {scale}')
    fixture['Renderer']['SSAOEnabled'] = True
    check(difference(render('ssao_fixed', size=(320, 180)),
                     render('ssao_dynamic', dynamic=True, size=(320, 180))) == 0,
          'full-stack dynamic FSR differs with SSAO')
    fixture['Renderer']['SSAOEnabled'] = False
    fsr = render('fsr')
    bilinear = render('bilinear', enabled=False)
    check(difference(fsr, bilinear, 2) > 1, 'FSR did not visibly change the scaled image')
    strong = render('strong', sharpness=0)
    weak = render('weak', sharpness=2)
    check(difference(strong, weak, 2) > 0.1, 'RCAS sharpness is ineffective')
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
