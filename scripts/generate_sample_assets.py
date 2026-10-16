#!/usr/bin/env python3
"""Generates the sound effects of the Marble Run sample (deterministic, no dependencies).

Run from the repository root:  python3 scripts/generate_sample_assets.py
"""
import math, os, struct

RATE = 22050
ROOT = os.path.join(os.path.dirname(__file__), "..", "Samples", "MarbleRun", "Assets", "Audio")

def wav(samples):
    # 16-bit mono PCM; samples are floats in -1..1.
    data = struct.pack("<%dh" % len(samples), *(int(max(-1.0, min(1.0, s)) * 32767) for s in samples))
    return b"RIFF" + struct.pack("<I", 36 + len(data)) + b"WAVEfmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16) + b"data" + struct.pack("<I", len(data)) + data

def chime(seconds=0.9):
    # A small bell: two inharmonic partials with an exponential decay and a 5 ms attack.
    out = []
    for i in range(int(seconds * RATE)):
        t = i / RATE
        envelope = min(1.0, t / 0.005) * math.exp(-4.0 * t)
        out.append(envelope * (0.45 * math.sin(2 * math.pi * 1046.5 * t) + 0.25 * math.sin(2 * math.pi * 2637.0 * t)))
    return out

def clack(seconds=0.07):
    # A short wooden knock: a fast-decaying resonance plus a little pseudo-random noise (fixed LCG, so the file never changes).
    out, state = [], 12345
    for i in range(int(seconds * RATE)):
        t = i / RATE
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        noise = state / 0x3FFFFFFF - 1.0
        out.append(math.exp(-70.0 * t) * (0.6 * math.sin(2 * math.pi * 1800.0 * t) + 0.25 * noise))
    return out

def hum(seconds=1.0):
    # The spinner's motor: whole cycles of 110 Hz and its harmonics in exactly one second, so the clip loops seamlessly.
    out = []
    for i in range(int(seconds * RATE)):
        t = i / RATE
        out.append(0.22 * math.sin(2 * math.pi * 110 * t) + 0.12 * math.sin(2 * math.pi * 220 * t) + 0.05 * math.sin(2 * math.pi * 330 * t))
    return out

def write(name, samples):
    os.makedirs(ROOT, exist_ok=True)
    with open(os.path.join(ROOT, name), "wb") as f:
        f.write(wav(samples))

if __name__ == "__main__":
    write("Chime.wav", chime())
    write("Clack.wav", clack())
    write("Hum.wav", hum())
