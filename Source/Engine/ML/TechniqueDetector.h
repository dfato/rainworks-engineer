#pragma once

#include "MelSpectrogram.h"
#include "../PitchTracker.h"

namespace ve::TechniqueDetector
{
    // Vocal technique classifier: a small convolutional network (about 0.3 M weights,
    // embedded in the plugin) that reads the same 16 kHz log-mel as the pitch model and
    // says, every 10 ms, how likely the voice is
    //   vibrato  - periodic pitch/loudness wobble
    //   breathy  - air in the tone
    //   belt     - loud, chest-dominant singing
    //   grit     - distortion: scream, growl, fry, rasp
    //   spoken   - talking/rapping rather than holding pitches
    // It was trained by tools/technique-model/train.py on VocalSet (CC BY 4.0) and the
    // Extreme Metal Vocals Dataset (CC BY 4.0). Needs ONNX Runtime; without it the
    // pitch-curve rules in NoteIntent work alone.

    bool isAvailable (juce::String* why = nullptr);

    // Per-frame probabilities, one TechniqueFrame per mel frame (10 ms).
    juce::Result run (const ml::MelSpectrogram&, JobContext*, std::vector<TechniqueFrame>& out);

    // Mean probabilities over region-local samples [start, end) of a track (zeros if the
    // track has no technique frames). Only loud-enough frames count.
    TechniqueFrame summarise (const PitchTrack&, std::int64_t start, std::int64_t end);

    const char* kindName (int kind) noexcept;
}
