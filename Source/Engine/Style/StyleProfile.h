#pragma once

#include <juce_core/juce_core.h>
#include <map>
#include <vector>

namespace ve
{
    // How a genre performs a note plan. Loaded from JSON; a profile can inherit
    // from another and override only what differs.
    //
    // {
    //   "id": "trap", "name": "Trap", "inherits": "hip-hop", "category": "Hip-hop",
    //   "pitch":   { "correction": 1.0, "retuneMs": 5, "keepVariation": 0.1 },
    //   "vibrato": { "addDepthCents": 0, "rateHz": 5.5, "onsetMs": 300 },
    //   "slides":  { "betweenNotesMs": 25, "maxGapMs": 80, "scoopCents": 0, "scoopMs": 0 },
    //   "timing":  { "quantize": 0.0, "grid": 16 }
    // }
    struct StyleProfile
    {
        juce::String id = "natural", name = "Natural", category = "Basic", inherits, description;

        // Pitch: how hard notes are pulled to target.
        float correction = 0.6f;     // default for the Correct slider when the style is picked
        float retuneMs = 40.0f;      // time to reach the target after a note starts (0 = instant, "hard tune")
        float keepVariation = 1.0f;  // 1 = keep the singer's vibrato/drift around the note, 0 = flat line

        // Vibrato added on long notes (on top of what is kept).
        float vibratoDepthCents = 0.0f;
        float vibratoRateHz = 5.5f;
        float vibratoOnsetMs = 250.0f;

        // Glides.
        float slideMs = 0.0f;        // portamento between consecutive corrected notes
        float slideMaxGapMs = 80.0f; // only when the gap between notes is shorter than this
        float scoopCents = 0.0f;     // start each note this far below and scoop up
        float scoopMs = 0.0f;

        // Timing (stored now, applied in a later step: needs time-stretching).
        float quantize = 0.0f;
        int grid = 16;

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
