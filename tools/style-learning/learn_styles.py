#!/usr/bin/env python3
"""Tune Engineer's built-in genre styles from openly licensed singing.

    python learn_styles.py fetch   --work DIR
    python learn_styles.py analyze --work DIR --tool path/to/EngineerStyleLearn
    python learn_styles.py learn   --work DIR --tool path/to/EngineerStyleLearn [--apply]

fetch    reads sources.json and pulls only the needed vocal files out of each
         dataset's zip on Zenodo (HTTP range requests, so multi-GB zips are not
         downloaded whole), converts them to mono 44.1 kHz WAV in DIR/audio.
analyze  runs EngineerStyleLearn on each file and keeps DIR/stats/<id>.stats.json.
learn    combines the statistics per genre, writes learned/<genre>.json and
         manifest.json, and with --apply rewrites Source/Engine/Style/BuiltinStyles.cpp.

Audio stays in DIR (outside the repo). Only statistics and profiles are committed.
Needs Python 3.9+ and ffmpeg. No third-party packages.
"""

import argparse
import io
import json
import re
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent.parent
BUILTINS = REPO / "Source" / "Engine" / "Style" / "BuiltinStyles.cpp"

# A take needs at least this much singing to count.
MIN_NOTES = 30
MIN_VOICED_SECONDS = 20.0


class HttpRangeFile(io.RawIOBase):
    """Seekable read-only view of a remote file, fetched with HTTP Range requests."""

    def __init__(self, url):
        self.url, self.pos = url, 0
        with urllib.request.urlopen(urllib.request.Request(url, method="HEAD")) as r:
            self.size = int(r.headers["Content-Length"])

    def seekable(self): return True
    def readable(self): return True
    def tell(self): return self.pos

    def seek(self, off, whence=0):
        self.pos = off if whence == 0 else self.pos + off if whence == 1 else self.size + off
        return self.pos

    def readinto(self, b):
        n = min(len(b), self.size - self.pos)
        if n <= 0:
            return 0
        req = urllib.request.Request(self.url, headers={"Range": f"bytes={self.pos}-{self.pos + n - 1}"})
        for attempt in range(5):
            try:
                with urllib.request.urlopen(req, timeout=120) as r:
                    data = r.read()
                break
            except Exception:
                if attempt == 4:
                    raise
        b[:len(data)] = data
        self.pos += len(data)
        return len(data)


def load_sources():
    return json.loads((HERE / "sources.json").read_text(encoding="utf-8"))


def select_items(ds, names):
    """Items to take from a dataset's zip: listed explicitly, or matched by regex."""
    if "items" in ds:
        present = set(names)
        return [dict(it) for it in ds["items"] if it["member"] in present]
    m = ds["match"]
    rx = re.compile(m["regex"])
    seen, out = set(), []
    for name in sorted(names):
        g = rx.match(name)
        if not g:
            continue
        key = tuple(g.group(k) for k in m["onePer"])
        if m["onePer"] and key in seen:
            continue
        seen.add(key)
        stem = Path(name).stem if not m["onePer"] else "-".join(key)
        title = g.groupdict().get("song", Path(name).stem)
        out.append({"member": name, "id": f"{ds['id']}-{re.sub(r'[^A-Za-z0-9]+', '-', stem).strip('-')}",
                    "title": title, "genre": m["genre"]})
    return out


def cmd_fetch(args):
    work = Path(args.work)
    (work / "audio").mkdir(parents=True, exist_ok=True)
    raw = work / "raw"
    raw.mkdir(exist_ok=True)
    index_path = work / "items.json"
    index = json.loads(index_path.read_text(encoding="utf-8")) if index_path.exists() else []
    known = {it["id"] for it in index}
    for ds in load_sources()["datasets"]:
        if args.only and ds["id"] not in args.only:
            continue
        z = zipfile.ZipFile(io.BufferedReader(HttpRangeFile(ds["zip"]), buffer_size=4 << 20))
        names = [n for n in z.namelist() if not n.startswith("__MACOSX/")]
        items = select_items(ds, names)
        print(f"{ds['id']}: {len(items)} files", file=sys.stderr)
        for it in items:
            if it.get("genre") is None:
                continue
            out = work / "audio" / f"{it['id']}.wav"
            if not out.exists():
                tmp = raw / f"{it['id']}{Path(it['member']).suffix}"
                with z.open(it["member"]) as src, open(tmp, "wb") as dst:
                    shutil.copyfileobj(src, dst, 4 << 20)
                subprocess.run(["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", str(tmp),
                                "-ac", "1", "-ar", "44100", "-sample_fmt", "s16", str(out)], check=True)
                tmp.unlink()
                print(f"  {it['id']}", file=sys.stderr)
            if it["id"] not in known:
                index.append({**it, "dataset": ds["id"]})
    index_path.write_text(json.dumps(index, ensure_ascii=False, indent=1), encoding="utf-8")


def cmd_analyze(args):
    work = Path(args.work)
    stats = work / "stats"
    stats.mkdir(exist_ok=True)
    for it in json.loads((work / "items.json").read_text(encoding="utf-8")):
        out = stats / f"{it['id']}.stats.json"
        if out.exists():
            continue
        wav = work / "audio" / f"{it['id']}.wav"
        subprocess.run([args.tool, "--stats-dir", str(stats), "--stats-only", str(wav)],
                       check=True, stdout=subprocess.DEVNULL)


