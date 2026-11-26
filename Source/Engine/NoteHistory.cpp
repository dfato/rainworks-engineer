#include "NoteHistory.h"

namespace ve
{
    void NoteHistory::record (std::vector<Change> step)
    {
        // Drop takes that didn't actually change.
        step.erase (std::remove_if (step.begin(), step.end(), [] (const Change& c)
        {
            if (c.before.size() != c.after.size()) return false;
            for (size_t i = 0; i < c.before.size(); ++i)
                if (! NoteEdit::editableFieldsEqual (c.before[i], c.after[i])) return false;
            return true;
        }), step.end());
        if (step.empty()) return;

        undoSteps.push_back (std::move (step));
        if ((int) undoSteps.size() > limit)
            undoSteps.erase (undoSteps.begin());
        redoSteps.clear();
    }

    bool NoteHistory::apply (std::vector<Note>& current, const std::vector<Note>& from, const std::vector<Note>& to)
    {
        if (current.size() != from.size())
            return false;
        if (from.size() != to.size())
        {
            current = to;   // split or merge: the whole list
            return true;
        }
        for (size_t i = 0; i < to.size(); ++i)
            if (! NoteEdit::editableFieldsEqual (from[i], to[i]))
                NoteEdit::copyEditableFields (current[i], to[i]);
        return true;
    }

    juce::StringArray NoteHistory::move (std::vector<std::vector<Change>>& source, std::vector<std::vector<Change>>& dest,
                                         bool backwards, const Getter& get, const Setter& set)
    {
        if (source.empty()) return {};
        auto step = std::move (source.back());
        source.pop_back();

        // Check every take first so a step is applied whole or not at all.
        std::vector<std::vector<Note>> updated;
        for (auto& c : step)
        {
            auto cur = get (c.regionId);
            if (! cur || ! apply (*cur, backwards ? c.after : c.before, backwards ? c.before : c.after))
            {
                clear();
                return {};
            }
            updated.push_back (std::move (*cur));
        }

        juce::StringArray changed;
        for (size_t i = 0; i < step.size(); ++i)
        {
            set (step[i].regionId, updated[i]);
            changed.addIfNotAlreadyThere (step[i].regionId);
        }
        dest.push_back (std::move (step));
        return changed;
    }

    juce::StringArray NoteHistory::undo (const Getter& get, const Setter& set) { return move (undoSteps, redoSteps, true, get, set); }
    juce::StringArray NoteHistory::redo (const Getter& get, const Setter& set) { return move (redoSteps, undoSteps, false, get, set); }
}
