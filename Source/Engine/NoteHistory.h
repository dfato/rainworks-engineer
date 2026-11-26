#pragma once

#include <juce_core/juce_core.h>
#include "NotePlan.h"
#include <functional>
#include <optional>

namespace ve
{
    // Undo/redo for graph-editor note edits. Each step remembers, per take, the notes
    // before and after the edit. Undo puts back only what the edit changed (target,
    // correction, timing, vibrato, drift), so a key or style change made in between is
    // kept on the other notes. Split and merge change the note count and swap the whole
    // list. If the notes were re-analysed since (the count no longer matches) the
    // history is dropped rather than guessing. Message thread only.
    class NoteHistory
    {
    public:
        struct Change
        {
            juce::String regionId;
            std::vector<Note> before, after;
        };

        using Getter = std::function<std::optional<std::vector<Note>> (const juce::String& regionId)>;
        using Setter = std::function<void (const juce::String& regionId, const std::vector<Note>&)>;

        explicit NoteHistory (int maxSteps = 200) : limit (maxSteps) {}

        // Adds one undo step (several takes may change together). Clears redo.
        void record (std::vector<Change> step);

        bool canUndo() const noexcept { return ! undoSteps.empty(); }
        bool canRedo() const noexcept { return ! redoSteps.empty(); }
        int getNumUndoSteps() const noexcept { return (int) undoSteps.size(); }
        void clear() { undoSteps.clear(); redoSteps.clear(); }

        // Returns the takes that changed. Empty when there was nothing to do, or when the
        // notes no longer match the history (it is then cleared).
        juce::StringArray undo (const Getter&, const Setter&);
        juce::StringArray redo (const Getter&, const Setter&);

        // Applies one change in the `from` -> `to` direction to `current`. False if
        // `current` doesn't line up with `from`.
        static bool apply (std::vector<Note>& current, const std::vector<Note>& from, const std::vector<Note>& to);

    private:
        juce::StringArray move (std::vector<std::vector<Change>>& source, std::vector<std::vector<Change>>& dest,
                                bool backwards, const Getter&, const Setter&);

        int limit;
        std::vector<std::vector<Change>> undoSteps, redoSteps;
    };
}
