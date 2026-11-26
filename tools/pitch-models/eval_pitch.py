#!/usr/bin/env python3
"""Compare the plugin's pitch trackers on a dataset with reference f0 (vocadito by default).

    python3 eval_pitch.py --tool build/EngineerStyleLearn_artefacts/Release/EngineerStyleLearn \
        --audio vocadito/Audio --f0 vocadito/Annotations/F0 [--config "neural:--engine neural" ...]

vocadito (Bittner et al., CC BY 4.0): https://zenodo.org/records/5578807. Prints a table of
frame-weighted means over all files and writes per-file JSON lines with --out.
"""
import argparse, json, pathlib, shlex, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

DEFAULT_CONFIGS = [
    "dsp:--engine dsp",
    "neural:--engine neural",
    "neural+refine:--engine neural --refine 50",
]
KEYS = ["rawPitchAccuracy", "within25", "octaveErrors", "voicingRecall", "voicingFalseAlarm", "medianCents"]
NOTE_KEYS = ["notePrecision", "noteRecall", "noteF"]


def run(tool, args, audio, ref, notes):
    cmd = [tool] + shlex.split(args) + ["--pitch-eval", str(ref)] + (["--note-eval", str(notes)] if notes else []) + [str(audio)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        sys.exit(f"failed: {' '.join(cmd)}\n{p.stderr}")
    return json.loads(p.stdout.strip().splitlines()[-1])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--audio", required=True)
    ap.add_argument("--f0", required=True)
    ap.add_argument("--notes", help="folder of reference notes (<stem>_notesA1.csv), to score note segmentation too")
    ap.add_argument("--config", action="append", help="name:extra args (repeatable)")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--out")
    a = ap.parse_args()
    configs = [c.split(":", 1) for c in (a.config or DEFAULT_CONFIGS)]
    pairs = []
    for wav in sorted(pathlib.Path(a.audio).glob("*.wav")):
        ref = pathlib.Path(a.f0) / (wav.stem + "_f0.csv")
        notes = pathlib.Path(a.notes) / (wav.stem + "_notesA1.csv") if a.notes else None
        if ref.exists():
            pairs.append((wav, ref, notes if notes and notes.exists() else None))
    rows = []
    with ThreadPoolExecutor(a.jobs) as ex:
        for name, args in configs:
            res = list(ex.map(lambda p: run(a.tool, args, *p), pairs))
            for r in res:
                r["config"] = name
            rows += res
    if a.out:
        pathlib.Path(a.out).write_text("\n".join(json.dumps(r) for r in rows) + "\n")
    print(f"{len(pairs)} files")
    keys = KEYS + (NOTE_KEYS if a.notes else [])
    print("config".ljust(16) + "".join(k[:14].rjust(15) for k in keys))
    for name, _ in configs:
        rs = [r for r in rows if r["config"] == name]
        w = sum(r["voicedFrames"] for r in rs)
        line = name.ljust(16)
        for k in keys:
            if k in NOTE_KEYS:   # pooled over all notes
                if k == "noteF":
                    p, rc = float(line.split()[-2].rstrip("%")) / 100, float(line.split()[-1].rstrip("%")) / 100
                    v = 2 * p * rc / (p + rc) if p + rc else 0.0
                else:
                    matched = sum(r[k] * (r["notes"] if k == "notePrecision" else r["refNotes"]) for r in rs if k in r)
                    total = sum(r["notes"] if k == "notePrecision" else r["refNotes"] for r in rs if k in r)
                    v = matched / max(1, total)
            else:
                v = sum(r[k] * r["voicedFrames"] for r in rs) / w
            line += (f"{v:15.2f}" if "Cents" in k else f"{100 * v:14.1f}%")
        print(line)


if __name__ == "__main__":
    main()
