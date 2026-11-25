#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "JobQueue.h"
#include "TempoMap.h"
#include <array>
#include <vector>

namespace ve
{
    using Chroma = std::array<float, 12>; // C, C#, D ... B

    enum class ChordQuality : int { none = 0, major, minor, dominant7, sus2, sus4, diminished };

    struct ChordSpan
    {
        double startPpq = 0.0;
        double endPpq = 0.0;
        int root = -1;                       // pitch class, -1 = no chord
        ChordQuality quality = ChordQuality::none;
        float confidence = 0.0f;
    };

    struct KeyEstimate
    {
        int tonic = -1;                      // pitch class, -1 = unknown
        bool minor = false;
        float confidence = 0.0f;             // best correlation
        float margin = 0.0f;                 // best minus second best

        bool isValid() const noexcept { return tonic >= 0; }
    };

    struct SidechainAnalysisResult
    {
        std::vector<double> beatStartPpq;
        std::vector<Chroma> beatChroma;      // L2-normalised per beat
        std::vector<float> beatEnergy;
        Chroma chromaSum {};                 // energy-weighted sum, for session-wide key
        KeyEstimate key;
        std::vector<ChordSpan> chords;       // merged spans
    };

    namespace Analysis
    {
        // Full analysis of a sidechain take. `tempoMap` is the region's tempo map.
        juce::Result analyseSidechain (const juce::AudioBuffer<float>& audio, double sampleRate,
                                       const TempoMap& tempoMap, JobContext* ctx,
                                       SidechainAnalysisResult& out);

        // Chroma of a mono signal segment (sum of FFT frames).
        Chroma computeChroma (const float* mono, int numSamples, double sampleRate);

        // Krumhansl-Schmuckler key estimate.
        KeyEstimate estimateKey (const Chroma& chroma);

        struct ChordScore { int root; ChordQuality quality; float score; };
        ChordScore bestChord (const Chroma& chroma);

        // Viterbi smoothing over per-beat chord candidates; returns state per beat.
        std::vector<int> smoothChords (const std::vector<Chroma>& beats, const std::vector<float>& energy,
                                       float selfTransition = 0.75f);

        int numChordStates();
        void stateToChord (int state, int& root, ChordQuality& quality);

        juce::String pitchClassName (int pc);
        juce::String keyName (int tonic, bool minor);
        juce::String chordName (int root, ChordQuality quality);
    }
}
