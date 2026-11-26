# Pitch models

- `export_fcpe.py`: exports FCPE (torchfcpe 0.0.4, MIT) to `pitch-fcpe.onnx`. The Windows CI build runs it
  and puts the file in the VST3 bundle. To install by hand, copy it to `Documents\RainWorks\Engineer\Models`.
- `eval_pitch.py`: compares `EngineerStyleLearn --engine dsp|neural` against reference f0 (and notes with
  `--notes`) on vocadito (CC BY 4.0, https://zenodo.org/records/5578807).
- `degrade.py`: reverb / noise copies of a folder, for the harder comparisons.

    python3 degrade.py --in vocadito/Audio --out vocadito-reverb --reverb 1.0 --wet 0.6
    python3 eval_pitch.py --tool build/EngineerStyleLearn_artefacts/Release/EngineerStyleLearn \
        --audio vocadito-reverb --f0 vocadito/Annotations/F0 --notes vocadito/Annotations/Notes
