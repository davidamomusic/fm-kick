#!/usr/bin/env python3
# SPDX-License-Identifier: MPL-2.0
"""FM-KICK: the classic kinds keep their character. Each factory kick is hit once in the host simulator and measured:
the pitch (from the zero crossings) in windows after the hit, and which way the first cycle goes.
  808: down first; ~49 Hz, sighing a few Hz down over the first few hundred ms
  909: up first; from ~230 Hz through ~100-200 Hz in its first 20 ms down to ~55 Hz
  606: higher and shorter, settled ~70-85 Hz
    python3 kick/tests/kicks_check.py [path/to/fm-kick_host]"""
import os, subprocess, sys, tempfile, wave
import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
HOST = sys.argv[1] if len(sys.argv) > 1 else os.path.join(HERE, "../build/host/fm-kick_host")
FIRST_PATTERN_PRESET = 9                 # the factory pattern 1 starts on preset 9

def render(n, d):
    s = os.path.join(d, f"s{n}.txt")
    with open(s, "w") as f:
        f.write("wait 300\n")
        if n != FIRST_PATTERN_PRESET:
            f.write(f"spin PRESET {n - FIRST_PATTERN_PRESET}\n")
        f.write(f"expect preset {n}\nwait 300\nwav p{n}.wav\nmidi 90 24 64\nwait 900\nwavstop\n")
    subprocess.run([HOST, s, d], cwd=d, check=True, stdout=subprocess.DEVNULL)
    w = wave.open(os.path.join(d, f"p{n}.wav"))
    x = np.frombuffer(w.readframes(w.getnframes()), dtype=np.int16).astype(float).reshape(-1, w.getnchannels())[:, 0]
    x /= 32768.0
    return x[np.argmax(np.abs(x) > 1e-3):], w.getframerate()

def measure(x, fs):
    zc = np.where(np.diff(np.signbit(x)))[0]
    def f(t0, t1):
        z = zc[(zc > t0 * fs) & (zc < t1 * fs)]
        return (len(z) - 1) / 2 / ((z[-1] - z[0]) / fs) if len(z) > 2 else 0.0
    first = x[: int(0.004 * fs)]
    return first[np.argmax(np.abs(first))] > 0, f(0.005, 0.02), f(0.02, 0.06), f(0.15, 0.3), f(0.3, 0.6)

CHECKS = {   # preset: (up first, (lo, hi) 5-20 ms or None, (lo, hi) 150-300 ms, sigh: 20-60 ms above 300-600 ms by Hz)
    0: (False, None, (48, 52), 3.0),     # 808 LONG
    1: (False, None, (53, 58), 3.0),     # 808 PUNCH
    3: (True, (100, 200), (52, 58), None),   # 909 CLASS
    5: (True, (100, 200), (60, 70), None),   # 909 SHORT
    6: (True, None, (70, 85), None),     # 606 TIGHT
}
fails = 0
with tempfile.TemporaryDirectory() as d:
    for n, (up, early, late, sigh) in CHECKS.items():
        x, fs = render(n, d)
        u, f520, f2060, f150, f300 = measure(x, fs)
        ok = u == up and late[0] <= f150 <= late[1] and (early is None or early[0] <= f520 <= early[1]) and \
            (sigh is None or f2060 - f300 >= sigh)
        print(f"{'ok  ' if ok else 'FAIL'} preset {n}: {'up' if u else 'down'} first, 5-20 ms {f520:.0f} Hz, "
              f"20-60 ms {f2060:.1f} Hz, 150-300 ms {f150:.1f} Hz, 300-600 ms {f300:.1f} Hz")
        fails += not ok
print(f"{fails} failed" if fails else "all passed")
sys.exit(1 if fails else 0)
