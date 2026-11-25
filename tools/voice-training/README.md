# Training a voice model for Engineer (RTX 3060 12 GB)

Engineer re-sings takes in a singer's own voice with a retrieval-based voice conversion
(RVC v2) model. You train one model per singer, once, then export it for the plugin.
Training runs on your PC; nothing is uploaded.

Expect about **1 to 2 hours** of training per model on an RTX 3060 12 GB with 20 minutes
of vocals.

## 1. Record the training vocals

- **10 to 30 minutes** of the singer alone. More than 30 minutes rarely helps.
- **Dry.** No reverb, delay, doubling, tuning or backing track. Light EQ is fine.
- **Clean.** A quiet room, no clipping, peaks around -6 dBFS.
- **Their whole range.** Low and high notes, soft and loud singing, a few styles. The model
  can only sing what it has heard.
- Export as WAV (44.1 or 48 kHz), in clips of any length. Cut out long silences and talking.
- **Consent.** Only train on your own voice, or on a singer who has agreed to it.

Takes recorded with the plugin are already dry WAVs in the session's `Regions` folder
(Data folder > Show in file browser), so you can use those.

## 2. Install RVC (one time)

1. Download the RVC WebUI for NVIDIA from the
   [Retrieval-based-Voice-Conversion-WebUI releases](https://github.com/RVC-Project/Retrieval-based-Voice-Conversion-WebUI/releases)
   (MIT licence) and unzip it, for example to `C:\RVC`.
2. Run its model download step if the release asks for it. You need
   `assets\hubert\hubert_base.pt`, `assets\rmvpe\rmvpe.pt` and the v2 pretrained models
   (`assets\pretrained_v2\f0G40k.pth`, `f0D40k.pth`).
3. Start it with `go-web.bat`. A page opens in the browser.

## 3. Train

On the **Train** tab:

| Setting | Value |
| --- | --- |
| Experiment name | the singer, for example `Dominic` |
| Target sample rate | 40k |
| Pitch guidance | **Yes** (Engineer needs it) |
| Version | v2 |
| Training folder | the folder with the WAVs from step 1 |
| Pitch extraction | rmvpe_gpu |
| Save frequency | 25 |
| Total epochs | 200 for 10-20 min of audio, 300 for less |
| Batch size per GPU | 8 (lower to 6 if you run out of memory) |
| Save only latest ckpt | Yes |
| Cache training set to GPU | No |

Then click, in order: **Process data**, **Feature extraction**, **Train model**, **Train
feature index**. When it's done you have:

- `assets\weights\Dominic.pth` (the voice)
- `logs\Dominic\added_IVF..._v2.index` (the retrieval index)

Try it on the WebUI's Model Inference tab before exporting. If it sounds metallic, train
longer; if it slurs words, use fewer epochs or more varied data.

## 4. Export for the plugin

Use RVC's own Python so torch, faiss and fairseq are available. From this folder:

```bat
C:\RVC\runtime\python.exe -m pip install onnx onnxruntime
C:\RVC\runtime\python.exe export_content_encoder.py --rvc-dir C:\RVC
C:\RVC\runtime\python.exe export_voice.py --rvc-dir C:\RVC ^
    --pth C:\RVC\assets\weights\Dominic.pth ^
    --index "C:\RVC\logs\Dominic\added_IVF256_Flat_nprobe_1_Dominic_v2.index" ^
    --name Dominic
```

- `export_content_encoder.py` runs once per PC. It writes
  `Documents\RainWorks\Engineer\Models\content-encoder.onnx`, which every voice shares.
- `export_voice.py` runs once per voice. It writes
  `Documents\RainWorks\Engineer\Voices\Dominic\` (`synth.onnx`, `features.bin`, `voice.json`)
  and runs the exported model once as a check.

## 5. Use it in FL Studio

1. In the plugin, open the **Voice** menu and pick **Rescan voices folder**.
2. Pick the voice. Every take re-renders in that voice, following the notes, style and
   timing. Rendering takes roughly 30 to 90 seconds per 3 minutes of vocal on a CPU.
3. **As recorded** goes back to the singer's own recording, with pitch and timing edits.

## voice.json

```json
{ "name": "Dominic", "format": "rvc-v2", "sampleRate": 40000,
  "synth": "synth.onnx", "features": "features.bin", "speakerId": 0, "featureDim": 768 }
```

Optional: `"contentEncoder": "my-encoder.onnx"` (relative to the voice folder) to use a
different encoder for this voice.

## Licences to check before shipping commercially

The RVC code is MIT. The HuBERT base weights come from fairseq (MIT). Check the licence of
the RVC pretrained models (`f0G40k` / `f0D40k`) and of any model you didn't train yourself
before you distribute it with the product.
