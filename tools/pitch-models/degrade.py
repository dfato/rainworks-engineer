#!/usr/bin/env python3
"""Make harder copies of a folder of vocals for the tracker comparison: room reverb, a noise
floor, or both. Same timing as the originals, so the reference f0 files still apply.

    python3 degrade.py --in vocadito/Audio --out vocadito-reverb --reverb 1.0
    python3 degrade.py --in vocadito/Audio --out vocadito-noise --snr 15
"""
import argparse, pathlib
import numpy as np
import soundfile as sf


def pink(n, rng):
    x = np.fft.rfft(rng.standard_normal(n))
    f = np.arange(len(x)); f[0] = 1
    return np.fft.irfft(x / np.sqrt(f), n)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--in", dest="src", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--reverb", type=float, default=0.0, help="RT60 in seconds (0 = none)")
    ap.add_argument("--wet", type=float, default=0.5, help="reverb level relative to the dry signal's RMS")
    ap.add_argument("--snr", type=float, default=None, help="pink noise at this SNR in dB")
    a = ap.parse_args()
    out = pathlib.Path(a.out); out.mkdir(parents=True, exist_ok=True)
    rng = np.random.default_rng(7)
    for wav in sorted(pathlib.Path(a.src).glob("*.wav")):
        y, sr = sf.read(str(wav), dtype="float64", always_2d=True)
        y = y.mean(1)
        z = y.copy()
        if a.reverb > 0:
            n = int(sr * a.reverb * 1.2)
            t = np.arange(n) / sr
            ir = rng.standard_normal(n) * np.exp(-6.9 * t / a.reverb)
            ir[: int(0.01 * sr)] *= np.linspace(0, 1, int(0.01 * sr))   # 10 ms pre-delay ramp
            m = 1 << int(np.ceil(np.log2(len(y) + n)))
            wet = np.fft.irfft(np.fft.rfft(y, m) * np.fft.rfft(ir, m), m)[: len(y)]
            wet *= a.wet * np.sqrt(np.mean(y ** 2)) / max(1e-12, np.sqrt(np.mean(wet ** 2)))
            z = z + wet
        if a.snr is not None:
            noise = pink(len(z), rng)
            noise *= np.sqrt(np.mean(y ** 2)) / max(1e-12, np.sqrt(np.mean(noise ** 2))) * 10 ** (-a.snr / 20)
            z = z + noise
        z *= 0.9 / max(1e-9, np.abs(z).max())
        sf.write(str(out / wav.name), z.astype(np.float32), sr, subtype="PCM_16")


if __name__ == "__main__":
    main()
