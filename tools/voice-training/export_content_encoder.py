"""Export the content encoder (HuBERT base, as used by RVC) to ONNX, once per machine.

    python export_content_encoder.py --rvc-dir C:\\RVC

Reads <rvc-dir>/assets/hubert/hubert_base.pt and writes
Documents\\RainWorks\\Engineer\\Models\\content-encoder.onnx (about 360 MB).
Input "source" float32 [1, 1, samples] at 16 kHz; output [1, frames, 768] (v2 voices:
layer 12) at 50 frames per second. Use --v1 for old 256-dim voices (layer 9 + projection).
Run with RVC's Python environment (it has fairseq).
"""

import argparse
import sys
from pathlib import Path

from export_voice import documents_dir


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--rvc-dir", required=True, type=Path)
    ap.add_argument("--out", type=Path)
    ap.add_argument("--v1", action="store_true", help="256-dim features for RVC v1 voices")
    a = ap.parse_args()

    import torch
    try:
        from fairseq import checkpoint_utils
    except ImportError:
        sys.exit("fairseq is not importable. Run this with RVC's Python environment (runtime\\python.exe).")

    ckpt = a.rvc_dir / "assets" / "hubert" / "hubert_base.pt"
    if not ckpt.exists():
        sys.exit(f"Missing {ckpt}. Run RVC's model download step first.")
    models, _, _ = checkpoint_utils.load_model_ensemble_and_task([str(ckpt)], suffix="")
    hubert = models[0].eval().float()

    class Encoder(torch.nn.Module):
        def __init__(self, m, v1):
            super().__init__()
            self.m, self.v1 = m, v1

        def forward(self, source):  # [1, 1, N]
            wav = source.squeeze(1)
            pad = torch.zeros_like(wav, dtype=torch.bool)
            feats = self.m.extract_features(source=wav, padding_mask=pad, output_layer=9 if self.v1 else 12)[0]
            return self.m.final_proj(feats) if self.v1 else feats

    out = (a.out or documents_dir() / "RainWorks" / "Engineer" / "Models" / "content-encoder.onnx").resolve()
    out.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.randn(1, 1, 16000)
    torch.onnx.export(Encoder(hubert, a.v1), (dummy,), str(out), opset_version=17,
                      input_names=["source"], output_names=["embed"],
                      dynamic_axes={"source": {2: "samples"}, "embed": {1: "frames"}})

    import onnxruntime as ort
    sess = ort.InferenceSession(str(out), providers=["CPUExecutionProvider"])
    y = sess.run(None, {"source": dummy.numpy()})[0]
    print(f"Wrote {out}  (1 s of audio -> {y.shape[1]} frames x {y.shape[2]})")


if __name__ == "__main__":
    main()
