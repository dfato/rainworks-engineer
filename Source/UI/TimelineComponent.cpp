#include "TimelineComponent.h"
#include "Theme.h"
#include "Engine/Intent/NoteIntent.h"

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
        noteLane = r.removeFromBottom (120).withTrimmedLeft (gutter);
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

    double TimelineComponent::outStart (const TimelineRegion& r, int i) noexcept
    {
        return i < (int) r.noteOutStart.size() ? r.noteOutStart[(size_t) i] : (double) r.notes[(size_t) i].start;
    }

    juce::Rectangle<float> TimelineComponent::noteBounds (const TimelineRegion& r, int i, bool target) const
    {
        const auto& n = r.notes[(size_t) i];
        const auto lane = noteLane.reduced (0, 4).toFloat();
        const float midi = target ? n.detectedMidi + (n.targetMidi - n.detectedMidi) * n.correction : n.detectedMidi;
        const float y = lane.getBottom() - (midi - noteLo) * noteRowH;
        const double s = target ? outStart (r, i) : (double) n.start;
        const float x0 = ppqToX (r.tempoMap.sampleToPpq (s));
        const float x1 = ppqToX (r.tempoMap.sampleToPpq (s + (double) (n.end - n.start)));
        return { x0, y - juce::jmax (noteH, 8.0f) * 0.5f, juce::jmax (4.0f, x1 - x0), juce::jmax (noteH, 8.0f) };
    }

    TimelineComponent::NoteHit TimelineComponent::noteAt (juce::Point<int> p) const
    {
        if (! noteLane.contains (p)) return {};
        for (int ri = (int) data.regions.size() - 1; ri >= 0; --ri)
        {
            const auto& r = data.regions[(size_t) ri];
            for (int ni = 0; ni < (int) r.notes.size(); ++ni)
                if (noteBounds (r, ni, true).contains (p.toFloat()))
                    return { ri, ni };
        }
        return {};
    }

    void TimelineComponent::mouseUp (const juce::MouseEvent&)
    {
        if (dragNote.region >= 0 && noteDragged && onNoteChanged)
        {
            const auto& r = data.regions[(size_t) dragNote.region];
            onNoteChanged (r.id, dragNote.note, r.notes[(size_t) dragNote.note]);
        }
        dragNote = {};
        noteDragged = false;
    }

    void TimelineComponent::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const auto hit = noteAt (e.getPosition());
        if (hit.region < 0) return;
        auto& r = data.regions[(size_t) hit.region];
        auto n = r.notes[(size_t) hit.note];
        n.correction = n.correction > 0.0f ? 0.0f : 1.0f;
        r.notes[(size_t) hit.note] = n;
        repaint();
        if (onNoteChanged) onNoteChanged (r.id, hit.note, n);
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

        if (const auto hit = noteAt (e.getPosition()); hit.region >= 0)
        {
            auto& r = data.regions[(size_t) hit.region];
            const auto n = r.notes[(size_t) hit.note];
            if (e.mods.isPopupMenu())
            {
                juce::PopupMenu m;
                m.addSectionHeader (Analysis::pitchClassName (((int) std::round (n.detectedMidi)) % 12) + "  "
                                    + juce::String (n.centsOff() >= 0 ? "+" : "") + juce::String (juce::roundToInt (n.centsOff())) + " cents from target");
                if (const auto intent = NoteIntent::describe (n.intent); intent.isNotEmpty())
                    m.addSectionHeader ("Sung as: " + intent + " (" + juce::String (juce::roundToInt (n.intentScale * 100.0f)) + "% of Correct)");
                m.addItem (1, "Correct fully (100%)");
                m.addItem (2, "Correct halfway (50%)");
                m.addItem (3, "Leave as sung (0%)");
                m.addItem (4, "Reset target to key");
                m.addItem (5, "Reset timing", n.timeShift != 0);
                m.addItem (7, "Split note here");
                m.addItem (8, "Merge with next note", hit.note + 1 < (int) r.notes.size());
                m.addSeparator();
                m.addItem (6, "Tip: Shift + drag a note to move it in time", false);
                // Click position in sung time: undo this note's timing move.
                const double clickOut = r.tempoMap.ppqToSample (xToPpq ((float) e.x));
                const auto splitAt = (std::int64_t) std::llround (clickOut - (outStart (r, hit.note) - (double) n.start));
                m.showMenuAsync (juce::PopupMenu::Options(), [this, id = r.id, idx = hit.note, n, splitAt] (int res)
                {
                    if (res == 7) { if (onSplitNote) onSplitNote (id, idx, splitAt); return; }
                    if (res == 8) { if (onMergeNote) onMergeNote (id, idx); return; }
                    if (res == 0 || ! onNoteChanged) return;
                    auto edited = n;
                    if (res == 1) edited.correction = 1.0f;
                    if (res == 2) edited.correction = 0.5f;
                    if (res == 3) edited.correction = 0.0f;
                    if (res == 4) edited.targetMidi = std::round (n.detectedMidi), edited.correction = juce::jmax (n.correction, 1.0f);
                    if (res == 5) edited.timeShift = 0;
                    onNoteChanged (id, idx, edited);
                });
                return;
            }
            dragNote = hit;
            dragStartTarget = n.targetMidi;
            dragStartShift = n.timeShift;
            dragStartOut = outStart (r, hit.note);
            dragTiming = e.mods.isShiftDown();
            noteDragged = false;
            return;
        }

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

        if (dragNote.region >= 0)
        {
            auto& r = data.regions[(size_t) dragNote.region];
            auto& n = r.notes[(size_t) dragNote.note];
            if (dragTiming)
            {
                // Shift + horizontal drag nudges timing (snaps to 1/16 unless Alt is held).
                const double startPpq = r.tempoMap.sampleToPpq (dragStartOut);
                double ppq = startPpq + e.getDistanceFromDragStartX() / pxPerPpq;
                if (! e.mods.isAltDown())
                    ppq = TimeWarp::nearestGridPpq (ppq, 16, 0.0f);
                const double delta = r.tempoMap.ppqToSample (ppq) - dragStartOut;
                n.timeShift = dragStartShift + (std::int64_t) std::llround (delta);
                if (dragNote.note < (int) r.noteOutStart.size())
                    r.noteOutStart[(size_t) dragNote.note] = dragStartOut + delta;
                noteDragged = true;
                repaint();
                return;
            }
            // Vertical drag moves the target in semitones (Alt for fine/cents).
            const float steps = (float) -e.getDistanceFromDragStartY() / juce::jmax (1.0f, noteRowH);
            n.targetMidi = e.mods.isAltDown() ? dragStartTarget + steps : std::round (dragStartTarget + steps);
            n.correction = 1.0f;
            noteDragged = true;
            repaint();
            return;
        }
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

    void TimelineComponent::drawNotes (juce::Graphics& g)
    {
        // Pitch range over all notes so blobs fill the lane.
        float lo = 1000.0f, hi = -1000.0f;
        bool any = false, tracking = false;
        for (auto& r : data.regions)
        {
            tracking = tracking || r.trackingPitch;
            for (auto& n : r.notes) { lo = juce::jmin (lo, n.detectedMidi, n.targetMidi); hi = juce::jmax (hi, n.detectedMidi, n.targetMidi); any = true; }
        }
        if (! any)
        {
            g.setColour (Theme::textDim.withAlpha (0.6f));
            g.setFont (Theme::font (11.0f));
            g.drawText (tracking ? "Tracking pitch..." : "Notes appear here after a take is analysed",
                        noteLane.reduced (8, 0), juce::Justification::centredLeft);
            return;
        }
        lo = std::floor (lo) - 1.0f;
        hi = std::ceil (hi) + 1.0f;
        const auto lane = noteLane.reduced (0, 4).toFloat();
        const float rowH = lane.getHeight() / juce::jmax (1.0f, hi - lo);
        if (dragNote.region < 0) { noteLo = lo; noteRowH = rowH; } // keep the scale stable while dragging
        auto yFor = [&] (float midi) { return lane.getBottom() - (midi - noteLo) * noteRowH; };

        // Octave C lines with labels.
        g.setFont (Theme::font (10.0f));
        for (int m = (int) lo; m <= (int) hi; ++m)
        {
            if (m % 12 != 0) continue;
            const float y = yFor ((float) m);
            g.setColour (Theme::gridBeat);
            g.drawHorizontalLine ((int) y, lane.getX(), lane.getRight());
            g.setColour (Theme::textDim);
            g.drawText ("C" + juce::String (m / 12 - 1), (int) lane.getX() + 2, (int) y - 12, 30, 12, juce::Justification::bottomLeft);
        }

        const float h = juce::jlimit (3.0f, 12.0f, noteRowH * 0.8f);
        noteH = h;
        for (auto& r : data.regions)
            for (int ni = 0; ni < (int) r.notes.size(); ++ni)
            {
                const auto& n = r.notes[(size_t) ni];
                const double os = outStart (r, ni);
                const float x0 = ppqToX (r.tempoMap.sampleToPpq (os));
                const float x1 = ppqToX (r.tempoMap.sampleToPpq (os + (double) (n.end - n.start)));
                if (x1 < lane.getX() || x0 > lane.getRight()) continue;

                // Moved in time: faint outline where it was sung.
                if (std::abs (os - (double) n.start) > 0.002 * r.tempoMap.getSampleRate())
                {
                    const float g0 = ppqToX (r.tempoMap.sampleToPpq ((double) n.start));
                    const float g1 = ppqToX (r.tempoMap.sampleToPpq ((double) n.end));
                    g.setColour (Theme::textDim.withAlpha (0.4f));
                    g.drawRoundedRectangle (juce::Rectangle<float> (g0, yFor (n.detectedMidi) - h * 0.5f, juce::jmax (2.0f, g1 - g0), h), h * 0.4f, 1.0f);
                }

                const float cents = std::abs (n.centsOff());
                const auto colour = cents < 15.0f ? Theme::good : (cents < 35.0f ? Theme::warn : Theme::bad);

                // Target pitch as an outline, detected pitch as the filled blob.
                if (std::abs (n.targetMidi - n.detectedMidi) > 0.05f)
                {
                    g.setColour (Theme::text.withAlpha (0.35f));
                    g.drawRoundedRectangle (juce::Rectangle<float> (x0, yFor (n.targetMidi) - h * 0.5f, juce::jmax (2.0f, x1 - x0), h), h * 0.4f, 1.0f);
                }
                // Corrected notes are drawn at their target (where they will sound), with a
                // faint ghost where they were sung.
                const bool corrected = n.correction > 0.0f;
                const float shown = n.detectedMidi + (n.targetMidi - n.detectedMidi) * n.correction;
                if (corrected)
                {
                    g.setColour (colour.withAlpha (0.25f));
                    g.fillRoundedRectangle (juce::Rectangle<float> (x0, yFor (n.detectedMidi) - h * 0.5f, juce::jmax (2.0f, x1 - x0), h), h * 0.4f);
                }
                g.setColour ((corrected ? Theme::accent : colour).withAlpha (0.9f));
                g.fillRoundedRectangle (juce::Rectangle<float> (x0, yFor (shown) - h * 0.5f, juce::jmax (2.0f, x1 - x0), h), h * 0.4f);
                if (n.userEdited)
                {
                    g.setColour (Theme::text);
                    g.drawRoundedRectangle (juce::Rectangle<float> (x0, yFor (shown) - h * 0.5f, juce::jmax (2.0f, x1 - x0), h), h * 0.4f, 1.0f);
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

            drawGrid (g, noteLane, false);
            drawNotes (g);

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
