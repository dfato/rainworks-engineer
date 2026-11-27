#pragma once

#include "SidechainAnalysis.h"
#include <cstdint>
#include <vector>

namespace ve::MelodyHunt
{
    // Hunt: re-pitches the notes of a take (keeping their rhythm) into a new melody
    // inside the key and scale. It writes in phrases, builds a short motif and brings it
    // back, puts chord tones on the strong beats, mostly moves in steps, fills in after a
    // leap, aims each phrase at a contour and lands the last phrase on the root.
    // Every call with a new seed gives a new idea; the same seed gives the same melody.

    enum class Flavour : int { followStyle = 0, pop, rock, rnb, hipHop, trap, indie };
    enum class Contour : int { automatic = 0, arch, rise, fall, wave, valley, hook };
    enum class Variation : int { subtle = 0, fresh, wild };      // how far from what is there now
    enum class Repetition : int { hooky = 0, balanced, free };   // how often the motif comes back
    enum class Range : int { tight = 0, normal, wide };
    enum class Register : int { asSung = 0, higher, lower };

    struct Options
    {
        Flavour flavour = Flavour::followStyle;
        Contour contour = Contour::automatic;
        Variation variation = Variation::fresh;
        Repetition repetition = Repetition::balanced;
        Range range = Range::normal;
        Register registerShift = Register::asSung;
        bool followChords = true;   // use the beat's detected chords when there are any
        bool endOnRoot = true;      // last note of the last phrase lands on the key's root
    };

    struct InNote
    {
        double startPpq = 0.0;      // host quarter notes, where the note will sound
        double lengthPpq = 0.25;
        float sungMidi = 60.0f;     // what was sung (keeps the new melody singable from here)
        float currentMidi = 60.0f;  // where the note is now (Subtle starts from this)
    };

    struct Result
    {
        std::vector<float> midi;    // new target per input note, same order as the input
        int phrases = 0;
        bool usedDetectedChords = false;
        KeyEstimate key;            // the key and scale that were used
        Flavour flavour = Flavour::pop;
        juce::String form;          // e.g. "A A' B A"
        juce::String description;   // one line for the status bar
    };

    // `flavour` should be resolved (not followStyle); followStyle is treated as Pop.
    // Chords are in host PPQ. `beatsPerBar` is quarter notes per bar.
    Result hunt (const std::vector<InNote>& notes, const KeyEstimate& key, const std::vector<ChordSpan>& chords,
                 const Options& options, std::uint32_t seed, double beatsPerBar = 4.0);

    // Which genre flavour a style id stands for (pop, rock, rnb, hip-hop, trap, indie; others -> Pop).
    Flavour flavourForStyle (const juce::String& styleId) noexcept;
    Flavour resolve (Flavour, const juce::String& styleId) noexcept;

    // A key guessed from the notes themselves, for takes with no beat or key set.
    KeyEstimate keyFromNotes (const std::vector<InNote>&);

    juce::String toString (Flavour);
    juce::String toString (Contour);
    juce::String toString (Variation);
    juce::String toString (Repetition);
    juce::String toString (Range);
    juce::String toString (Register);
}
