#!/usr/bin/env python3
"""Generates the sound effects of the Marble Run, Breakout and First Person samples (deterministic, no dependencies).

Run:  python3 scripts/generate_sample_assets.py  (from any directory; paths are relative to this script)
"""
import math
import os
import struct

RATE = 22050
SAMPLES = os.path.join(os.path.dirname(__file__), "..", "Samples")

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

def bounce(seconds=0.08):
    # Breakout's paddle and walls: a bright blip that drops an octave as it fades.
    out, phase = [], 0.0
    for i in range(int(seconds * RATE)):
        t = i / RATE
        phase += 2 * math.pi * (880.0 - 3000.0 * t) / RATE
        out.append(min(1.0, t / 0.002) * math.exp(-35.0 * t) * 0.5 * math.sin(phase))
    return out

def brick(seconds=0.18):
    # A breaking brick: a short square-ish chirp up plus a burst of pseudo-random noise (fixed LCG).
    out, state, phase = [], 54321, 0.0
    for i in range(int(seconds * RATE)):
        t = i / RATE
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        noise = state / 0x3FFFFFFF - 1.0
        phase += 2 * math.pi * (520.0 + 2400.0 * t) / RATE
        tone = math.copysign(1.0, math.sin(phase)) * 0.18
        out.append(math.exp(-18.0 * t) * (tone + 0.3 * noise * math.exp(-30.0 * t)))
    return out

def lose(seconds=0.6):
    # A lost ball: a falling tone with a slow vibrato.
    out, phase = [], 0.0
    for i in range(int(seconds * RATE)):
        t = i / RATE
        phase += 2 * math.pi * (440.0 - 380.0 * t + 12.0 * math.sin(2 * math.pi * 7.0 * t)) / RATE
        out.append(min(1.0, t / 0.01) * (1.0 - t / seconds) * 0.4 * math.sin(phase))
    return out

def generator(seconds=1.0):
    # First Person's generator: a 60 Hz buzz with strong odd harmonics up to 2.1 kHz (whole cycles in one second, so it
    # loops seamlessly). The bright harmonics are what its low-pass occlusion takes away.
    out = []
    for i in range(int(seconds * RATE)):
        t = i / RATE
        value = sum(math.sin(2 * math.pi * 60 * k * t) / k for k in range(1, 36, 2))
        out.append(0.3 * value)
    return out

def drone(seconds=1.0):
    # First Person's drone: a 150 Hz rotor buzz (a band-limited sawtooth) beating at 10 Hz; whole cycles, so it loops.
    out = []
    for i in range(int(seconds * RATE)):
        t = i / RATE
        saw = sum(math.sin(2 * math.pi * 150 * k * t) / k for k in range(1, 13))
        out.append(0.22 * (0.75 + 0.25 * math.sin(2 * math.pi * 10 * t)) * saw)
    return out

def shot(seconds=0.35):
    # A shot: a noise crack over a falling low thump (fixed LCG noise).
    out, state, phase = [], 24680, 0.0
    for i in range(int(seconds * RATE)):
        t = i / RATE
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        noise = state / 0x3FFFFFFF - 1.0
        phase += 2 * math.pi * (160.0 - 220.0 * t) / RATE
        out.append(0.55 * noise * math.exp(-28.0 * t) + 0.45 * math.sin(phase) * math.exp(-9.0 * t))
    return out

def step(seconds=0.12):
    # A footstep: a dull knock with a little grit.
    out, state = [], 13579
    for i in range(int(seconds * RATE)):
        t = i / RATE
        state = (1103515245 * state + 12345) & 0x7FFFFFFF
        noise = state / 0x3FFFFFFF - 1.0
        out.append(math.exp(-40.0 * t) * (0.5 * math.sin(2 * math.pi * 95.0 * t) + 0.12 * noise))
    return out

def ding(seconds=0.5):
    # A target going down: a bright two-note ping.
    out = []
    for i in range(int(seconds * RATE)):
        t = i / RATE
        frequency = 1318.5 if t < 0.08 else 1760.0
        out.append(min(1.0, t / 0.003) * math.exp(-7.0 * t) * 0.35 * math.sin(2 * math.pi * frequency * t))
    return out

def write(sample, name, samples):
    directory = os.path.join(SAMPLES, sample, "Assets", "Audio")
    os.makedirs(directory, exist_ok=True)
    with open(os.path.join(directory, name), "wb") as f:
        f.write(wav(samples))

if __name__ == "__main__":
    write("MarbleRun", "Chime.wav", chime())
    write("MarbleRun", "Clack.wav", clack())
    write("MarbleRun", "Hum.wav", hum())
    write("Breakout", "Bounce.wav", bounce())
    write("Breakout", "Break.wav", brick())
    write("Breakout", "Lose.wav", lose())
    write("FirstPerson", "Generator.wav", generator())
    write("FirstPerson", "Drone.wav", drone())
    write("FirstPerson", "Shot.wav", shot())
    write("FirstPerson", "Step.wav", step())
    write("FirstPerson", "Ding.wav", ding())
