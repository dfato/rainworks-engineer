"""Technique classifier network (shared by train.py and export)."""
import torch
import torch.nn as nn

KINDS = ["vibrato", "breathy", "belt", "grit", "spoken"]


class TechniqueNet(nn.Module):
    """log-mel [B, T, 128] (torchfcpe Wav2Mel, 16 kHz, 10 ms hop) -> probabilities [B, T, 5]."""

    def __init__(self, mels=128, width=128):
        super().__init__()
        chans = [1, 8, 16, 32, 32]
        layers = []
        for a, b in zip(chans[:-1], chans[1:]):
            layers += [nn.Conv2d(a, b, 3, padding=1), nn.BatchNorm2d(b), nn.ReLU(), nn.MaxPool2d((2, 1))]
        self.front = nn.Sequential(*layers)
        self.proj = nn.Conv1d(chans[-1] * (mels // 16), width, 1)
        self.blocks = nn.ModuleList()
        for d in (1, 2, 4, 8, 16):
            self.blocks.append(nn.Sequential(nn.Conv1d(width, width, 3, padding=d, dilation=d),
                                             nn.BatchNorm1d(width), nn.ReLU()))
        self.head = nn.Conv1d(width, len(KINDS), 1)

    def logits(self, mel):
        x = ((mel + 5.0) / 3.0).transpose(1, 2).unsqueeze(1)    # [B, 1, 128, T]
        x = self.front(x)                                        # [B, 64, 8, T]
        b, c, f, t = x.shape
        x = self.proj(x.reshape(b, c * f, t))
        for blk in self.blocks:
            x = x + blk(x)
        return self.head(x).transpose(1, 2)                      # [B, T, 5]

    def forward(self, mel):
        return torch.sigmoid(self.logits(mel))


def export(net, path):
    net.eval()
    mel = torch.randn(1, 400, 128)
    torch.onnx.export(net, (mel,), path, input_names=["mel"], output_names=["probs"],
                      dynamic_axes={"mel": {1: "frames"}, "probs": {1: "frames"}}, opset_version=17, dynamo=False)
