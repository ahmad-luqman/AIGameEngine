#!/usr/bin/env python3
"""GPU regression: each point face and a spot must cast shadows, honor both switches and filter edges.

Uses only Python's standard library and basalt image.compare, so the same assertions run on MoltenVK
and lavapipe. Generated projects and renders live in the build tree; no source assets are modified.
"""
import argparse
import json
import math
import os
import sys
from pathlib import Path
import subprocess
import struct


def add(a, b):
    return [x + y for x, y in zip(a, b)]


def mul(v, n):
    return [x * n for x in v]


def rotation(direction):
    length = math.sqrt(sum(x * x for x in direction))
    x, y, z = [v / length for v in direction]
    return [math.degrees(math.asin(y)), math.degrees(math.atan2(-x, -z)), 0]


def run(command):
    env = dict(os.environ)
    if sys.platform == "darwin":
        env.setdefault("DYLD_FALLBACK_LIBRARY_PATH", "/opt/homebrew/lib:/usr/local/lib")
    result = subprocess.run(command, text=True, capture_output=True, env=env)
    if result.returncode:
        raise RuntimeError(f"{' '.join(map(str, command))}\n{result.stdout}\n{result.stderr}")
    return result.stdout if command[1] == "image.compare" else result.stdout + result.stderr


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--runtime', required=True)
    parser.add_argument('--cli', required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    (args.output / 'Project.bproject').write_text(json.dumps({
        'Format': 'BasaltProject', 'Name': 'LocalShadows', 'StartScene': 'Scene.bscene'
    }))

    def render(name, scene):
        (args.output / 'Scene.bscene').write_text(json.dumps(scene))
        image = args.output / f'{name}.png'
        log = run([args.runtime, '--project', str(args.output), '--offscreen', '--frames', '6',
                   '--width', '256', '--height', '256', '--require-validation', '--screenshot', str(image)])
        (args.output / f'{name}.log').write_text(log)
        if 'validation enabled' not in log:
            raise AssertionError('Vulkan validation did not load')
        return image

    def difference(a, b):
        result = json.loads(run([args.cli, 'image.compare', json.dumps({
            'actual': str(a), 'reference': str(b), 'pixelThreshold': 2, 'maxPercent': 0
        })]))
        if not result['ok']:
            raise AssertionError(result)
        return result['result']['differingPercent']

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
        kind = 'PointLight' if case < 6 else 'SpotLight'
        # The receiver is a thin cube, avoiding dependence on a plane's front-face orientation.
        tangent = (0, 1, 0) if direction[0] else (1, 0, 0)
        side = (0, 0, 1) if direction[2] == 0 else (0, 1, 0)
        receiver = mul(direction, 5)
        camera = add(add(mul(direction, -1), mul(tangent, 6)), mul(side, 3))
        light = {'Intensity': 80, 'CastShadows': True, 'ShadowSoftness': 0}
        light.update({'Radius': 15} if kind == 'PointLight' else {'Range': 15, 'InnerConeAngle': 30, 'OuterConeAngle': 45})
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
                'Transform': {'Translation': mul(direction, 2.5)},
                'Mesh': {'Mesh': 'builtin://Cube', 'CastShadows': True},
                'Material': {'AlbedoColor': [1, 1, 1, 1], 'Roughness': 1}}},
            {'ID': 5, 'Name': 'DarkSky', 'Components': {'SkyLight': {'Intensity': 0, 'ShowBackground': False}}},
            # A higher-ranked dark light occupies tiles 0..5. The tested cube spans atlas rows, and the
            # spotlight has a nonzero base map. Its zero radiance keeps the reference images isolated.
            {'ID': 2, 'Name': 'AtlasOccupant', 'Components': {
                'PointLight': {'Color': [0, 0, 0], 'Intensity': 1, 'Radius': 100, 'CastShadows': True}}}
        ]
        scene = {'Format': 'BasaltScene', 'Version': 1, 'Name': 'LocalShadows',
                 'Renderer': {'AmbientColor': [0, 0, 0], 'SSAOEnabled': False, 'ShadowsEnabled': True}, 'Entities': entities}
        name = f'{kind}_{case}'
        shadow = render(name, scene)
        light['CastShadows'] = False
        unshadowed = render(name + '_disabled', scene)
        delta = difference(shadow, unshadowed)
        assert delta > 0.2, (name, 'missing shadow', delta)
        light['CastShadows'] = True
        scene['Renderer']['ShadowsEnabled'] = False
        global_off = render(name + '_global_off', scene)
        assert difference(unshadowed, global_off) == 0, (name, 'global shadow switch ignored')
        scene['Renderer']['ShadowsEnabled'] = True
        light['ShadowSoftness'] = 4
        soft = render(name + '_soft', scene)
        assert difference(shadow, soft) > 0.02, (name, 'softness has no effect')
        light['ShadowSoftness'] = 0
        entities[3]['Components']['Mesh']['CastShadows'] = False
        caster_off = render(name + '_caster_off', scene)
        assert difference(shadow, caster_off) > 0.2, (name, 'mesh shadow switch ignored')
        if case in (3, 6):
            entities[3]['Components'].pop('Material')
            for alpha in (0, 1):
                mesh = entities[3]['Components']['Mesh']
                mesh.update({'Mesh': f'Masked{alpha}.glb', 'CastShadows': True})
                masked = render(name + f'_mask{alpha}', scene)
                mesh['CastShadows'] = False
                masked_off = render(name + f'_mask{alpha}_off', scene)
                masked_delta = difference(masked, masked_off)
                assert (masked_delta == 0 if alpha == 0 else masked_delta > 0.2), (name, 'alpha mask', alpha, masked_delta)
            mesh.update({'Mesh': 'builtin://Cube', 'CastShadows': True})
            # A close view of the receiving wall places the cube above the camera's frustum.
            camera = add(add(mul(direction, 3), mul(tangent, 4)), mul(side, 2))
            entities[0]['Components']['Transform'] = {'Translation': camera, 'Rotation': rotation(add(receiver, mul(camera, -1)))}
            entities[0]['Components']['Camera']['FOV'] = 30
            offscreen = render(name + '_offscreen_caster', scene)
            mesh['CastShadows'] = False
            offscreen_off = render(name + '_offscreen_caster_off', scene)
            assert difference(offscreen, offscreen_off) > 0.2, (name, 'offscreen caster was culled')
        print(f'{name}: shadow, softness, light/mesh/global switches passed ({delta:.2f}% shadow coverage)', flush=True)


if __name__ == '__main__':
    main()
