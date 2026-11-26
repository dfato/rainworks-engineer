#!/usr/bin/env python3
"""Compute log-mel features and technique labels for training the technique classifier.

    python3 prepare.py --vocalset VocalSet/FULL --emvd emvd/audio --emvd-meta metadata_files.csv \
        --arctic cmu_arctic_concat15 --ursing ursing --out features.npz

Data (all allow commercial use of a trained model; none of the audio is committed or shipped):
  VocalSet 1.1       Wilkins et al., CC BY 4.0     https://zenodo.org/records/1442513
  EMVD               Extreme Metal Vocals Dataset, CC BY 4.0  https://zenodo.org/records/17813678
  CMU ARCTIC (15 s)  CC0                           https://zenodo.org/records/3066489
  URSing             Li, Wang, Duan, CC BY 4.0     https://zenodo.org/records/6404999
                     (the Vocal.wav of each song, as fetched for tools/style-learning)

URSing matters: it is the only source of pop songs sung with lyrics. Without it, sung words look
more like speech than VocalSet's vowels and scales do, and sung lines get called "spoken".

Labels per file, for [vibrato, breathy, belt, grit, spoken]: 1, 0 or None (unknown, not trained).
The features are torchfcpe's Wav2Mel (16 kHz, 128 bands, 10 ms), exactly what the plugin computes.
"""
import argparse, csv, pathlib, sys
import numpy as np
import soundfile as sf
import torch
import torchfcpe

N = None
# VocalSet technique folder -> labels. Classical singers use vibrato on most techniques, so
# vibrato is only known for "vibrato" and "straight"; belt is only known where it is ruled out.
VOCALSET = {
    "vibrato":    (1, 0, N, 0, 0),
    "vibrado":    (1, 0, N, 0, 0),
    "straight":   (0, 0, N, 0, 0),
    "breathy":    (N, 1, 0, 0, 0),
    "belt":       (N, 0, 1, 0, 0),
    "vocal_fry":  (N, 0, 0, 1, 0),
    "spoken":     (0, N, 0, 0, 1),
    "fast_forte": (N, 0, N, 0, 0),
    "slow_forte": (N, 0, N, 0, 0),
    "forte":      (N, 0, N, 0, 0),
    "fast_piano": (N, N, 0, 0, 0),
    "slow_piano": (N, N, 0, 0, 0),
    "pp":         (N, N, 0, 0, 0),
    "messa":      (N, N, N, 0, 0),
    "trill":      (N, N, N, 0, 0),
    "trillo":     (N, N, N, 0, 0),
}
EMVD_CLEAN = (N, N, N, 0, 0)
EMVD_GRIT = (N, 0, 0, 1, 0)
SPEECH = (0, N, 0, 0, 1)
SUNG_LYRICS = (N, N, N, N, 0)

HELD_OUT_VOCALSET = {"female8", "female9", "male10", "male11"}
HELD_OUT_EMVD = {"24", "25", "26", "27"}
HELD_OUT_ARCTIC = {"slt", "bdl"}
HELD_OUT_URSING_EVERY = 6   # every 6th song


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--vocalset", required=True)
    ap.add_argument("--emvd", required=True)
    ap.add_argument("--emvd-meta", required=True)
    ap.add_argument("--arctic", required=True)
    ap.add_argument("--ursing", help="folder of URSing Vocal.wav files")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()

    items = []  # (path, labels, group, split)
    for wav in sorted(pathlib.Path(a.vocalset).glob("*/*/*/[!.]*.wav")):
        singer, context, tech = wav.parts[-4], wav.parts[-3], wav.parts[-2]
        if tech not in VOCALSET:
            continue
        items.append((wav, VOCALSET[tech], "vocalset:" + tech, "val" if singer in HELD_OUT_VOCALSET else "train"))
    for row in csv.DictReader(open(a.emvd_meta, encoding="utf-8")):
        if row["type"] == "Other" or row["name"] in ("-", "Effect"):
            continue
        wav = pathlib.Path(a.emvd) / row["file_name"]
        if not wav.exists():
            continue
        lab = EMVD_CLEAN if row["name"] == "ClearVoice" else EMVD_GRIT
        items.append((wav, lab, "emvd:" + ("clean" if lab is EMVD_CLEAN else "grit"),
                      "val" if row["singer_id"] in HELD_OUT_EMVD else "train"))
    for wav in sorted(pathlib.Path(a.arctic).glob("cmu_arctic_*.wav")):
        spk = wav.stem.split("_")[3]
        items.append((wav, SPEECH, "arctic", "val" if spk in HELD_OUT_ARCTIC else "train"))

    if a.ursing:
        for k, wav in enumerate(sorted(pathlib.Path(a.ursing).glob("*.wav"))):
            items.append((wav, SUNG_LYRICS, "ursing", "val" if k % HELD_OUT_URSING_EVERY == 0 else "train"))

    wav2mel = torchfcpe.spawn_bundled_infer_model("cpu").wav2mel
    mels, labels, groups, splits, offsets = [], [], [], [], [0]
    for i, (wav, lab, group, split) in enumerate(items):
        y, sr = sf.read(str(wav), dtype="float32", always_2d=True)
        y = y.mean(1)
        peak = np.abs(y).max()
        if peak > 0.99:
            y = y * (0.99 / peak)
        with torch.no_grad():
            m = wav2mel(torch.from_numpy(y)[None, :, None], sr)[0].numpy()
        mels.append(m.astype(np.float16))
        labels.append([-1 if v is None else v for v in lab])
        groups.append(group)
        splits.append(split)
        offsets.append(offsets[-1] + len(m))
        if i % 200 == 0:
            print(f"{i}/{len(items)} {wav.name}", file=sys.stderr)
    np.savez(a.out, mel=np.concatenate(mels), offsets=np.array(offsets), labels=np.array(labels, dtype=np.int8),
             groups=np.array(groups), splits=np.array(splits))
    print(f"{len(items)} files, {offsets[-1] / 360000:.1f} h", file=sys.stderr)


if __name__ == "__main__":
    main()
