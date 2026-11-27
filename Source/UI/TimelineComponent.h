#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine/HostContext.h"
#include "Engine/MelodyHunter.h"
#include "Engine/NotePlan.h"
#include "Engine/RenderCache.h"
#include "Engine/SidechainAnalysis.h"
#include "Engine/TempoMap.h"
#include "Engine/TimeWarp.h"
#include <map>
#include <set>

namespace ve::ui
{
    struct TimelineRegion
    {
        juce::String id;
        std::int64_t order = 0;
        double startPpq = 0.0, endPpq = 0.0;
        TempoMap tempoMap;                           // region-local, capture sample rate
        std::shared_ptr<const WaveformPeaks> peaks;  // may be null while loading
        double renderedRate = 0.0;                   // sample rate the peaks were built at
        bool missing = false, loading = false, hasSidechain = false;
        std::vector<ChordSpan> chords;
        std::vector<Note> notes;                     // region-local samples at capture rate
        std::vector<double> noteOutStart;            // where each note will sound after timing edits/quantize
        bool trackingPitch = false;
    };

    struct TimelineData
    {
        std::vector<TimelineRegion> regions;  // sorted by order, oldest first
        juce::String keyText;
        bool keyIsOverride = false;
        KeyEstimate scaleKey;                 // key + the style's scale: what "Snap to scale" lands on
        juce::String scaleText;               // e.g. "A minor, pentatonic"
        bool snapToScale = true;
        bool canUndo = false, canRedo = false;
        juce::String styleId;                 // the loaded style: Hunt's "Follow the style" flavour
        MelodyHunt::Options huntOptions;
    };

    struct TimelineNoteEdit
    {
        juce::String regionId;
        int index = -1;
        Note note;
    };

