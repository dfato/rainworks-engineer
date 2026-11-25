#include "TimelineComponent.h"
#include "Theme.h"

namespace ve::ui
{
    TimelineComponent::TimelineComponent()
    {
        hScroll.setAutoHide (false);
        hScroll.addListener (this);
        addAndMakeVisible (hScroll);

        followButton.setToggleState (true, juce::dontSendNotification);
        followButton.setColour (juce::ToggleButton::textColourId, Theme::textDim);
        followButton.onClick = [this] { followPlayhead = followButton.getToggleState(); };
        addAndMakeVisible (followButton);
    }

    void TimelineComponent::setData (TimelineData d)
    {
        data = std::move (d);
        updateScrollRange();
        repaint();
    }

    void TimelineComponent::setHost (const HostSnapshot& s)
    {
        host = s;

        if (followPlayhead && host.isPlaying && host.hasPpq && takesLane.getWidth() > 0)
        {
            const double visible = takesLane.getWidth() / pxPerPpq;
            if (host.ppqPosition < viewStartPpq || host.ppqPosition > viewStartPpq + visible * 0.92)
                viewStartPpq = juce::jmax (0.0, host.ppqPosition - visible * 0.08);
        }
        updateScrollRange();
        repaint();
    }

    void TimelineComponent::setLiveCapture (bool capturing, double startPpq, double lengthPpq)
    {
        liveCapturing = capturing;
        liveStartPpq = startPpq;
        liveLengthPpq = lengthPpq;
    }

    double TimelineComponent::xToPpq (float x) const noexcept { return viewStartPpq + (x - (float) takesLane.getX()) / pxPerPpq; }
    float TimelineComponent::ppqToX (double ppq) const noexcept { return (float) takesLane.getX() + (float) ((ppq - viewStartPpq) * pxPerPpq); }

    void TimelineComponent::resized()
    {
        auto r = getLocalBounds();
        hScroll.setBounds (r.removeFromBottom (12).withTrimmedLeft (gutter));
        ruler = r.removeFromTop (24).withTrimmedLeft (gutter);
        noteLane = r.removeFromBottom (40).withTrimmedLeft (gutter);
        r.removeFromBottom (2);
        chordLane = r.removeFromBottom (28).withTrimmedLeft (gutter);
        r.removeFromBottom (2);
        takesLane = r.withTrimmedLeft (gutter);
        followButton.setBounds (0, 2, gutter, 20);
        updateScrollRange();
    }

    void TimelineComponent::updateScrollRange()
    {
        double end = 16.0;
        for (auto& r : data.regions) end = juce::jmax (end, r.endPpq);
        if (host.hasPpq) end = juce::jmax (end, host.ppqPosition);
        end += 64.0;
        const double visible = takesLane.getWidth() > 0 ? takesLane.getWidth() / pxPerPpq : 16.0;
        hScroll.setRangeLimits (0.0, juce::jmax (end, viewStartPpq + visible), juce::dontSendNotification);
        hScroll.setCurrentRange (viewStartPpq, visible, juce::dontSendNotification);
    }

    void TimelineComponent::scrollBarMoved (juce::ScrollBar*, double start)
    {
        viewStartPpq = juce::jmax (0.0, start);
        repaint();
    }

    void TimelineComponent::mouseWheelMove (const juce::MouseEvent& e, const juce::MouseWheelDetails& w)
    {
        if (e.mods.isCommandDown() || e.mods.isCtrlDown())
        {
            const double anchor = xToPpq ((float) e.x);
            pxPerPpq = juce::jlimit (1.0, 600.0, pxPerPpq * std::pow (1.15, w.deltaY * 8.0));
            viewStartPpq = juce::jmax (0.0, anchor - (e.x - takesLane.getX()) / pxPerPpq);
        }
        else
        {
            const float delta = std::abs (w.deltaX) > std::abs (w.deltaY) ? w.deltaX : w.deltaY;
            viewStartPpq = juce::jmax (0.0, viewStartPpq - delta * 300.0 / pxPerPpq);
        }
        updateScrollRange();
        repaint();
    }

    const TimelineRegion* TimelineComponent::regionAt (juce::Point<int> p) const
    {
        if (! takesLane.contains (p))
            return nullptr;
        const double q = xToPpq ((float) p.x);
        for (auto it = data.regions.rbegin(); it != data.regions.rend(); ++it) // newest on top
            if (q >= it->startPpq && q < it->endPpq)
                return &*it;
        return nullptr;
    }

