"""Export a trained RVC voice into an Engineer voice folder.

    python export_voice.py --rvc-dir C:\\RVC --pth C:\\RVC\\assets\\weights\\Dominic.pth \\
        --index C:\\RVC\\logs\\Dominic\\added_IVF..._v2.index --name Dominic

Writes Documents\\RainWorks\\Engineer\\Voices\\<name>\\ with:
    synth.onnx     the synthesiser (RVC's own ONNX export)
    features.bin   retrieval bank from the .index (at most 4096 vectors)
    voice.json     what the plugin reads

Run it with the Python environment that trained the model (RVC's runtime), so torch,
faiss and RVC's own code are importable. Nothing here is uploaded anywhere.
"""

import argparse
import json
import os
import struct
import sys
from pathlib import Path

MAX_BANK = 4096


def documents_dir() -> Path:
    if os.name == "nt":
        try:
            import ctypes.wintypes
            buf = ctypes.create_unicode_buffer(ctypes.wintypes.MAX_PATH)
            ctypes.windll.shell32.SHGetFolderPathW(None, 5, None, 0, buf)  # CSIDL_PERSONAL
            if buf.value:
                return Path(buf.value)
        except Exception:
            pass
    return Path.home() / "Documents"


def export_synth(rvc_dir: Path, pth: Path, out: Path) -> dict:
    import torch

    cpt = torch.load(str(pth), map_location="cpu")
    if int(cpt.get("f0", 1)) != 1:
        sys.exit("This model was trained without pitch guidance. Retrain with 'pitch guidance' on; Engineer needs it.")
    version = cpt.get("version", "v1")
    sample_rate = int(cpt["config"][-1])

    sys.path.insert(0, str(rvc_dir))
    os.chdir(rvc_dir)  # RVC resolves some paths relative to its root
    try:
        from infer.modules.onnx.export import export_onnx  # RVC-Project WebUI layout
    except ImportError as e:
        sys.exit(f"Could not import RVC's ONNX exporter from {rvc_dir}: {e}\n"
                 "Point --rvc-dir at the Retrieval-based-Voice-Conversion-WebUI folder.")
    export_onnx(str(pth), str(out))
    return {"format": "rvc-v2" if version == "v2" else "rvc-v1",
            "sampleRate": sample_rate,
            "featureDim": 768 if version == "v2" else 256}


def export_bank(index_path: Path, out: Path, dim: int) -> int:
    import faiss
    import numpy as np

    index = faiss.read_index(str(index_path))
    try:
        vecs = index.reconstruct_n(0, index.ntotal)
    except RuntimeError:
        ivf = faiss.extract_index_ivf(index)
        ivf.make_direct_map()
        vecs = ivf.reconstruct_n(0, ivf.ntotal)
    vecs = np.ascontiguousarray(vecs, dtype=np.float32)
    if vecs.shape[1] != dim:
        sys.exit(f"Index has {vecs.shape[1]}-dim features but the model expects {dim}. Wrong .index file?")
    if len(vecs) > MAX_BANK:
        km = faiss.Kmeans(dim, MAX_BANK, niter=25, seed=1, verbose=False)
        km.train(vecs)
        vecs = np.ascontiguousarray(km.centroids, dtype=np.float32)
    with open(out, "wb") as f:
        f.write(b"VEFB")
        f.write(struct.pack("<III", 1, len(vecs), dim))
        f.write(vecs.tobytes())
    return len(vecs)


def check(synth: Path, dim: int):
    """Runs the exported synthesiser once on dummy input, as the plugin will feed it."""
    import numpy as np
    import onnxruntime as ort

    sess = ort.InferenceSession(str(synth), providers=["CPUExecutionProvider"])
    names = {i.name for i in sess.get_inputs()}
    need = {"phone", "phone_lengths", "pitch", "pitchf", "ds", "rnd"}
    if not need <= names:
        sys.exit(f"Exported model is missing inputs {sorted(need - names)}")
    T = 200
    out = sess.run(None, {
        "phone": np.random.randn(1, T, dim).astype(np.float32),
        "phone_lengths": np.array([T], dtype=np.int64),
        "pitch": np.full((1, T), 60, dtype=np.int64),
        "pitchf": np.full((1, T), 220.0, dtype=np.float32),
        "ds": np.array([0], dtype=np.int64),
        "rnd": np.random.randn(1, 192, T).astype(np.float32),
    })[0]
    print(f"  check: 2 s of frames -> {out.size} samples")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rvc-dir", required=True, type=Path)
    ap.add_argument("--pth", required=True, type=Path, help="trained voice, assets/weights/<name>.pth")
    ap.add_argument("--index", type=Path, help="logs/<name>/added_*.index (optional but recommended)")
    ap.add_argument("--name", required=True, help="name shown in the plugin's Voice menu")
    ap.add_argument("--out", type=Path, help="defaults to Documents/RainWorks/Engineer/Voices/<name>")
    ap.add_argument("--speaker-id", type=int, default=0)
    ap.add_argument("--no-check", action="store_true")
    a = ap.parse_args()

    rvc_dir, pth = a.rvc_dir.resolve(), a.pth.resolve()
    index = a.index.resolve() if a.index else None
    folder_name = "".join(c if c.isalnum() or c in "-_ " else "_" for c in a.name).strip() or "voice"
    out = (a.out or documents_dir() / "RainWorks" / "Engineer" / "Voices" / folder_name).resolve()
    out.mkdir(parents=True, exist_ok=True)

    print(f"Exporting {pth.name} -> {out}")
    info = export_synth(rvc_dir, pth, out / "synth.onnx")
    manifest = {"name": a.name, "synth": "synth.onnx", "speakerId": a.speaker_id, **info}

    if index:
        n = export_bank(index, out / "features.bin", info["featureDim"])
        manifest["features"] = "features.bin"
        print(f"  retrieval bank: {n} vectors")

    (out / "voice.json").write_text(json.dumps(manifest, indent=2))
    if not a.no_check:
        check(out / "synth.onnx", info["featureDim"])
    print("Done. In the plugin: Voice menu > Rescan voices folder.")


if __name__ == "__main__":
    main()
