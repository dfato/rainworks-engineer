#pragma once

#include <juce_core/juce_core.h>
#include "../SidechainAnalysis.h"
#include <cmath>
#include <map>
#include <vector>

namespace ve
{
    // How a genre performs a note plan. Loaded from JSON; a profile can inherit
    // from another and override only what differs.
    //
    // {
    //   "id": "trap", "name": "Trap", "inherits": "hip-hop", "category": "Hip-hop",
    //   "pitch":   { "correction": 1.0, "retuneMs": 5, "keepVariation": 0.1, "scale": "pentatonic" },
    //   "vibrato": { "addDepthCents": 0, "rateHz": 5.5, "onsetMs": 300 },
    //   "slides":  { "betweenNotesMs": 25, "maxGapMs": 80, "scoopCents": 0, "scoopMs": 0 },
    //   "timing":  { "quantize": 0.0, "grid": 16, "swing": 0.0 },
    //   "ornaments": { "runs": 0.3, "runNotes": 4, "runNoteMs": 90, "fallCents": 0, "fallMs": 0 },
    //   "tone":    { "brightness": 0.2, "warmth": 0.0, "grit": 0.0, "breath": 0.1 },
    //   "dynamics": { "compress": 0.5 }
    // }
    struct StyleProfile
    {
        juce::String id = "natural", name = "Natural", category = "Basic", inherits, description;

        // Pitch: how hard notes are pulled to target.
        float correction = 0.6f;     // default for the Correct slider when the style is picked
        float retuneMs = 40.0f;      // time to reach the target after a note starts (0 = instant, "hard tune")
        float keepVariation = 1.0f;  // 1 = keep the singer's vibrato/drift around the note, 0 = flat line
        // Which notes of the key targets snap to: "key", "pentatonic", "blues", "harmonic",
        // "modal" or "chromatic". Anything but "key" can move a note to another scale degree.
        ScaleMode scale = ScaleMode::key;

        // Vibrato added on long notes (on top of what is kept).
        float vibratoDepthCents = 0.0f;
        float vibratoRateHz = 5.5f;
        float vibratoOnsetMs = 250.0f;

        // Glides.
        float slideMs = 0.0f;        // portamento between consecutive corrected notes
        float slideMaxGapMs = 80.0f; // only when the gap between notes is shorter than this
        float scoopCents = 0.0f;     // start each note this far below and scoop up
        float scoopMs = 0.0f;

        // Timing: pull corrected note onsets toward the host grid (time-stretched).
        float quantize = 0.0f;       // 0 = as sung, 1 = onsets exactly on the grid
        int grid = 16;               // grid division per whole note (16 = sixteenths)
        float swing = 0.0f;          // 0 = straight, 1 = full triplet swing on every other grid step

        // Ornaments: notes added on top of the sung melody.
        float runs = 0.0f;           // 0..1, share of long notes that get a run (melisma) through the scale
        int runNotes = 4;            // notes in a run, 2..8
        float runNoteMs = 90.0f;     // length of each run note
        float fallCents = 0.0f;      // pitch falls off this far at the end of a phrase
        float fallMs = 0.0f;         // over this long

        // Tone of the voice (applied to the whole take after pitch).
        float brightness = 0.0f;     // -1 darker .. +1 brighter (presence and air)
        float warmth = 0.0f;         // -1 thinner .. +1 fuller low mids
        float grit = 0.0f;           // 0..1 rasp / saturation in the mids
        float breath = 0.0f;         // 0..1 airy breath noise that follows the voice
        // Dynamics.
        float compress = 0.0f;       // 0..1, evens out loud and quiet words

        bool hasTone() const noexcept
        {
            return std::abs (brightness) > 1.0e-3f || std::abs (warmth) > 1.0e-3f || grit > 1.0e-3f
                   || breath > 1.0e-3f || compress > 1.0e-3f;
        }
        // The style pushed further or pulled back: 0 = no style expression (only the
        // tuning), 1 = as designed, 2 = twice as strong. Correction and timing are unchanged.
        StyleProfile withAmount (float amount) const;

        juce::var toVar() const;
        std::uint64_t hash() const;
    };

    // Built-in profiles plus any user JSON files, with inheritance resolved.
    class StyleLibrary
    {
    public:
        StyleLibrary();  // loads the built-ins

        // Adds/overrides profiles from *.json files in a folder. Returns errors, if any.
        juce::StringArray loadFolder (const juce::File& folder);
        // Adds one JSON profile (raw, unresolved). Returns an error message or empty.
        juce::String addJson (const juce::String& json);

        // Resolved profile (inheritance applied). Unknown ids give "natural".
        StyleProfile get (const juce::String& id) const;
        bool contains (const juce::String& id) const { return raw.count (id) > 0; }

        struct Entry { juce::String id, name, category; };
        std::vector<Entry> list() const;   // sorted by category, then name

        static juce::File userStylesFolder();

    private:
        juce::var resolve (const juce::String& id, int depth) const;
        std::map<juce::String, juce::var> raw;
    };

    namespace BuiltinStyles
    {
        const std::vector<const char*>& json();
    }
}

namespace ve
{
    // Process-wide library: built-ins + the user's styles folder, loaded once.
    StyleLibrary& sharedStyleLibrary();
}
