"""Rebuild ManyMorphs.gltf in Animation for the GPU morph packing tests.
Two meshes share an 8x6 grid facing +Z with 40 targets, each pushing one vertex forward. Sparse shows its mesh's 20
nonzero default weights (pairs past the first draw record); Crowded's node weights make 36 targets active, past the
renderer's limit of 32, with distinct magnitudes so the strongest are well defined.
"""
import base64
import json
from pathlib import Path
import struct

blob = bytearray()
views = []
accessors = []


def accessor(values, width, kind='f', **extra):
    while len(blob) % 4:
        blob.append(0)
    raw = struct.pack('<' + kind * len(values), *values)
    views.append(dict(buffer=0, byteOffset=len(blob), byteLength=len(raw)))
    blob.extend(raw)
    accessors.append(dict(bufferView=len(views) - 1, componentType={'f': 5126, 'H': 5123}[kind],
                          count=len(values) // width, type={1: 'SCALAR', 3: 'VEC3'}[width], **extra))
    return len(accessors) - 1


columns, rows, targets = 8, 6, 40
grid = [(x * 0.2, y * 0.2, 0.0) for y in range(rows) for x in range(columns)]
positions = accessor([c for p in grid for c in p], 3, min=[0, 0, 0], max=[(columns - 1) * 0.2, (rows - 1) * 0.2, 0])
normals = accessor([0, 0, 1] * len(grid), 3)
quads = [(y * columns + x, y * columns + x + 1, (y + 1) * columns + x + 1, (y + 1) * columns + x) for y in range(rows - 1) for x in range(columns - 1)]
indices = accessor([i for a, b, c, d in quads for i in (a, b, c, a, c, d)], 1, 'H')
deltas = []
for t in range(targets):
    vertex = 4 + t  # Skip the bottom-left corner so every target moves an interior or edge vertex.
    values = [c for v in range(len(grid)) for c in ((0, 0, 0.15) if v == vertex else (0, 0, 0))]
    deltas.append(accessor(values, 3, min=[0, 0, 0], max=[0, 0, 0.15]))
primitive = dict(attributes=dict(POSITION=positions, NORMAL=normals), indices=indices, material=0, targets=[dict(POSITION=d) for d in deltas])
sparse_defaults = [round(0.05 + 0.045 * t, 3) if t < 20 else 0 for t in range(targets)]
crowded = [round(0.2 + 0.02 * t, 3) if t < 36 else 0 for t in range(targets)]
model = dict(
    asset=dict(version='2.0', generator='Basalt test fixture'),
    buffers=[dict(byteLength=len(blob), uri='data:application/octet-stream;base64,' + base64.b64encode(blob).decode())],
    bufferViews=views, accessors=accessors,
    materials=[dict(name='Surface', pbrMetallicRoughness=dict(baseColorFactor=[0.7, 0.75, 0.8, 1], metallicFactor=0, roughnessFactor=0.5), doubleSided=True)],
    meshes=[dict(name='Sparse', weights=sparse_defaults, primitives=[primitive]), dict(name='Crowded', primitives=[primitive])],
    nodes=[dict(name='Sparse', mesh=0, translation=[-1.6, 0, 0]), dict(name='Crowded', mesh=1, weights=crowded, translation=[0.2, 0, 0])],
    scenes=[dict(nodes=[0, 1])], scene=0)
(Path(__file__).resolve().parent / 'ManyMorphs.gltf').write_text(json.dumps(model, indent=2) + '\n')
