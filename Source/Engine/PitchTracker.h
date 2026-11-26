#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "JobQueue.h"
#include <vector>

namespace ve
{
    // One analysis frame of a monophonic vocal.
    struct PitchFrame
    {
        float f0 = 0.0f;          // Hz, 0 = unvoiced
        float confidence = 0.0f;  // 0..1 (1 - YIN aperiodicity)
        float rms = 0.0f;         // linear
    };

    // How a vocal is being sung at one moment, from the technique classifier (0..1 each).
    struct TechniqueFrame
    {
        enum Kind { vibrato, breathy, belt, grit, spoken, count };
        float p[count] {};
        float operator[] (int k) const noexcept { return p[k]; }
    };

    // Which tracker made a track.
    enum class PitchEngine : std::uint8_t { dsp = 0, neural = 1 };

    // Frame-based pitch track of a region. Frame i is centred on sample i * hopSize
    // (region-local, at sampleRate).
    struct PitchTrack
    {
        double sampleRate = 0.0;
        int hopSize = 256;
        std::vector<PitchFrame> frames;
        PitchEngine engine = PitchEngine::dsp;
        // Same length as frames when the technique classifier ran, otherwise empty.
        std::vector<TechniqueFrame> technique;
        bool hasTechnique() const noexcept { return ! technique.empty() && technique.size() == frames.size(); }

        double frameToSample (int i) const noexcept { return (double) i * hopSize; }
        bool isVoiced (int i) const noexcept { return frames[(size_t) i].f0 > 0.0f; }

        static float hzToMidi (float hz) noexcept { return hz > 0.0f ? 69.0f + 12.0f * std::log2 (hz / 440.0f) : 0.0f; }
        static float midiToHz (float m) noexcept  { return 440.0f * std::pow (2.0f, (m - 69.0f) / 12.0f); }

        // Compact binary form for the data folder (pitch tracks are too large for the ValueTree).
        juce::MemoryBlock serialise() const;
        static bool deserialise (const juce::MemoryBlock&, PitchTrack& out);
    };

    namespace PitchTracker
    {
        struct Settings
        {
            float minHz = 65.0f;          // C2
            float maxHz = 1100.0f;        // ~C#6
            float threshold = 0.15f;      // YIN absolute threshold
            float minConfidence = 0.55f;  // below = unvoiced
            float silenceDb = -50.0f;     // below = unvoiced
            int hopSize = 0;              // 0 = ~5 ms
        };

        // FFT-accelerated YIN over a mono mix of `audio`.
        juce::Result track (const juce::AudioBuffer<float>& audio, double sampleRate, const Settings&,
                            JobContext*, PitchTrack& out);

        enum class Engine { automatic, dsp, neural };

        struct NeuralSettings
        {
            float voicingThreshold = 0.5f;  // FCPE peak salience below this = unvoiced
            float refineCents = 50.0f;      // > 0: use the YIN estimate where it agrees within this many cents
            float rescueThreshold = 0.2f;   // > 0: frames down to this salience stay voiced when a confident YIN agrees
            bool techniques = true;         // also run the technique classifier
        };

        // The best tracker this build and machine have: the neural tracker (FCPE) when its
        // model and ONNX Runtime are available, with YIN's per-frame loudness; otherwise
        // YIN alone. `Engine::neural` fails instead of falling back. Background threads only.
        // `status` (optional) gets a short note on what ran and why.
        juce::Result trackBest (const juce::AudioBuffer<float>& audio, double sampleRate, const Settings&,
                                JobContext*, PitchTrack& out, Engine = Engine::automatic,
                                const NeuralSettings& = {}, juce::String* status = nullptr);
    }
}
