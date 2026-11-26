#!/usr/bin/env python3
"""Export the FCPE pitch model (torchfcpe, MIT licence) to ONNX for the plugin.

    pip install torch torchaudio torchfcpe==0.0.4 onnx
    python3 export_fcpe.py --out pitch-fcpe.onnx

Then either put pitch-fcpe.onnx in Documents/RainWorks/Engineer/Models/, or next to the plugin
binary inside the .vst3 bundle (Contents/x86_64-win/), where the Windows CI build already puts it.

The exported graph is the network only: log-mel [1, frames, 128] in, salience [1, frames, 360]
out (sigmoid). The plugin computes the mel itself (Source/Engine/ML/MelSpectrogram.cpp, which
matches torchfcpe's Wav2Mel) and decodes pitch with FCPE's local weighted average. The
check at the end compares ONNX Runtime with PyTorch on random input, at two lengths.

Model: CNChTu/FCPE ("Fast Context-based Pitch Estimation"), bundled weights fcpe_c_v001.pt,
MIT licence: https://github.com/CNChTu/FCPE
"""
import argparse
import numpy as np
import torch
import torchfcpe


class Network(torch.nn.Module):
    def __init__(self, net):
        super().__init__()
        self.net = net

    def forward(self, mel):
        return self.net(mel)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default="pitch-fcpe.onnx")
    a = ap.parse_args()
    model = torchfcpe.spawn_bundled_infer_model("cpu")
    net = Network(model.model.eval()).eval()
    mel = torch.randn(1, 300, 128)
    export_kwargs = dict(input_names=["mel"], output_names=["salience"],
                         dynamic_axes={"mel": {1: "frames"}, "salience": {1: "frames"}}, opset_version=17)
    try:
        torch.onnx.export(net, (mel,), a.out, dynamo=False, **export_kwargs)
    except TypeError:   # older torch without the dynamo switch
        torch.onnx.export(net, (mel,), a.out, **export_kwargs)

    try:
        import onnxruntime as ort
    except ImportError:
        print(f"wrote {a.out} (onnxruntime not installed, skipped the check)")
        return
    s = ort.InferenceSession(a.out, providers=["CPUExecutionProvider"])
    with torch.no_grad():
        for frames in (300, 1234):
            x = torch.randn(1, frames, 128) * 2 - 5
            err = np.abs(net(x).numpy() - s.run(None, {"mel": x.numpy()})[0]).max()
            assert err < 1e-4, f"ONNX differs from PyTorch by {err}"
    print(f"wrote {a.out}, matches PyTorch")


if __name__ == "__main__":
    main()
