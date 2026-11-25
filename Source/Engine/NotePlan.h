#pragma once

#include "PitchTracker.h"
#include "SidechainAnalysis.h"
#include <vector>

namespace ve
{
    // One sung note: what was detected and where correction should take it.
    struct Note
    {
        std::int64_t start = 0;      // region-local samples (capture rate)
        std::int64_t end = 0;
        float detectedMidi = 0.0f;   // median pitch of the note (fractional MIDI)
        float targetMidi = 0.0f;     // pitch the correction pulls toward
        float correction = 0.0f;     // 0 = leave as sung, 1 = fully on target
        bool userEdited = false;     // set when the user moved it; automatic re-snaps leave it alone
        std::int64_t timeShift = 0;  // user timing nudge in samples (+ = later); applied by time-stretching

        float centsOff() const noexcept { return (detectedMidi - targetMidi) * 100.0f; }
    };

    // Per-region plan for how the take should be re-rendered. Stage 3 adds
    // style/expression parameters next to the notes.
    struct NotePlan
    {
        std::vector<Note> notes;

        // True when rendering this plan would reproduce the captured audio.
        bool isIdentity() const noexcept;
        std::uint64_t hash() const noexcept;
    };

    namespace NoteSegmenter
    {
        struct Settings
        {
            float minNoteMs = 70.0f;       // shorter blobs are dropped
            float splitSemitones = 0.7f;   // pitch move that starts a new note...
            float splitHoldMs = 45.0f;     // ...if held this long
            int maxGapFrames = 2;          // unvoiced frames bridged inside a note
            int medianFrames = 5;
        };

        std::vector<Note> segment (const PitchTrack&, const KeyEstimate& key, const Settings& = {});

        // Nearest in-key MIDI note (natural minor for minor keys); nearest semitone without a key.
        float snapToKey (float midi, const KeyEstimate& key) noexcept;

        // Re-snaps targets of notes the user has not edited (e.g. after a key change).
        void resnap (std::vector<Note>&, const KeyEstimate& key);
    }
}
