#!/usr/bin/env python3
"""GPU regressions for cube faces, spots, three shadow switches, PCF, masks and off-camera casters.
Also checks atlas lifetime/budgets, invalid transforms, moving casters, bias and feature-scene coverage.

Uses only Python's standard library and basalt image.compare, so the same assertions run on MoltenVK
and lavapipe. Generated projects and renders live in the build tree; no source assets are modified.
"""
import argparse
import copy
import json
import math
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import zlib


def add(a, b):
    return [x + y for x, y in zip(a, b)]


def mul(v, n):
    return [x * n for x in v]


def rotation(direction):
    length = math.sqrt(sum(x * x for x in direction))
    x, y, z = [v / length for v in direction]
    return [math.degrees(math.asin(y)), math.degrees(math.atan2(-x, -z)), 0]


def check(condition, message):
    # Unlike bare assert, regression checks must also run under PYTHONOPTIMIZE/python -O.
    if not condition:
        raise AssertionError(message)


def run(command):
    env = dict(os.environ)
    # macOS needs this for Homebrew validation layers; SIP can strip an inherited DYLD variable.
    if sys.platform == "darwin":
        env.setdefault("DYLD_FALLBACK_LIBRARY_PATH", "/opt/homebrew/lib:/usr/local/lib")
    result = subprocess.run(command, text=True, capture_output=True, env=env, timeout=60)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, command))}\n{result.stdout}\n{result.stderr}")
    return result


def capture(command, image, expected_maps=None, expected_atlas=None):
    image.unlink(missing_ok=True)
    result = run(command)
    log = result.stdout + result.stderr
    image.with_suffix('.log').write_text(log)
    check(image.is_file(), f'{image.stem}: runtime did not write a fresh screenshot')
    check(not re.search(r'SceneRenderer:.*(?:failed|cannot)|Script error:|\[Vulkan\].*error|\[nvrhi\].*error', log, re.IGNORECASE), f'{image.stem}: renderer failed\n{log}')
    stats = re.search(r'localShadowMaps=(\d+) localShadowsSkipped=(\d+) lightsSkipped=(\d+) localShadowAtlasSize=(\d+)', log)
    check(stats is not None, f'{image.stem}: missing render statistics')
    if expected_maps is not None:
        check(int(stats[1]) == expected_maps, f'{image.stem}: wrong map count: {stats[0]}')
    if expected_atlas is not None:
        check(int(stats[4]) == expected_atlas, f'{image.stem}: wrong atlas size: {stats[0]}')
    if 'validation enabled' not in log:
        raise AssertionError('Vulkan validation did not load')
    return image


def comparison_percent(result):
    check(result.get('ok') and result.get('result', {}).get('sizeMatches'), result)
    return result['result']['differingPercent']


