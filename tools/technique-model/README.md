# Technique classifier

Rates vibrato, breathy, belt, grit and spoken every 10 ms from the 16 kHz log-mel the pitch model
also uses. The exported model is `Models/technique-classifier.onnx` (about 1 MB), compiled into the
plugin. See `Source/Engine/ML/TechniqueDetector.h`.

## Data (all allow commercial use of the trained model; no audio is committed)

| Set | Licence | Used for |
| --- | --- | --- |
| VocalSet 1.1 (zenodo 1442513) | CC BY 4.0 | vibrato/straight, breathy, belt, vocal fry, spoken |
| Extreme Metal Vocals Dataset (zenodo 17813678) | CC BY 4.0 | screams, growls, shrieks vs clean voice |
| CMU ARCTIC 15 s (zenodo 3066489) | CC0 | read speech |
| URSing (zenodo 6404999), Vocal.wav of 65 songs | CC BY 4.0 | sung pop lyrics (not speech) |

MERT and similar music models were ruled out: their weights are CC BY-NC.

## Steps

    python3 prepare.py --vocalset VocalSet/FULL --emvd emvd/audio --emvd-meta metadata_files.csv \
        --arctic cmu_arctic_concat15 --ursing ursing --out features.npz
    python3 train.py --features features.npz --out ../../Models/technique-classifier.onnx --report heldout-results.json

About 10 minutes on a 4-core CPU (an RTX 3060 is faster but not needed). Training adds random gain, EQ tilt,
a noise floor and room reverb (without reverb, a wet vocal's tail read as grit).

## Held-out results

`heldout-results.json`: singers and speakers never trained on (VocalSet female8/9, male10/11; EMVD singers
24-27; ARCTIC slt/bdl; every 6th URSing song). Clip-level accuracy: vibrato 96%, breathy 97%, belt 96%,
grit 99%, spoken 100%.

These sets are studio recordings of exercises. On vocadito (amateur singers, never trained on) it still
calls about 16% of sung frames "spoken" (median per take; one low French take 95%). That is why Intent uses
the spoken mark for information only and lets pitch evidence decide, and only softens correction for grit
when the classifier is sure (a clear scream or growl).
