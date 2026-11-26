#!/usr/bin/env python3
"""Train the technique classifier on features from prepare.py and export it for the plugin.

    python3 train.py --features features.npz --out ../../Models/technique-classifier.onnx

Runs on a CPU in about 15 minutes (an RTX 3060 makes it a minute or two). Prints held-out
results per technique: singers/speakers in the validation split are never trained on.
"""
import argparse, json, pathlib, random, sys, time
import numpy as np
import torch
import torch.nn.functional as F
from model import KINDS, TechniqueNet, export

CROP = 150          # 1.5 s
LOG_FLOOR = float(np.log(1e-5))


def voiced_mask(m):
    """Frames within 40 dB of the file's loudest frame (silence carries no technique)."""
    e = m.mean(1)
    return e > (e.max() - 4.6)


def load(path):
    d = np.load(path)
    mel, off = d["mel"], d["offsets"]
    files = []
    for i in range(len(off) - 1):
        m = mel[off[i]:off[i + 1]].astype(np.float32)
        files.append(dict(mel=m, mask=voiced_mask(m), labels=d["labels"][i], group=str(d["groups"][i]), split=str(d["splits"][i])))
    return files


def augment(m):
    m = m + random.uniform(-2.5, 1.0)                                   # gain
    m = m + np.linspace(-1, 1, m.shape[1])[None, :] * random.uniform(-0.8, 0.8)  # spectral tilt (mic/EQ)
    if random.random() < 0.5:                                           # noise floor
        floor = random.uniform(-9.0, -5.0)
        m = np.logaddexp(m, floor + 0.3 * np.random.randn(*m.shape))
    if random.random() < 0.3:                                           # band mask
        f0 = random.randint(0, 110)
        m[:, f0:f0 + random.randint(4, 16)] = m.min()
    return np.maximum(m, LOG_FLOOR).astype(np.float32)


def batch(files, by_group, groups, n):
    xs, ys, ms = [], [], []
    for _ in range(n):
        f = random.choice(by_group[random.choice(groups)])
        T = len(f["mel"])
        if T < CROP:
            s, m, mk = 0, np.pad(f["mel"], ((0, CROP - T), (0, 0)), constant_values=LOG_FLOOR), np.pad(f["mask"], (0, CROP - T))
        else:
            s = random.randint(0, T - CROP)
            m, mk = f["mel"][s:s + CROP], f["mask"][s:s + CROP]
        xs.append(augment(m.copy()))
        ys.append(f["labels"])
        ms.append(mk)
    return torch.tensor(np.stack(xs)), torch.tensor(np.stack(ys), dtype=torch.float32), torch.tensor(np.stack(ms), dtype=torch.float32)


def loss_fn(logits, y, frame_mask, pos_weight):
    # logits [B, T, K]; y [B, K] with -1 = unknown; frame_mask [B, T]
    known = (y >= 0).float()[:, None, :] * frame_mask[:, :, None]
    target = y.clamp(min=0)[:, None, :].expand_as(logits)
    l = F.binary_cross_entropy_with_logits(logits, target, reduction="none", pos_weight=pos_weight)
    return (l * known).sum() / known.sum().clamp(min=1)


def evaluate(net, files):
    net.eval()
    out = {k: dict(tp=0, fp=0, tn=0, fn=0, scores=[], ys=[]) for k in KINDS}
    with torch.no_grad():
        for f in files:
            p = net(torch.tensor(f["mel"])[None])[0].numpy()
            mk = f["mask"]
            if mk.sum() == 0:
                continue
            clip = p[mk].mean(0)
            for k, name in enumerate(KINDS):
                y = f["labels"][k]
                if y < 0:
                    continue
                o = out[name]
                o["scores"].append(float(clip[k])); o["ys"].append(int(y))
                pred = clip[k] >= 0.5
                o["tp" if (pred and y) else "fp" if pred else "fn" if y else "tn"] += 1
    res = {}
    for name, o in out.items():
        ys, sc = np.array(o["ys"]), np.array(o["scores"])
        pos, neg = sc[ys == 1], sc[ys == 0]
        auc = float((pos[:, None] > neg[None, :]).mean() + 0.5 * (pos[:, None] == neg[None, :]).mean()) if len(pos) and len(neg) else float("nan")
        n = o["tp"] + o["fp"] + o["tn"] + o["fn"]
        res[name] = dict(clips=n, positives=int(ys.sum()), accuracy=(o["tp"] + o["tn"]) / max(1, n),
                         precision=o["tp"] / max(1, o["tp"] + o["fp"]), recall=o["tp"] / max(1, o["tp"] + o["fn"]), auc=auc)
    net.train()
    return res


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--features", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--steps", type=int, default=3000)
    ap.add_argument("--batch", type=int, default=32)
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--report", help="write held-out results here (JSON)")
    a = ap.parse_args()
    random.seed(a.seed); np.random.seed(a.seed); torch.manual_seed(a.seed)
    torch.set_num_threads(max(1, torch.get_num_threads()))

    files = load(a.features)
    train = [f for f in files if f["split"] == "train"]
    val = [f for f in files if f["split"] == "val"]
    by_group = {}
    for f in train:
        by_group.setdefault(f["group"], []).append(f)
    # Sample techniques evenly, with extra weight on the rarer, important classes.
    groups = []
    for g in by_group:
        w = 3 if g in ("ursing", "vocalset:spoken", "arctic", "emvd:grit", "vocalset:vocal_fry", "vocalset:breathy", "vocalset:belt", "vocalset:vibrato", "vocalset:straight") else 1
        groups += [g] * w
    print(f"{len(train)} train files, {len(val)} held-out files, groups: {sorted(by_group)}", file=sys.stderr)

    net = TechniqueNet()
    opt = torch.optim.AdamW(net.parameters(), lr=2e-3, weight_decay=1e-3)
    sched = torch.optim.lr_scheduler.OneCycleLR(opt, max_lr=2e-3, total_steps=a.steps, pct_start=0.1)
    pos_weight = torch.ones(len(KINDS))
    t0 = time.time()
    for step in range(1, a.steps + 1):
        x, y, mk = batch(train, by_group, groups, a.batch)
        loss = loss_fn(net.logits(x), y, mk, pos_weight)
        opt.zero_grad(); loss.backward(); opt.step(); sched.step()
        if step % 250 == 0:
            print(f"step {step} loss {loss.item():.3f} ({time.time() - t0:.0f} s)", file=sys.stderr)
        if step % 1000 == 0 or step == a.steps:
            r = evaluate(net, val)
            print(" ".join(f"{k}: acc {v['accuracy']:.2f} auc {v['auc']:.2f}" for k, v in r.items()), file=sys.stderr)

    res = evaluate(net, val)
    print(json.dumps(res, indent=1))
    if a.report:
        pathlib.Path(a.report).write_text(json.dumps(res, indent=1))
    export(net, a.out)
    print(f"wrote {a.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
