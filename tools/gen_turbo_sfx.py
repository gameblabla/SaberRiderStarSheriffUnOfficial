#!/usr/bin/env python3
"""Generate the Red Fury's turbo sounds (assets/sfx/turbo_start.wav, turbo_loop.wav): PCM16 mono 22050 Hz.
turbo_start: the afterburner lighting - a rising whine over a noise burst (0.55 s), played once when turbo is pressed.
turbo_loop:  the burner's roar while it is held - band-passed noise with a jet rumble, built periodic so it loops
             without a click (1.0 s). mode7.c keeps it queued while the aim button is down and the meter has fuel."""
import os, struct, math
import numpy as np

RATE = 22050
OUT = os.path.join(os.path.dirname(__file__), "..", "assets", "sfx")

def write_wav(path, x):
    x = np.clip(x, -1, 1); pcm = (x * 32767).astype("<i2").tobytes()
    with open(path, "wb") as f:
        f.write(b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVE")
        f.write(b"fmt " + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16))
        f.write(b"data" + struct.pack("<I", len(pcm)) + pcm)

def lowpass(x, cutoff):
    a = math.exp(-2 * math.pi * cutoff / RATE); y = np.zeros_like(x); s = 0.0
    for i in range(len(x)): s = a * s + (1 - a) * x[i]; y[i] = s
    return y

def start():
    n = int(RATE * 0.55); t = np.arange(n) / RATE
    rng = np.random.default_rng(3)
    # the whine: a saw sweeping 180 -> 900 Hz with a little vibrato
    f = 180 + 720 * (t / t[-1]) ** 0.6 + 12 * np.sin(2 * math.pi * 31 * t)
    ph = 2 * math.pi * np.cumsum(f) / RATE
    saw = 2 * ((ph / (2 * math.pi)) % 1.0) - 1
    saw = lowpass(saw, 2200)
    # the burst: noise shaped by a fast attack / slow decay
    noise = lowpass(rng.standard_normal(n), 3000) - lowpass(rng.standard_normal(n), 300)
    env_n = np.exp(-t * 7) * (1 - np.exp(-t * 200))
    env_w = np.minimum(1, t * 12) * (1 - np.exp(-(t[-1] - t) * 25))
    x = 0.55 * saw * env_w + 0.8 * noise * env_n
    x[-int(RATE * 0.03):] *= np.linspace(1, 0, int(RATE * 0.03))
    return x / max(1e-6, np.abs(x).max()) * 0.9

def loop():
    n = RATE; t = np.arange(n) / RATE
    rng = np.random.default_rng(5)
    # periodic by construction: noise synthesised from whole-cycle harmonics of 1 Hz, so sample n wraps to sample 0
    k = np.arange(1, 4000); amp = 1.0 / (1 + (k / 900.0) ** 2)   # band roll-off above ~900 Hz
    amp *= (k > 60)                                                 # nothing below 60 Hz (the rumble is added below)
    phases = rng.uniform(0, 2 * math.pi, len(k))
    x = np.zeros(n)
    for kk, a, p in zip(k, amp, phases): x += a * np.sin(2 * math.pi * kk * t + p)
    x /= np.abs(x).max()
    # the rumble: pulse train of the burner (73 Hz, an integer count of cycles in the second)
    rumble = np.sin(2 * math.pi * 73 * t) + 0.5 * np.sin(2 * math.pi * 146 * t + 0.4)
    # slow flutter, also whole cycles
    flutter = 1 + 0.18 * np.sin(2 * math.pi * 6 * t) + 0.08 * np.sin(2 * math.pi * 11 * t)
    y = (0.75 * x + 0.3 * rumble / 1.5) * flutter
    return y / np.abs(y).max() * 0.8

if __name__ == "__main__":
    os.makedirs(OUT, exist_ok=True)
    write_wav(os.path.join(OUT, "turbo_start.wav"), start())
    write_wav(os.path.join(OUT, "turbo_loop.wav"), loop())
    print("wrote", OUT)