def builtin_entries():
    text = BUILTINS.read_text(encoding="utf-8")
    entries = {}
    for m in re.finditer(r'R"VE\((\{.*?\})\)VE"', text, re.S):
        entries[json.loads(m.group(1))["id"]] = m.group(1)
    return text, entries


def resolve(raw, gid, depth=0):
    """Profile with inheritance applied (child values win, sections merge), like StyleLibrary."""
    prof = raw[gid]
    parent = prof.get("inherits")
    if not parent or parent not in raw or depth > 16:
        return json.loads(json.dumps(prof))
    out = resolve(raw, parent, depth + 1)
    for k, v in prof.items():
        if isinstance(v, dict) and isinstance(out.get(k), dict):
            out[k] = {**out[k], **v}
        else:
            out[k] = v
    return out


def cmd_learn(args):
    work = Path(args.work)
    sources = {d["id"]: d for d in load_sources()["datasets"]}
    items = json.loads((work / "items.json").read_text(encoding="utf-8"))
    text, builtins = builtin_entries()

    by_genre, manifest = {}, []
    for it in items:
        st_path = work / "stats" / f"{it['id']}.stats.json"
        st = json.loads(st_path.read_text())
        ds = sources[it["dataset"]]
        used = st["notes"] >= MIN_NOTES and st["voicedSeconds"] >= MIN_VOICED_SECONDS
        reason = "" if used else "too little singing detected"
        if used and it["genre"] not in builtins:
            used, reason = False, "reference only" if it["genre"] == "reference" else "no matching style"
        manifest.append({
            "id": it["id"], "title": it["title"], "dataset": ds["title"], "page": ds["page"],
            "file": it["member"], "license": ds["license"], "licenseUrl": ds["licenseUrl"],
            "authors": ds["authors"], "genre": it["genre"], "genreSource": ds["genreSource"],
            "used": used, **({"skipped": reason} if reason else {}), "stats": st,
        })
        if used:
            by_genre.setdefault(it["genre"], []).append(st_path)

    learned_dir = HERE / "learned"
    learned_dir.mkdir(exist_ok=True)
    for old in learned_dir.glob("*.json"):
        old.unlink()

    coverage = {}
    for gid in sorted(builtins):
        files = by_genre.get(gid, [])
        coverage[gid] = len(files)
        if not files:
            continue
        base = json.loads(builtins[gid])
        out = learned_dir / f"{gid}.json"
        # Learn from the style's own current values, so evidence moves it from where it is.
        subprocess.run([args.tool, "--base", gid, "--id", gid, "--name", base["name"],
                        "--category", base["category"], "--strength", str(args.strength),
                        "--out", str(out)] + [str(f) for f in files], check=True)
        prof = json.loads(out.read_text())
        prof["inherits"] = base.get("inherits", "")
        if not prof["inherits"]:
            del prof["inherits"]
        n = len(files)
        prof["description"] = (base.get("description", "").rstrip(".") +
                               f". Tuned from {n} openly licensed reference vocal{'s' if n != 1 else ''}.")
        out.write_text(json.dumps(prof, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")

    (HERE / "manifest.json").write_text(json.dumps({
        "_comment": "Every vocal analysed for the built-in styles, its licence and the statistics measured. "
                    "No audio is stored in this repository.",
        "coverage": coverage, "items": manifest}, ensure_ascii=False, indent=1) + "\n", encoding="utf-8")

    if args.apply:
        order = ["id", "name", "category", "inherits", "description", "pitch", "vibrato", "slides", "timing", "learned"]
        old_raw = {gid: json.loads(j) for gid, j in builtins.items()}
        new_raw = dict(old_raw)
        for gid in coverage:
            p = learned_dir / f"{gid}.json"
            if p.exists():
                prof = json.loads(p.read_text(encoding="utf-8"))
                new_raw[gid] = {k: prof[k] for k in order if k in prof}

        # Styles without evidence must sound exactly as before, even when a parent they
        # inherit from was re-tuned: pin any value that would otherwise change.
        before = {gid: resolve(old_raw, gid) for gid in old_raw}
        for gid in old_raw:
            if (learned_dir / f"{gid}.json").exists():
                continue
            after = resolve(new_raw, gid)
            pinned = json.loads(json.dumps(old_raw[gid]))
            for sec in ("pitch", "vibrato", "slides", "timing"):
                for key, val in before[gid].get(sec, {}).items():
                    if after.get(sec, {}).get(key) != val:
                        pinned.setdefault(sec, {})[key] = val
            new_raw[gid] = {k: pinned[k] for k in order if k in pinned}

        for gid, prof in new_raw.items():
            if prof != old_raw[gid]:
                text = text.replace(f'R"VE({builtins[gid]})VE"', f'R"VE({json.dumps(prof, ensure_ascii=False)})VE"')
        BUILTINS.write_text(text, encoding="utf-8")

    for gid, n in coverage.items():
        print(f"{gid:18s} {n}")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    f = sub.add_parser("fetch"); f.add_argument("--work", required=True); f.add_argument("--only", nargs="*")
    a = sub.add_parser("analyze"); a.add_argument("--work", required=True); a.add_argument("--tool", required=True)
    l = sub.add_parser("learn"); l.add_argument("--work", required=True); l.add_argument("--tool", required=True)
    l.add_argument("--strength", type=float, default=1.0); l.add_argument("--apply", action="store_true")
    args = ap.parse_args()
    {"fetch": cmd_fetch, "analyze": cmd_analyze, "learn": cmd_learn}[args.cmd](args)


if __name__ == "__main__":
    main()