def read_png(path):
    """Decode the engine's non-interlaced RGB/RGBA8 screenshots using only the standard library."""
    data = path.read_bytes()
    check(data[:8] == b'\x89PNG\r\n\x1a\n', f'{path}: not a PNG')
    packed = bytearray()
    offset = 8
    while offset < len(data):
        size, kind = struct.unpack_from('>I4s', data, offset)
        chunk = data[offset + 8:offset + 8 + size]
        if kind == b'IHDR':
            width, height, depth, color, compression, filtering, interlace = struct.unpack('>IIBBBBB', chunk)
            check(depth == 8 and color in (2, 6) and not (compression or filtering or interlace), 'unsupported PNG format')
        elif kind == b'IDAT':
            packed.extend(chunk)
        offset += size + 12
    channels = 3 if color == 2 else 4
    raw = zlib.decompress(packed)
    stride = width * channels
    check(len(raw) == height * (stride + 1), 'invalid PNG data length')
    previous = bytearray(stride)
    pixels = []
    for y in range(height):
        start = y * (stride + 1)
        filter_type = raw[start]
        check(filter_type in range(5), 'invalid PNG filter')
        row = bytearray(raw[start + 1:start + 1 + stride])
        for x in range(stride):
            left = row[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            prediction = left + up - corner
            distances = [abs(prediction - v) for v in (left, up, corner)]
            paeth = (left, up, corner)[distances.index(min(distances))]
            row[x] = (row[x] + (0, left, up, (left + up) // 2, paeth)[filter_type]) % 256
        pixels.extend(tuple(row[x:x + 3]) for x in range(0, stride, channels))
        previous = row
    return width, height, pixels


def shadow_pixels(shadow, disabled):
    width, height, pixels = read_png(shadow)
    other_width, other_height, reference = read_png(disabled)
    check((width, height) == (other_width, other_height), 'screenshot dimensions differ')
    return [(i % width, i // width) for i, (a, b) in enumerate(zip(pixels, reference)) if max(abs(x - y) for x, y in zip(a, b)) > 2]


def dot(a, b):
    return sum(x * y for x, y in zip(a, b))


def cross(a, b):
    return [a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]]


def normalized(v):
    return mul(v, 1 / math.sqrt(dot(v, v)))


def project(position, camera, target, fov=55):
    forward = normalized(add(target, mul(camera, -1)))
    right = normalized(cross(forward, [0, 1, 0]))
    up = cross(right, forward)
    relative = add(position, mul(camera, -1))
    scale = 128 / (dot(relative, forward) * math.tan(math.radians(fov / 2)))
    return (128 + scale * dot(relative, right), 128 - scale * dot(relative, up))


def check_shadow_location(shadow, disabled, caster, direction, camera, receiver):
    # Project the eight caster corners along rays from the light onto the receiver's near face.
    # A small padding allows PCF, world bias and rasterization while rejecting mirrored UVs/acne.
    corners = []
    for x in (-0.5, 0.5):
        for y in (-0.5, 0.5):
            for z in (-0.5, 0.5):
                corner = add(caster, [x, y, z])
                point = mul(corner, 4.95 / dot(corner, direction))
                corners.append(project(point, camera, receiver))
    bounds = [min(v[axis] for v in corners) - 3 for axis in (0, 1)] + [max(v[axis] for v in corners) + 3 for axis in (0, 1)]
    pixels = shadow_pixels(shadow, disabled)
    check(len(pixels) > 130, f'{shadow}: missing shadow')
    outside = sum(not (bounds[0] <= x <= bounds[2] and bounds[1] <= y <= bounds[3]) for x, y in pixels)
    check(outside < 35, f'{shadow}: {outside} changed pixels outside projected shadow {bounds}')
    center = tuple(sum(p[axis] for p in pixels) / len(pixels) for axis in (0, 1))
    expected = project(mul(caster, 4.95 / dot(caster, direction)), camera, receiver)
    check(math.dist(center, expected) < 25, f'{shadow}: shadow centroid {center}, expected {expected}')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--case', type=int, choices=range(8), help='Run one cube face (0–5), spot (6), or feature scene (7)')
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'Project.bproject').write_text(json.dumps({
        'Format': 'BasaltProject', 'Name': 'LocalShadows', 'StartScene': 'Scene.bscene'
    }))

    def render(name, scene, expected_maps=None, expected_atlas=None, project=None, frames=6, size=256):
        (args.output / 'Scene.bscene').write_text(json.dumps(scene))
        image = args.output / f'{name}.png'
        return capture([args.runtime, '--project', str(project or args.output), '--scene', str((args.output / 'Scene.bscene').resolve()),
                        '--offscreen', '--frames', str(frames), '--width', str(size), '--height', str(size),
                        '--require-validation', '--screenshot', str(image)], image, expected_maps, expected_atlas)

    def difference(a, b):
        # CLI stdout is JSON; diagnostics belong to stderr. Runtime logs use both streams.
        result = json.loads(run([args.cli, 'image.compare', json.dumps({
            'actual': str(a), 'reference': str(b), 'pixelThreshold': 2, 'maxPercent': 0
        })]).stdout)
        return comparison_percent(result)

    # Reuse the feature fixture's embedded cube geometry; make fully discarded and fully covered MASK
    # materials. This tests the shadow fragment discard without relying on a driver's edge rasterization.
    template = Path(__file__).resolve().parents[1] / 'Data/FeatureTest/Assets/Models/TexturedCube.glb'
    glb = template.read_bytes()
    json_size = struct.unpack_from('<I', glb, 12)[0]
    model = json.loads(glb[20:20 + json_size])
    binary_chunk = glb[20 + json_size:]
    for alpha in (0, 1):
        model['materials'] = [{'alphaMode': 'MASK', 'alphaCutoff': 0.5,
                              'pbrMetallicRoughness': {'baseColorFactor': [1, 1, 1, alpha], 'roughnessFactor': 1}}]
        encoded = json.dumps(model).encode()
        encoded += b' ' * (-len(encoded) % 4)
        data = struct.pack('<III', 0x46546C67, 2, 20 + len(encoded) + len(binary_chunk))
        data += struct.pack('<II', len(encoded), 0x4E4F534A) + encoded + binary_chunk
        (args.output / f'Masked{alpha}.glb').write_bytes(data)

    directions = [(1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1)]
    for case, direction in enumerate(directions + [(0, -1, 0)]):
        if args.case is not None and case != args.case:
            continue
        kind = 'PointLight' if case < 6 else 'SpotLight'
        # The receiver is a thin cube, avoiding dependence on a plane's front-face orientation.
        tangent = (0, 1, 0) if direction[0] else (1, 0, 0)
        side = (0, 0, 1) if direction[2] == 0 else (0, 1, 0)
        receiver = mul(direction, 5)
        camera = add(add(mul(direction, -1), mul(tangent, 6)), mul(side, 3))
        light = {'Intensity': 80, 'CastShadows': True, 'ShadowSoftness': 0}
        light.update({'Radius': 15} if kind == 'PointLight' else {'Range': 15, 'InnerConeAngle': 30, 'OuterConeAngle': 45})
        caster = add(add(mul(direction, 2.5), mul(tangent, 0.8)), mul(side, 0.55))
        entities = [
            {'ID': 1, 'Name': 'Camera', 'Components': {
                'Transform': {'Translation': camera, 'Rotation': rotation(add(receiver, mul(camera, -1)))},
                'Camera': {'FOV': 55, 'Near': 0.1, 'Far': 100, 'Primary': True}}},
            {'ID': 20, 'Name': 'Light', 'Components': {
                'Transform': {'Rotation': rotation(direction)}, kind: light}},
            {'ID': 3, 'Name': 'Receiver', 'Components': {
                'Transform': {'Translation': receiver, 'Scale': [0.1 if v else 10 for v in direction]},
                'Mesh': {'Mesh': 'builtin://Cube'}, 'Material': {'AlbedoColor': [1, 1, 1, 1], 'Roughness': 1}}},
            {'ID': 4, 'Name': 'Caster', 'Components': {
                'Transform': {'Translation': caster},
                'Mesh': {'Mesh': 'builtin://Cube', 'CastShadows': True},
                'Material': {'AlbedoColor': [1, 1, 1, 1], 'Roughness': 1}}},
            {'ID': 5, 'Name': 'DarkSky', 'Components': {'SkyLight': {'Intensity': 0, 'ShowBackground': False}}},
            # Both lights reach the full-screen importance cap: UUID 2 < 20 makes this dark light
            # occupy tiles 0..5. The tested cube then spans rows; the spot has a nonzero base map.
            # A different origin ensures using the occupant's matrices cannot produce the right shadow.
            {'ID': 2, 'Name': 'AtlasOccupant', 'Components': {
                'Transform': {'Translation': [-3, 2, 4]},
                'PointLight': {'Color': [0, 0, 0], 'Intensity': 1, 'Radius': 100, 'CastShadows': True}}}
        ]
        scene = {'Format': 'BasaltScene', 'Version': 1, 'Name': 'LocalShadows',
                 'Renderer': {'AmbientColor': [0, 0, 0], 'SSAOEnabled': False, 'ShadowsEnabled': True}, 'Entities': entities}
        name = f'{kind}_{case}'
        shadow = render(name, scene, 12 if case < 6 else 7, 4096)
        light['CastShadows'] = False
        unshadowed = render(name + '_disabled', scene)
        delta = difference(shadow, unshadowed)
        check(delta > 0.2, (name, 'missing shadow', delta))
        check_shadow_location(shadow, unshadowed, caster, direction, camera, receiver)
        light['CastShadows'] = True
        scene['Renderer']['ShadowsEnabled'] = False
        global_off = render(name + '_global_off', scene, 0, 1)
        check(difference(unshadowed, global_off) == 0, (name, 'global shadow switch ignored'))
        scene['Renderer']['ShadowsEnabled'] = True
        light['ShadowSoftness'] = 4
        soft = render(name + '_soft', scene)
        check(difference(shadow, soft) > 0.02, (name, 'softness has no effect'))
        light['ShadowSoftness'] = 0
        entities[3]['Components']['Mesh']['CastShadows'] = False
        caster_off = render(name + '_caster_off', scene)
        check(difference(shadow, caster_off) > 0.2, (name, 'mesh shadow switch ignored'))
        if case in (3, 6):
            moving = copy.deepcopy(scene)
            moved = add(caster, mul(tangent, -1.5))
            (args.output / 'Move.lua').write_text(
                'local M = {}\nfunction M:OnUpdate(dt)\n'
                ' self.Frame = (self.Frame or 0) + 1\n'
                f' if self.Frame == 3 then self.Entity.Translation = Vec3({moved[0]}, {moved[1]}, {moved[2]}) end\n'
                'end\nreturn M\n')
            moving['Entities'][3]['Components']['Mesh']['CastShadows'] = True
            moving['Entities'][3]['Components']['Script'] = {'Script': 'Move.lua'}
            animated = render(name + '_moving', moving)
            del moving['Entities'][3]['Components']['Script']
            moving['Entities'][3]['Components']['Transform']['Translation'] = moved
            final_position = render(name + '_moved_static', moving)
            check(difference(animated, final_position) == 0, (name, 'stale shadow depth after caster moved'))

        if case == 6:
            activation = copy.deepcopy(scene)
            activation['Entities'].pop()  # No dark occupant: the first two frames must use the dummy.
            activation['Entities'][3]['Components']['Mesh']['CastShadows'] = True
            activation['Entities'][1]['Components'][kind]['CastShadows'] = False
            activation['Entities'][1]['Components']['Script'] = {'Script': 'Enable.lua'}
            (args.output / 'Enable.lua').write_text(
                'local M = {}\nfunction M:OnUpdate(dt)\n'
                ' self.Frame = (self.Frame or 0) + 1\n'
                ' if self.Frame == 3 then self.Entity:SetComponent("SpotLight", { CastShadows = true }) end\n'
                'end\nreturn M\n')
            enabled_later = render('Spot_enable_later', activation, 1, 4096)
            del activation['Entities'][1]['Components']['Script']
            activation['Entities'][1]['Components'][kind]['CastShadows'] = True
            enabled_initially = render('Spot_enable_initially', activation, 1, 4096)
            check(difference(enabled_later, enabled_initially) == 0, 'lazy atlas did not replace existing fallback bindings')

            invalid = copy.deepcopy(scene)
            invalid['Entities'][1]['Components']['Transform']['Scale'] = [0, 0, 0]
            zero_scale = render('Spot_zero_scale', invalid, 6)
            invalid['Entities'][1]['Components']['Transform']['Scale'] = [1e30, 1e30, 1e30]
            nonfinite = render('Spot_nonfinite_direction', invalid, 6)
            invalid['Entities'].pop(1)
            no_spot = render('Spot_removed', invalid, 6)
            check(difference(zero_scale, no_spot) == 0, 'zero-scale spot was not skipped')
            check(difference(nonfinite, no_spot) == 0, 'non-finite spot direction was not skipped')
            check((args.output / 'Spot_nonfinite_direction.log').read_text().count('skipped spot lights with non-finite') == 1,
                  'non-finite direction warning must occur once per episode')

            grazing = copy.deepcopy(scene)
            grazing['Entities'] = grazing['Entities'][:3] + [grazing['Entities'][4]]
            grazing['Entities'][2]['Components']['Transform']['Rotation'] = [0, 0, -65]
            grazing['Renderer'].update({'ShadowBias': 0, 'ShadowNormalBias': 0.1})
            grazing_on = render('Spot_grazing_normal_bias', grazing, 1)
            grazing['Entities'][1]['Components'][kind]['CastShadows'] = False
            grazing_off = render('Spot_grazing_disabled', grazing, 0, 1)
            check(sum(max(pixel) > 32 for pixel in read_png(grazing_off)[2]) > 500, 'grazing receiver must be visibly lit')
            acne = difference(grazing_on, grazing_off)
            check(acne < 0.05, ('normal bias left self-shadowing on a grazing receiver', acne))

            # Raster slope bias already protects this flat plane. A caster makes the world-normal
            # offset observable as a shifted contact edge, independently of that raster fallback.
            grazing['Entities'][1]['Components'][kind]['CastShadows'] = True
            grazing_caster = copy.deepcopy(entities[3])
            grazing_caster['Components']['Mesh']['CastShadows'] = True
            grazing_caster['Components']['Transform']['Translation'] = [0, -2, 0]
            grazing['Entities'].append(grazing_caster)
            normal_on = render('Spot_grazing_caster_bias', grazing)
            grazing['Renderer']['ShadowNormalBias'] = 0
            normal_off = render('Spot_grazing_caster_no_bias', grazing)
            check(difference(normal_on, normal_off) > 0.02, 'normal bias does not affect grazing contact shadows')
            grazing['Entities'].pop()

            budget = copy.deepcopy(grazing)
            budget['Entities'] = [budget['Entities'][0], budget['Entities'][2], budget['Entities'][3]]
            for i in range(11):
                budget['Entities'].append({'ID': 100 + i, 'Components': {'PointLight': {
                    'Radius': 100, 'Color': [0, 0, 0], 'CastShadows': True}}})
            for i in range(5):
                budget['Entities'].append({'ID': 200 + i, 'Components': {'SpotLight': {
                    'Range': 100, 'OuterConeAngle': 80, 'Color': [0, 0, 0], 'CastShadows': True}}})
            render('Atlas_overflow', budget, 64, 4096)
            budget_log = (args.output / 'Atlas_overflow.log').read_text()
            check('localShadowsSkipped=2' in budget_log, 'atlas overflow skipped count incorrect')
            check(budget_log.count('local shadow budget 64 exceeded') == 1, 'atlas overflow must warn once per episode')
            budget['Entities'] = budget['Entities'][:3]
            for i in range(258):
                budget['Entities'].append({'ID': 100 + i, 'Components': {'PointLight': {'Intensity': 0, 'CastShadows': True}}})
            render('Light_overflow', budget, 0, 1)
            budget_log = (args.output / 'Light_overflow.log').read_text()
            check('lightsSkipped=2' in budget_log, 'light overflow skipped count incorrect')
            check(budget_log.count('light budget 256 exceeded') == 1, 'light overflow must warn once per episode')

        if case in (3, 6):
            entities[3]['Components'].pop('Material')
            for alpha in (0, 1):
                mesh = entities[3]['Components']['Mesh']
                mesh.update({'Mesh': f'Masked{alpha}.glb', 'CastShadows': True})
                masked = render(name + f'_mask{alpha}', scene)
                mesh['CastShadows'] = False
                masked_off = render(name + f'_mask{alpha}_off', scene)
                masked_delta = difference(masked, masked_off)
                check((masked_delta == 0 if alpha == 0 else masked_delta > 0.2), (name, 'alpha mask', alpha, masked_delta))
            mesh.update({'Mesh': 'builtin://Cube', 'CastShadows': True})
            # A close view of the receiving wall places the cube above the camera's frustum.
            camera = add(add(mul(direction, 3), mul(tangent, 4)), mul(side, 2))
            entities[0]['Components']['Transform'] = {'Translation': camera, 'Rotation': rotation(add(receiver, mul(camera, -1)))}
            entities[0]['Components']['Camera']['FOV'] = 30
            offscreen = render(name + '_offscreen_caster', scene)
            mesh['CastShadows'] = False
            offscreen_off = render(name + '_offscreen_caster_off', scene)
            check(difference(offscreen, offscreen_off) > 0.2, (name, 'offscreen caster was culled'))
        print(f'{name}: shadow, softness, light/mesh/global switches passed ({delta:.2f}% shadow coverage)', flush=True)

    if args.case in (None, 7):
        feature = Path(__file__).resolve().parents[1] / 'Data/FeatureTest'
        fixture = json.loads((feature / 'Assets/Scenes/FeatureTest.bscene').read_text())
        # Freeze scripted gameplay so both renders compare the same authored feature scene.
        for entity in fixture['Entities']:
            entity['Components'].pop('Script', None)
        feature_on = render('Feature_local_on', fixture, project=feature, size=512)
        for entity in fixture['Entities']:
            for kind in ('PointLight', 'SpotLight'):
                if kind in entity['Components']:
                    entity['Components'][kind]['CastShadows'] = False
        feature_off = render('Feature_local_off', fixture, project=feature, size=512)
        delta = difference(feature_on, feature_off)
        check(delta > 0.1, ('feature scene must exercise local shadows independently of golden tolerance', delta))
        print(f'Feature scene: local shadows change {delta:.3f}% of pixels', flush=True)


if __name__ == '__main__':
    main()
