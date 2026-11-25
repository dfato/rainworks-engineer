#pragma once

#include "VoiceModel.h"
#include "../JobQueue.h"
#include "../ML/InferenceBackend.h"
#include <vector>

namespace ve
{
    // Retrieval-based voice conversion (RVC v2 style) on top of two inference backends:
    //   content encoder: 16 kHz audio -> content features (50 per second)
    //   synthesiser:     features (100 per second) + pitch -> audio in the singer's voice
    // The pitch comes from the render plan, so corrected notes, styles and timing all carry over.
    // Long takes are converted in overlapping chunks and crossfaded. Never call from the audio thread.
    class VoiceConverter
    {
    public:
        struct Settings
        {
            float indexRate = 0.3f;        // 0 = encoder features only, 1 = fully from the singer's feature bank
            float protect = 0.33f;         // < 0.5 keeps more of the original sound on unvoiced parts (breaths, consonants)
            double chunkSeconds = 12.0;
            double overlapSeconds = 0.25;  // crossfade between chunks
            double contextSeconds = 0.3;   // extra audio either side of a chunk, trimmed after
            std::uint32_t seed = 1;        // noise input; fixed so renders are repeatable
        };

        VoiceConverter (ml::InferenceBackend& encoder, ml::InferenceBackend& synth,
                        const VoiceModelInfo&, const FeatureBank* bank = nullptr);

        // Checks the two models have the inputs this pipeline feeds. Empty = fine.
        juce::String validate() const;

        // `in`: mono, n samples at inRate. `plannedF0`: Hz per input sample (0 = unvoiced).
        // `out`: audio at the model's rate, round (n * modelRate / inRate) samples.
        juce::Result convert (const float* in, int n, double inRate, const std::vector<float>& plannedF0,
                              const Settings&, JobContext*, std::vector<float>& out);

        static std::int64_t coarsePitch (float hz) noexcept;  // RVC mel-scale pitch bins 1..255
        static constexpr double encoderRate = 16000.0;
        static constexpr int encoderHop = 320;                 // 50 feature frames per second
        static constexpr int frameRate = 100;                  // synthesiser frames per second

    private:
        juce::Result runEncoder (const std::vector<float>& audio16k, std::vector<float>& feats, int& frames);

        ml::InferenceBackend& encoder;
        ml::InferenceBackend& synth;
        VoiceModelInfo model;
        const FeatureBank* bank;
    };
}
