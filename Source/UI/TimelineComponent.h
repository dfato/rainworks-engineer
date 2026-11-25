#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Engine/HostContext.h"
#include "Engine/NotePlan.h"
#include "Engine/RenderCache.h"
#include "Engine/SidechainAnalysis.h"
#include "Engine/TempoMap.h"

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
        bool trackingPitch = false;
    };

    struct TimelineData
    {
        std::vector<TimelineRegion> regions;  // sorted by order, oldest first
        juce::String keyText;
        bool keyIsOverride = false;
    };

    // Takes lane with waveforms, bar/beat grid from the host tempo, chord lane,
    // key display, a reserved note lane (Stage 2) and a playhead.
    // Ctrl/Cmd + wheel zooms, wheel scrolls, right-click a take for options.
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

        void paint (juce::Graphics&) override;
        void resized() override;
        void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;

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

        static constexpr int gutter = 64;

        TimelineData data;
        HostSnapshot host;
        bool liveCapturing = false;
        double liveStartPpq = 0.0, liveLengthPpq = 0.0;

        double viewStartPpq = 0.0;
        double pxPerPpq = 24.0;
        double dragStartView = 0.0;
        bool followPlayhead = true;

        juce::Rectangle<int> ruler, takesLane, chordLane, noteLane;
        juce::ScrollBar hScroll { false };
        juce::ToggleButton followButton { "Follow" };
    };
}