    void TimelineComponent::mouseDown (const juce::MouseEvent& e)
    {
        dragStartView = viewStartPpq;
        if (! e.mods.isPopupMenu())
            return;

        if (auto* r = regionAt (e.getPosition()))
        {
            const auto id = r->id;
            juce::PopupMenu m;
            m.addSectionHeader ("Take at " + juce::String (r->startPpq, 2) + " PPQ");
            m.addItem (1, "Re-analyse sidechain", r->hasSidechain && ! r->missing);
            m.addItem (2, "Remove take from session");
            m.showMenuAsync (juce::PopupMenu::Options(), [this, id] (int res)
            {
                if (res == 1 && onAnalyseRegion) onAnalyseRegion (id);
                if (res == 2 && onDeleteRegion) onDeleteRegion (id);
            });
        }
    }

    void TimelineComponent::mouseDrag (const juce::MouseEvent& e)
    {
        if (e.mods.isPopupMenu()) return;
        // Drag to scroll (on empty space or the ruler).
        viewStartPpq = juce::jmax (0.0, dragStartView - e.getDistanceFromDragStartX() / pxPerPpq);
        followPlayhead = false;
        followButton.setToggleState (false, juce::dontSendNotification);
        updateScrollRange();
        repaint();
    }

    //==========================================================================
    void TimelineComponent::drawGrid (juce::Graphics& g, juce::Rectangle<int> area, bool labels)
    {
        const int num = host.hasTimeSig ? host.timeSigNumerator : 4;
        const double beatLen = host.hasTimeSig ? host.ppqPerBeat() : 1.0;
        const double barLen = num * beatLen;
        const double barPx = barLen * pxPerPpq;
        const bool drawBeats = beatLen * pxPerPpq >= 6.0;
        int labelEvery = 1;
        while (barPx * labelEvery < 36.0) labelEvery *= 2;

        const double first = std::floor (viewStartPpq / beatLen) * beatLen;
        const double last = xToPpq ((float) area.getRight());
        g.setFont (Theme::font (11.0f));

        for (double q = first; q <= last; q += beatLen)
        {
            const float x = ppqToX (q);
            if (x < (float) area.getX()) continue;
            const double barPos = q / barLen;
            const bool isBar = std::abs (barPos - std::round (barPos)) < 1.0e-6;
            if (! isBar && ! drawBeats) continue;

            g.setColour (isBar ? Theme::gridBar : Theme::gridBeat);
            g.drawVerticalLine ((int) x, (float) area.getY(), (float) area.getBottom());

            const int barNumber = (int) std::llround (barPos) + 1;
            if (labels && isBar && (barNumber - 1) % labelEvery == 0)
            {
                g.setColour (Theme::textDim);
                g.drawText (juce::String (barNumber), (int) x + 3, area.getY(), 40, area.getHeight(), juce::Justification::centredLeft);
            }
        }
    }

    void TimelineComponent::drawRegion (juce::Graphics& g, const TimelineRegion& r)
    {
        const float x0 = ppqToX (r.startPpq), x1 = ppqToX (r.endPpq);
        if (x1 < (float) takesLane.getX() || x0 > (float) takesLane.getRight())
            return;

        auto box = juce::Rectangle<float> (x0, (float) takesLane.getY() + 6.0f, x1 - x0, (float) takesLane.getHeight() - 12.0f);
        const auto clip = box.getIntersection (takesLane.toFloat());

        g.setColour (r.missing ? Theme::bad.withAlpha (0.25f) : Theme::region.withAlpha (0.85f));
        g.fillRoundedRectangle (box, 3.0f);
        g.setColour (r.missing ? Theme::bad : Theme::regionEdge);
        g.drawRoundedRectangle (box, 3.0f, 1.0f);

        if (r.missing || r.peaks == nullptr)
        {
            g.setColour (r.missing ? Theme::bad : Theme::textDim);
            g.setFont (Theme::font (12.0f));
            g.drawText (r.missing ? "Missing audio" : (r.loading ? "Loading..." : "No audio"),
                        clip.reduced (6.0f).toNearestInt(), juce::Justification::topLeft);
            return;
        }

        // Waveform: for each pixel column, the min/max of the peaks it covers.
        const auto& p = *r.peaks;
        const double captureRate = r.tempoMap.getSampleRate();
        const double rateScale = captureRate > 0.0 ? r.renderedRate / captureRate : 1.0;
        const float midY = box.getCentreY();
        const float halfH = box.getHeight() * 0.45f;
        g.setColour (Theme::waveform.withAlpha (0.9f));

        for (int px = (int) std::floor (clip.getX()); px < (int) std::ceil (clip.getRight()); ++px)
        {
            const double s0 = r.tempoMap.ppqToSample (xToPpq ((float) px)) * rateScale;
            const double s1 = r.tempoMap.ppqToSample (xToPpq ((float) px + 1.0f)) * rateScale;
            int i0 = (int) (s0 / p.samplesPerPeak), i1 = (int) (s1 / p.samplesPerPeak);
            i0 = juce::jlimit (0, (int) p.mins.size() - 1, i0);
            i1 = juce::jlimit (i0, (int) p.mins.size() - 1, juce::jmax (i0, i1));
            if (p.mins.empty()) break;
            float lo = 0.0f, hi = 0.0f;
            for (int i = i0; i <= i1; ++i) { lo = juce::jmin (lo, p.mins[(size_t) i]); hi = juce::jmax (hi, p.maxs[(size_t) i]); }
            g.drawVerticalLine (px, midY - hi * halfH, midY - lo * halfH + 1.0f);
        }
    }

