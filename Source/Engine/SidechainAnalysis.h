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

    // Scale that note targets snap to, relative to the key's tonic. Each has a major-key
    // and a minor-key form, so a style can say "pentatonic" and fit any key.
    enum class ScaleMode : int
    {
        key = 0,      // the key's own scale: major, or natural minor
        pentatonic,   // major pentatonic / minor pentatonic
        blues,        // major blues (adds b3) / minor blues (adds b5)
        harmonic,     // harmonic major / harmonic minor
        modal,        // mixolydian (major keys) / dorian (minor keys)
        chromatic     // nearest semitone, no key
    };
    ScaleMode scaleModeFromString (const juce::String&) noexcept;
    juce::String toString (ScaleMode) noexcept;

    struct KeyEstimate
    {
        int tonic = -1;                      // pitch class, -1 = unknown
        bool minor = false;
        float confidence = 0.0f;             // best correlation
        float margin = 0.0f;                 // best minus second best
        // Which notes of the key targets snap to. Set from the style, not detected.
        ScaleMode scale = ScaleMode::key;

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
