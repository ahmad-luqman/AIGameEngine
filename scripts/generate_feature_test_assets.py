#!/usr/bin/env python3
"""Generates the small binary assets of the feature-test project (deterministic, no dependencies).

Run from the repository root:  python3 scripts/generate_feature_test_assets.py
"""
import json, math, os, struct, zlib

ROOT = os.path.join(os.path.dirname(__file__), "..", "Tests", "Data", "FeatureTest", "Assets")

def png(width, height, pixel):
    rows = b"".join(b"\x00" + b"".join(bytes(pixel(x, y)) for x in range(width)) for y in range(height))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    return b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b"")

def wav(seconds, rate=22050, frequency=440.0):
    samples = [int(6000 * math.sin(2 * math.pi * frequency * i / rate)) for i in range(int(seconds * rate))]
    data = struct.pack("<%dh" % len(samples), *samples)
    return b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(data)) + data

def checker(x, y):
    return (230, 230, 230, 255) if ((x // 8) + (y // 8)) % 2 == 0 else (40, 40, 40, 255)

def write(path, data):
    full = os.path.join(ROOT, path)
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, "wb") as f:
        f.write(data)

def glb_textured_cube():
    # A unit cube with UVs and an embedded checker texture, as binary glTF.
    faces = [((1,0,0),(0,1,0)), ((-1,0,0),(0,1,0)), ((0,1,0),(0,0,-1)), ((0,-1,0),(0,0,1)), ((0,0,1),(0,1,0)), ((0,0,-1),(0,1,0))]
    positions, normals, uvs, indices = [], [], [], []
    for n, up in faces:
        right = (up[1]*n[2]-up[2]*n[1], up[2]*n[0]-up[0]*n[2], up[0]*n[1]-up[1]*n[0])
        base = len(positions)
        for cx, cy, u, v in [(-1,-1,0,1),(1,-1,1,1),(1,1,1,0),(-1,1,0,0)]:
            positions.append(tuple(0.5*(n[i] + right[i]*cx + up[i]*cy) for i in range(3)))
            normals.append(n)
            uvs.append((u, v))
        indices += [base, base+1, base+2, base, base+2, base+3]
    image = png(32, 32, checker)
    blob = b"".join(struct.pack("<3f", *p) for p in positions)
    blob += b"".join(struct.pack("<3f", *n) for n in normals)
    blob += b"".join(struct.pack("<2f", *t) for t in uvs)
    blob += struct.pack("<%dH" % len(indices), *indices)
    image_offset = len(blob)
    blob += image
    while len(blob) % 4:
        blob += b"\x00"
    count = len(positions)
    gltf = {
        "asset": {"version": "2.0", "generator": "Basalt feature-test generator"},
        "buffers": [{"byteLength": len(blob)}],
        "bufferViews": [
            {"buffer": 0, "byteOffset": 0, "byteLength": count * 12},
            {"buffer": 0, "byteOffset": count * 12, "byteLength": count * 12},
            {"buffer": 0, "byteOffset": count * 24, "byteLength": count * 8},
            {"buffer": 0, "byteOffset": count * 32, "byteLength": len(indices) * 2},
            {"buffer": 0, "byteOffset": image_offset, "byteLength": len(image)},
        ],
        "accessors": [
            {"bufferView": 0, "componentType": 5126, "count": count, "type": "VEC3", "min": [-0.5]*3, "max": [0.5]*3},
            {"bufferView": 1, "componentType": 5126, "count": count, "type": "VEC3"},
            {"bufferView": 2, "componentType": 5126, "count": count, "type": "VEC2"},
            {"bufferView": 3, "componentType": 5123, "count": len(indices), "type": "SCALAR"},
        ],
        "images": [{"bufferView": 4, "mimeType": "image/png"}],
        "textures": [{"source": 0}],
        "materials": [{"name": "Checker", "pbrMetallicRoughness": {"baseColorTexture": {"index": 0}, "metallicFactor": 0.0, "roughnessFactor": 0.6}}],
        "meshes": [{"name": "TexturedCube", "primitives": [{"attributes": {"POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2}, "indices": 3, "material": 0}]}],
        "nodes": [{"name": "CubeRoot", "children": [1]}, {"name": "CubeMesh", "mesh": 0}],
        "scenes": [{"nodes": [0]}],
        "scene": 0,
    }
    text = json.dumps(gltf).encode()
    while len(text) % 4:
        text += b" "
    return b"glTF" + struct.pack("<II", 2, 12 + 8 + len(text) + 8 + len(blob)) + struct.pack("<I", len(text)) + b"JSON" + text + struct.pack("<I", len(blob)) + b"BIN\x00" + blob

if __name__ == "__main__":
    write("Textures/Checker.png", png(64, 64, checker))
    write("Textures/FlatNormal.png", png(4, 4, lambda x, y: (128, 128, 255, 255)))
    write("Audio/Beep.wav", wav(0.2))
    write("Models/TexturedCube.glb", glb_textured_cube())
    print("Feature-test assets generated.")
