#pragma once

#include "StyleProfile.h"
#include "../NotePlan.h"
#include "../PitchTracker.h"
#include <vector>

namespace ve
{
    // Learns how a vocal is *sung* (not whose voice it is) from reference recordings,
    // and turns that into a StyleProfile. It measures, per note:
    //   - how far note centres sit from the semitone grid       -> correction
    //   - how much the pitch wanders on held, non-vibrato notes -> keepVariation
    //   - vibrato rate, depth, onset and how often it is used   -> vibrato
    //   - scoops into notes (start below, rise to pitch)        -> scoopCents/scoopMs
    //   - glide time between connected notes                    -> slides, retuneMs
    //   - onset spread and swing against the tempo grid         -> quantize/swing
    // Timbre, words and melody are never stored: only these statistics.
    namespace StyleLearner
    {
        // Per-take statistics. Medians over notes; counts say how much evidence there is.
        struct TakeStats
        {
            double voicedSeconds = 0.0;
            int notes = 0;

            float tuningOffsetCents = 0.0f;   // whole-take offset from A440, removed before in-tune checks
            float inTuneCents = 0.0f;  int inTuneNotes = 0;      // median |centre - nearest semitone|
            float settleMs = 0.0f;     int settleNotes = 0;      // median time to reach the note's pitch
            float wanderCents = 0.0f;  int wanderNotes = 0;      // median pitch spread on steady, non-vibrato notes

            int longNotes = 0, vibratoNotes = 0;                 // long = long enough to show vibrato
            float vibratoRateHz = 0.0f, vibratoDepthCents = 0.0f, vibratoOnsetMs = 0.0f;

            int scoopCandidates = 0, scoopedNotes = 0;
            float scoopCents = 0.0f, scoopMs = 0.0f;

            int transitions = 0;
            float transitionMs = 0.0f;   // 10%..90% of the move between connected notes

            // Timing (only when a tempo is known and the onsets line up with it).
            bool timingValid = false;
            int timedOnsets = 0;
            float gridLock = 0.0f;       // 0..1, how strongly onsets cluster on the 16th grid
            float onsetSpreadMs = 0.0f;  // median distance of on-grid onsets from the grid
            float swing = 0.0f;          // 0 straight .. 1 triplet swing on off-16ths
            int offbeatOnsets = 0;

            juce::var toVar() const;
            static TakeStats fromVar (const juce::var&);
        };

        TakeStats analyse (const PitchTrack&, const std::vector<Note>&, double bpm /* 0 = unknown */);

        // Pitch-tracks and segments a dry, monophonic vocal, then analyses it.
        TakeStats analyseAudio (const juce::AudioBuffer<float>&, double sampleRate, double bpm, JobContext* = nullptr);

        // Combines several takes (median across takes, so one long take cannot dominate).
        TakeStats combine (const std::vector<TakeStats>&);

        // Profile values implied by the statistics alone, before blending.
        struct Suggestion
        {
            StyleProfile values;
            // How much each group of values can be trusted, 0..1 (driven by note counts).
            float pitchTrust = 0.0f, vibratoTrust = 0.0f, scoopTrust = 0.0f, slideTrust = 0.0f, timingTrust = 0.0f;
        };
        Suggestion suggest (const TakeStats&, int takeCount);

        // Moves `base` toward the suggestion. `strength` 0 = keep base, 1 = take the
        // measured values wherever the evidence allows. Id/name/category come from base.
        StyleProfile learn (const StyleProfile& base, const TakeStats&, int takeCount, float strength = 1.0f);

        // Contrast learning: moves `base` by how this material differs from `pool` (all the
        // reference material together), times `gain`. What every singer does alike cancels
        // out, so a genre keeps only what sets it apart, and gain > 1 exaggerates that.
        StyleProfile learnContrast (const StyleProfile& base, const TakeStats& genre, int genreTakes,
                                    const TakeStats& pool, int poolTakes, float gain = 2.0f);

        // Profile JSON (with an "inherits" link back to the base style and a "learned" note).
        juce::String toJson (const StyleProfile& learned, const juce::String& inherits,
                             const TakeStats&, int takeCount);
    }
}
