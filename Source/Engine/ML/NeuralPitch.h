#pragma once

#include "MelSpectrogram.h"
#include "../JobQueue.h"
#include <juce_core/juce_core.h>
#include <vector>

namespace ve::NeuralPitch
{
    // Neural pitch tracking with FCPE (torchfcpe, MIT licence): a convolutional network that
    // reads the 128-band log-mel of 16 kHz audio and outputs, every 10 ms, a salience over
    // 360 pitch bins from 32.7 Hz to 1975.5 Hz. It was trained with music, noise and
    // key-shift augmentation, so it holds up on breathy, gritty and reverberant vocals where
    // autocorrelation trackers jump octaves or drop out.
    //
    // The model file is `pitch-fcpe.onnx`. It is looked for, in order:
    //   1. $VE_PITCH_MODEL (a full path)
    //   2. next to the plugin binary (inside the .vst3 bundle) or the executable
    //   3. Documents/RainWorks/Engineer/Models/pitch-fcpe.onnx
    // tools/pitch-models/export_fcpe.py makes it; the Windows CI build puts it in the bundle.
    // Without the model or ONNX Runtime the DSP tracker (YIN) is used, as before.

    constexpr const char* modelFileName = "pitch-fcpe.onnx";
    constexpr int numBins = 360;
    constexpr float minHz = 32.7f, maxHz = 1975.5f;

    juce::File findModel();

    // True when ONNX Runtime is present and the model file was found and loads.
    // Loads it on first call (a few hundred ms), so call from a background thread.
    bool isAvailable (juce::String* why = nullptr);

    // Short status line for the UI: "Neural pitch (FCPE) on CPU" or why it's off.
    juce::String describe();

    // Pitch bin centre in Hz.
    float binToHz (float bin) noexcept;

    struct Frames
    {
        std::vector<float> f0;          // Hz, decoded with a local weighted average around the peak (0 = below threshold)
        std::vector<float> confidence;  // peak salience 0..1
    };

    // Runs the model on a mel spectrogram (MelSpectrogram frames, 10 ms each).
    // `threshold`: peak salience below which a frame counts as unvoiced.
    juce::Result run (const ml::MelSpectrogram&, JobContext*, Frames& out, float threshold = 0.05f);

    // Decodes one frame of salience (numBins values).
    void decode (const float* salience, float& f0, float& confidence, float threshold) noexcept;
}
