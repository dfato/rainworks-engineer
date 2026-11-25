#pragma once

#include "../NotePlan.h"

namespace ve
{
    // Stage 4: what the singer most likely meant with each note, read from the pitch track.
    // Used to correct the mistakes and keep the choices: a deliberate slide, a blue note,
    // vibrato or a spoken line shouldn't be flattened onto the key like an out-of-tune note.
    namespace NoteIntent
    {
        enum Flags : std::uint8_t
        {
            slide      = 1 << 0,  // pitch moves through the note on purpose (glide/bend)
            vibrato    = 1 << 1,  // regular 4-8 Hz wobble on a held note
            scoop      = 1 << 2,  // starts below and rises into the note
            passing    = 1 << 3,  // short note on the way between two others
            accidental = 1 << 4,  // held, steady and on a semitone outside the key: a chosen note
            spoken     = 1 << 5,  // unstable / low-confidence pitch: speech, rap, whisper
            quiet      = 1 << 6,  // much quieter than the take: ad-lib or ghost note
        };

        struct Settings
        {
            float passingMaxMs = 120.0f;
            float passingMaxGapMs = 40.0f;
            float slideSemitones = 1.0f;
            float vibratoMinMs = 350.0f, vibratoMinCents = 20.0f, vibratoMinHz = 4.0f, vibratoMaxHz = 8.0f;
            float scoopSemitones = 0.8f, scoopMs = 80.0f;
            float accidentalMinMs = 250.0f, accidentalMaxCents = 25.0f;
            float spokenMinConfidence = 0.7f, spokenMaxSpreadCents = 60.0f;
            float quietBelowDb = -18.0f;
        };

        struct Analysis
        {
            std::uint8_t flags = 0;
            float scale = 1.0f;        // multiplier on the correction amount
            float target = 0.0f;       // suggested target (MIDI)
            float vibratoHz = 0.0f, vibratoCents = 0.0f;
        };

        // Analyses one note. `key` may be invalid (no accidentals then).
        Analysis analyse (const PitchTrack&, const std::vector<Note>&, int index, const KeyEstimate& key,
                          float takeMedianRms, const Settings& = {});

        // Analyses every note and writes intent + intentScale, and the target of notes the user
        // hasn't edited (key snap, or the chromatic note for accidentals).
        void apply (const PitchTrack&, std::vector<Note>&, const KeyEstimate& key, const Settings& = {});

        // Clears intent (all notes treated alike, targets snapped to the key).
        void clear (std::vector<Note>&, const KeyEstimate& key);

        // Correction for a note the user hasn't edited.
        inline float correctionFor (const Note& n, float globalAmount, bool useIntent) noexcept
        {
            return useIntent ? globalAmount * n.intentScale : globalAmount;
        }

        juce::String describe (std::uint8_t flags);   // "Slide, vibrato" / "" if none
    }
}