    void TimelineComponent::drawChords (juce::Graphics& g)
    {
        g.setFont (Theme::font (12.0f, true));
        for (auto& r : data.regions)
            for (auto& c : r.chords)
            {
                const float x0 = juce::jmax (ppqToX (c.startPpq), (float) chordLane.getX());
                const float x1 = juce::jmin (ppqToX (c.endPpq), (float) chordLane.getRight());
                if (x1 <= x0) continue;
                auto box = juce::Rectangle<float> (x0, (float) chordLane.getY() + 3.0f, x1 - x0, (float) chordLane.getHeight() - 6.0f).reduced (1.0f, 0.0f);
                const bool nc = c.root < 0;
                g.setColour (nc ? Theme::panelLight : Theme::accent.withAlpha (0.25f + 0.5f * juce::jlimit (0.0f, 1.0f, c.confidence)));
                g.fillRoundedRectangle (box, 3.0f);
                if (box.getWidth() > 18.0f)
                {
                    g.setColour (nc ? Theme::textDim : Theme::text);
                    g.drawText (Analysis::chordName (c.root, c.quality), box.reduced (4.0f, 0.0f).toNearestInt(),
                                juce::Justification::centredLeft, true);
                }
            }
    }

    void TimelineComponent::paint (juce::Graphics& g)
    {
        g.fillAll (Theme::panel);

        // Lane backgrounds and labels.
        g.setColour (Theme::background);
        g.fillRect (takesLane);
        g.fillRect (chordLane);
        g.fillRect (noteLane);

        g.setColour (Theme::panelLight);
        g.drawHorizontalLine (chordLane.getY() - 1, 0.0f, (float) getWidth());
        g.drawHorizontalLine (noteLane.getY() - 1, 0.0f, (float) getWidth());

        g.setFont (Theme::font (11.0f));
        g.setColour (Theme::textDim);
        g.drawText ("Takes", 6, takesLane.getY() + 4, gutter - 8, 16, juce::Justification::centredLeft);
        g.drawText ("Chords", 6, chordLane.getY(), gutter - 8, chordLane.getHeight(), juce::Justification::centredLeft);
        g.drawText ("Notes", 6, noteLane.getY(), gutter - 8, noteLane.getHeight(), juce::Justification::centredLeft);

        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (ruler.getUnion (noteLane));
            drawGrid (g, ruler, true);
            drawGrid (g, takesLane, false);
            drawGrid (g, chordLane, false);

            for (auto& r : data.regions)
                drawRegion (g, r);

            if (liveCapturing)
            {
                const float x0 = ppqToX (liveStartPpq), x1 = ppqToX (liveStartPpq + liveLengthPpq);
                g.setColour (Theme::record.withAlpha (0.35f));
                g.fillRect (juce::Rectangle<float> (x0, (float) takesLane.getY() + 6.0f, juce::jmax (1.0f, x1 - x0), (float) takesLane.getHeight() - 12.0f));
                g.setColour (Theme::record);
                g.setFont (Theme::font (12.0f, true));
                g.drawText ("Recording", juce::Rectangle<float> (x0 + 4.0f, (float) takesLane.getY() + 8.0f, 90.0f, 16.0f), juce::Justification::centredLeft);
            }

            drawChords (g);

            // Reserved lane for Stage 2 note blobs.
            g.setColour (Theme::textDim.withAlpha (0.5f));
            g.setFont (Theme::font (11.0f));
            g.drawText ("Note blobs arrive in Stage 2", noteLane.reduced (8, 0), juce::Justification::centredLeft);

            // Playhead.
            if (host.hasPpq)
            {
                const float x = ppqToX (host.ppqPosition);
                g.setColour (Theme::playhead);
                g.drawVerticalLine ((int) x, (float) ruler.getY(), (float) noteLane.getBottom());
            }
        }

        // Key display, top right of the ruler.
        auto keyArea = ruler.withTrimmedLeft (ruler.getWidth() - 220).reduced (6, 2);
        g.setColour (Theme::panel.withAlpha (0.9f));
        g.fillRoundedRectangle (keyArea.toFloat(), 3.0f);
        g.setColour (data.keyIsOverride ? Theme::warn : Theme::text);
        g.setFont (Theme::font (12.0f, true));
        g.drawText ("Key: " + data.keyText + (data.keyIsOverride ? " (manual)" : ""), keyArea.reduced (6, 0), juce::Justification::centredRight);
    }
}