    // Takes lane with waveforms, bar/beat grid from the host tempo, chord lane,
    // key display, the note graph and a playhead.
    // Ctrl/Cmd + wheel zooms, wheel scrolls, right-click a take for options.
    //
    // Note graph: click a note to select it, Ctrl/Cmd-click or Shift-click to add or
    // remove notes, drag on empty space for a box selection, Ctrl/Cmd+A for all.
    // Dragging moves every selected note (up/down in pitch; Shift + drag in time).
    // With Snap to scale on, pitch moves land on the key's scale notes; Alt is free.
    // The tool row edits Correct, Vibrato and Drift of the selection.
    // Hunt (H) writes a new melody into the selection, or every note when nothing is
    // selected; its arrow opens the options. Each press is a new idea and one undo step.
    class TimelineComponent final : public juce::Component,
                                    private juce::ScrollBar::Listener
    {
    public:
        TimelineComponent();

        void setData (TimelineData);
        void setHost (const HostSnapshot&);
        void setLiveCapture (bool capturing, double startPpq, double lengthPpq);

        std::function<void (const juce::String&)> onDeleteRegion;
        std::function<void (const juce::String&)> onAnalyseRegion;
        std::function<void (const std::vector<TimelineNoteEdit>&)> onNotesChanged;   // one undo step
        std::function<void()> onUndo, onRedo;
        std::function<void (bool)> onSnapToScaleChanged;
        std::function<void (const juce::String&, int, std::int64_t)> onSplitNote;   // region, note, sung sample
        std::function<void (const juce::String&, int)> onMergeNote;                 // region, note (joins with next)
        std::function<void (const MelodyHunt::Options&)> onHuntOptionsChanged;
        std::function<void (const juce::String&)> onStatus;                         // one line for the status bar

        // Re-pitches the selection (or every note) into a new melody. Returns the status line.
        juce::String hunt();
        juce::String huntWith (std::uint32_t seed);
        juce::PopupMenu getHuntMenu() const;

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        void mouseDoubleClick (const juce::MouseEvent&) override;
        bool keyPressed (const juce::KeyPress&) override;

        int getNumSelectedNotes() const noexcept { return (int) selection.size(); }

        // The two dark glass screens (local coordinates), for the editor's side rails.
        juce::Rectangle<int> getTakesScreenBounds() const noexcept { return takesScreen; }
        juce::Rectangle<int> getNotesScreenBounds() const noexcept { return notesScreen; }

        // Reserved lane geometry so Stage 2 note blobs drop straight in.
        juce::Rectangle<int> getNoteLaneBounds() const noexcept { return noteLane; }
        double xToPpq (float x) const noexcept;
        float ppqToX (double ppq) const noexcept;

    private:
        void scrollBarMoved (juce::ScrollBar*, double newRangeStart) override;
        void updateScrollRange();
        void drawGrid (juce::Graphics&, juce::Rectangle<int> area, bool labels);
        void drawRegion (juce::Graphics&, const TimelineRegion&);
        void drawChords (juce::Graphics&);
        void drawNotes (juce::Graphics&);
        const TimelineRegion* regionAt (juce::Point<int>) const;
        struct NoteHit { int region = -1, note = -1; };
        NoteHit noteAt (juce::Point<int>) const;
        juce::Rectangle<float> noteBounds (const TimelineRegion&, int noteIndex, bool target) const;
        static double outStart (const TimelineRegion&, int noteIndex) noexcept;

        // ---- note selection and group edits ----
        using NoteKey = std::pair<juce::String, int>;   // region id, note index
        std::set<NoteKey> selection;
        bool isSelected (int region, int note) const;
        NoteKey keyFor (NoteHit h) const;
        NoteHit hitFor (const NoteKey&) const;
        void selectOnly (NoteHit);
        void selectAll();
        void pruneSelection (const TimelineData& old);
        std::vector<NoteHit> selectedHits() const;
        // Applies `fn` to each selected note, then reports them all as one edit.
        void editSelection (const std::function<void (Note&, TimelineRegion&, int)>& fn, bool commit = true);
        void commitSelection();
        void transposeSelection (int steps, bool octave, bool fine);
        void nudgeSelectionTime (int direction);
        void showNoteMenu (NoteHit clicked, std::int64_t splitAt);
        void showLaneMenu();
        void refreshTools();
        float snapPitch (float midi, bool fine) const;
        std::vector<NoteHit> huntScope() const;      // the selection, or every note
        void commitHits (const std::vector<NoteHit>&);
        void showHuntMenu();
        void handleHuntMenu (int result);
        void putBackSung();

        struct DragStart { float target = 0.0f; std::int64_t shift = 0; double out = 0.0; };
        std::map<NoteKey, DragStart> dragStarts;
        bool marqueeActive = false;
        juce::Rectangle<float> marquee;
        std::set<NoteKey> marqueeBase;
        bool pendingSelectOnly = false, pendingDeselect = false;

        static constexpr int gutter = 64;

        TimelineData data;
        HostSnapshot host;
        bool liveCapturing = false;
        double liveStartPpq = 0.0, liveLengthPpq = 0.0;

        double viewStartPpq = 0.0;
        double pxPerPpq = 24.0;
        double dragStartView = 0.0;
        bool followPlayhead = true;

        // Note lane geometry from the last paint, and the note being dragged.
        float noteLo = 48.0f, noteRowH = 4.0f, noteH = 6.0f;
        NoteHit dragNote;
        float dragStartTarget = 0.0f;
        double dragStartOut = 0.0;
        bool dragTiming = false;
        bool noteDragged = false;

        juce::Rectangle<int> ruler, takesLane, chordLane, noteLane, noteTools;
        juce::Rectangle<int> takesScreen, notesScreen;
        std::vector<juce::Rectangle<int>> toolGrooves;
        std::vector<std::pair<int, float>> octaveLabels;   // (midi, y) from the last drawNotes   // recessed wells behind the tool-row sliders
        juce::ScrollBar hScroll { false };
        juce::ToggleButton followButton { "Follow" };

        juce::ToggleButton snapButton { "Snap to scale" };
        juce::TextButton undoButton { "Undo" }, redoButton { "Redo" };
        juce::TextButton huntButton { "Hunt" }, huntMenuButton;
        juce::Label selectionLabel;
        juce::Label correctLabel { {}, "Correct" }, vibratoLabel { {}, "Vibrato" }, driftLabel { {}, "Drift" };
        juce::Slider correctSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider vibratoSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::Slider driftSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        bool sliderEditing = false;
    };
}
