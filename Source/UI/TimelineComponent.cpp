#include "TimelineComponent.h"
#include "Theme.h"
#include "Engine/Intent/NoteIntent.h"

namespace ve::ui
{
    TimelineComponent::TimelineComponent()
    {
        setWantsKeyboardFocus (true);
        hScroll.setAutoHide (false);
        hScroll.addListener (this);
        addAndMakeVisible (hScroll);

        followButton.setToggleState (true, juce::dontSendNotification);
        followButton.setColour (juce::ToggleButton::textColourId, Theme::textDim);
        followButton.onClick = [this] { followPlayhead = followButton.getToggleState(); };
        addAndMakeVisible (followButton);

        // ---- note graph tool row ----
        snapButton.setToggleState (true, juce::dontSendNotification);
        snapButton.setColour (juce::ToggleButton::textColourId, Theme::text);
        snapButton.setTooltip ("On: dragged and moved notes land on the notes of the selected key and scale. "
                               "Off: they move in semitones. Hold Alt while dragging for free (cent) moves.");
        snapButton.onClick = [this]
        {
            data.snapToScale = snapButton.getToggleState();
            if (onSnapToScaleChanged) onSnapToScaleChanged (data.snapToScale);
            repaint();
        };
        addAndMakeVisible (snapButton);

        undoButton.setTooltip ("Undo the last note edit (Ctrl+Z)");
        redoButton.setTooltip ("Redo (Ctrl+Shift+Z or Ctrl+Y)");
        undoButton.onClick = [this] { if (onUndo) onUndo(); };
        redoButton.onClick = [this] { if (onRedo) onRedo(); };
        addAndMakeVisible (undoButton);
        addAndMakeVisible (redoButton);

        selectionLabel.setColour (juce::Label::textColourId, Theme::textDim);
        selectionLabel.setFont (Theme::font (11.0f));
        addAndMakeVisible (selectionLabel);

        auto setupSlider = [this] (juce::Label& label, juce::Slider& slider, double lo, double hi, const juce::String& tip)
        {
            label.setColour (juce::Label::textColourId, Theme::textDim);
            label.setFont (Theme::font (11.0f));
            label.setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (label);
            slider.setRange (lo, hi, 1.0);
            slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 18);
            slider.setTooltip (tip);
            slider.setEnabled (false);
            addAndMakeVisible (slider);
        };
        setupSlider (correctLabel, correctSlider, 0.0, 100.0,
                     "How far the selected notes are pulled to their target (0% = as sung, 100% = on pitch)");
        setupSlider (vibratoLabel, vibratoSlider, 0.0, 200.0,
                     "Vibrato kept on the selected notes: 0% = a flat line, 100% = as the style decides, 200% = double");
        setupSlider (driftLabel, driftSlider, -10.0, 100.0,
                     "Slow pitch wander kept inside the selected notes: 0% = held dead steady, 100% = as sung. Far left = automatic (set by Correct)");
        correctSlider.setTextValueSuffix ("%");
        vibratoSlider.setTextValueSuffix ("%");
        driftSlider.textFromValueFunction = [] (double v) { return v < 0.0 ? juce::String ("Auto") : juce::String (juce::roundToInt (v)) + "%"; };
        driftSlider.valueFromTextFunction = [] (const juce::String& t) { return t.trim().startsWithIgnoreCase ("a") ? -10.0 : (double) t.getIntValue(); };

        auto hookSlider = [this] (juce::Slider& slider, std::function<void (Note&, double)> apply)
        {
            slider.onDragStart = [this] { sliderEditing = true; };
            slider.onValueChange = [this, &slider, apply]
            {
                if (selection.empty()) return;
                const double v = slider.getValue();
                const bool live = slider.isMouseButtonDown();
                editSelection ([&] (Note& n, TimelineRegion&, int) { apply (n, v); }, ! live);
            };
            slider.onDragEnd = [this, &slider, apply]
            {
                sliderEditing = false;
                const double v = slider.getValue();
                editSelection ([&] (Note& n, TimelineRegion&, int) { apply (n, v); }, true);
            };
        };
        hookSlider (correctSlider, [] (Note& n, double v) { n.correction = (float) (v / 100.0); });
        hookSlider (vibratoSlider, [] (Note& n, double v) { n.vibrato = (float) (v / 100.0); });
        hookSlider (driftSlider, [] (Note& n, double v) { n.drift = v < 0.0 ? -1.0f : (float) (v / 100.0); });
        refreshTools();
    }

    void TimelineComponent::setData (TimelineData d)
    {
        const auto old = std::move (data);
        data = std::move (d);
        snapButton.setToggleState (data.snapToScale, juce::dontSendNotification);
        pruneSelection (old);
        updateScrollRange();
        refreshTools();
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

    //==========================================================================
    // Selection

    TimelineComponent::NoteKey TimelineComponent::keyFor (NoteHit h) const
    {
        return { data.regions[(size_t) h.region].id, h.note };
    }

    TimelineComponent::NoteHit TimelineComponent::hitFor (const NoteKey& k) const
    {
        for (int ri = 0; ri < (int) data.regions.size(); ++ri)
            if (data.regions[(size_t) ri].id == k.first)
                return k.second >= 0 && k.second < (int) data.regions[(size_t) ri].notes.size() ? NoteHit { ri, k.second } : NoteHit {};
        return {};
    }

    bool TimelineComponent::isSelected (int region, int note) const
    {
        return ! selection.empty() && selection.count ({ data.regions[(size_t) region].id, note }) > 0;
    }

    void TimelineComponent::selectOnly (NoteHit h)
    {
        selection.clear();
        if (h.region >= 0) selection.insert (keyFor (h));
        refreshTools();
        repaint();
    }

    void TimelineComponent::selectAll()
    {
        selection.clear();
        for (auto& r : data.regions)
            for (int ni = 0; ni < (int) r.notes.size(); ++ni)
                selection.insert ({ r.id, ni });
        refreshTools();
        repaint();
    }

    void TimelineComponent::pruneSelection (const TimelineData& old)
    {
        // A split or merge renumbers the notes of a take: drop its selection.
        std::map<juce::String, size_t> oldCounts;
        for (auto& r : old.regions) oldCounts[r.id] = r.notes.size();
        for (auto it = selection.begin(); it != selection.end();)
        {
            const auto h = hitFor (*it);
            const auto oc = oldCounts.find (it->first);
            const bool renumbered = h.region >= 0 && oc != oldCounts.end() && oc->second != data.regions[(size_t) h.region].notes.size();
            it = (h.region < 0 || renumbered) ? selection.erase (it) : std::next (it);
        }
    }

    std::vector<TimelineComponent::NoteHit> TimelineComponent::selectedHits() const
    {
        std::vector<NoteHit> out;
        for (auto& k : selection)
            if (const auto h = hitFor (k); h.region >= 0)
                out.push_back (h);
        return out;
    }

    void TimelineComponent::editSelection (const std::function<void (Note&, TimelineRegion&, int)>& fn, bool commit)
    {
        for (auto h : selectedHits())
        {
            auto& r = data.regions[(size_t) h.region];
            fn (r.notes[(size_t) h.note], r, h.note);
        }
        repaint();
        if (commit) commitSelection();
    }

    void TimelineComponent::commitSelection()
    {
        if (selection.empty() || ! onNotesChanged) return;
        std::vector<TimelineNoteEdit> edits;
        for (auto h : selectedHits())
        {
            const auto& r = data.regions[(size_t) h.region];
            edits.push_back ({ r.id, h.note, r.notes[(size_t) h.note] });
        }
        onNotesChanged (edits);
    }

    float TimelineComponent::snapPitch (float midi, bool fine) const
    {
        if (fine) return midi;
        return data.snapToScale ? NoteSegmenter::snapToKey (midi, data.scaleKey) : std::round (midi);
    }

    void TimelineComponent::transposeSelection (int steps, bool octave, bool fine)
    {
        editSelection ([&] (Note& n, TimelineRegion&, int)
        {
            if (octave)      n.targetMidi += 12.0f * (float) steps;
            else if (fine)   n.targetMidi += 0.05f * (float) steps;   // 5 cents
            else if (data.snapToScale) n.targetMidi = NoteSegmenter::stepInScale (n.targetMidi, data.scaleKey, steps);
            else             n.targetMidi = std::round (n.targetMidi) + (float) steps;
            n.correction = juce::jmax (n.correction, 1.0f);
        });
    }

    void TimelineComponent::nudgeSelectionTime (int direction)
    {
        // One 1/16 note per press, measured with each take's own tempo map.
        editSelection ([&] (Note& n, TimelineRegion& r, int idx)
        {
            const double out = outStart (r, idx);
            const double ppq = r.tempoMap.sampleToPpq (out) + 0.25 * direction;
            const double delta = r.tempoMap.ppqToSample (ppq) - out;
            n.timeShift += (std::int64_t) std::llround (delta);
            if (idx < (int) r.noteOutStart.size())
                r.noteOutStart[(size_t) idx] = out + delta;
        });
    }

    void TimelineComponent::refreshTools()
    {
        const auto hits = selectedHits();
        const bool any = ! hits.empty();
        for (auto* sl : { &correctSlider, &vibratoSlider, &driftSlider })
            sl->setEnabled (any);
        undoButton.setEnabled (data.canUndo);
        redoButton.setEnabled (data.canRedo);

        juce::String text;
        if (any)
            text << (int) hits.size() << (hits.size() == 1 ? " note" : " notes") << " selected";
        else
            text << "Click or box-select notes";
        selectionLabel.setText (text, juce::dontSendNotification);

        if (any && ! sliderEditing)
        {
            const auto& n = data.regions[(size_t) hits.front().region].notes[(size_t) hits.front().note];
            correctSlider.setValue (std::round (n.correction * 100.0f), juce::dontSendNotification);
            vibratoSlider.setValue (std::round (n.vibrato * 100.0f), juce::dontSendNotification);
            driftSlider.setValue (n.hasDriftOverride() ? std::round (n.drift * 100.0f) : -10.0, juce::dontSendNotification);
        }
    }

    double TimelineComponent::xToPpq (float x) const noexcept { return viewStartPpq + (x - (float) takesLane.getX()) / pxPerPpq; }
    float TimelineComponent::ppqToX (double ppq) const noexcept { return (float) takesLane.getX() + (float) ((ppq - viewStartPpq) * pxPerPpq); }

    void TimelineComponent::resized()
    {
        auto r = getLocalBounds();
        hScroll.setBounds (r.removeFromBottom (12).withTrimmedLeft (gutter));
        ruler = r.removeFromTop (24).withTrimmedLeft (gutter);
        // The note graph gets half the room (at least 140 px): it is where the editing happens.
        const int graphH = juce::jmax (140, (r.getHeight() - 26 - 28 - 4) / 2);
        noteLane = r.removeFromBottom (graphH).withTrimmedLeft (gutter);
        noteTools = r.removeFromBottom (26);
        r.removeFromBottom (2);
        chordLane = r.removeFromBottom (28).withTrimmedLeft (gutter);
        r.removeFromBottom (2);
        takesLane = r.withTrimmedLeft (gutter);
        followButton.setBounds (0, 2, gutter, 20);

        auto t = noteTools.reduced (4, 3);
        snapButton.setBounds (t.removeFromLeft (112));
        t.removeFromLeft (6);
        undoButton.setBounds (t.removeFromLeft (48));
        t.removeFromLeft (4);
        redoButton.setBounds (t.removeFromLeft (48));
        t.removeFromLeft (10);
        selectionLabel.setBounds (t.removeFromLeft (150));
        auto slot = [&t] (juce::Label& l, juce::Slider& s)
        {
            t.removeFromLeft (6);
            l.setBounds (t.removeFromLeft (50));
            s.setBounds (t.removeFromLeft (170));
        };
        slot (correctLabel, correctSlider);
        slot (vibratoLabel, vibratoSlider);
        slot (driftLabel, driftSlider);
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
        if (marqueeActive)
        {
            marqueeActive = false;
            refreshTools();
            repaint();
            return;
        }
        if (dragNote.region >= 0)
        {
            if (noteDragged)
                commitSelection();
            else if (pendingDeselect)
                selection.erase (keyFor (dragNote));
            else if (pendingSelectOnly)
                selection = { keyFor (dragNote) };
            refreshTools();
            repaint();
        }
        dragNote = {};
        dragStarts.clear();
        noteDragged = pendingSelectOnly = pendingDeselect = false;
    }

    void TimelineComponent::mouseDoubleClick (const juce::MouseEvent& e)
    {
        const auto hit = noteAt (e.getPosition());
        if (hit.region < 0) return;
        if (! isSelected (hit.region, hit.note))
            selectOnly (hit);
        // Toggle the clicked note between corrected and as sung; the selection follows it.
        const bool on = data.regions[(size_t) hit.region].notes[(size_t) hit.note].correction <= 0.0f;
        editSelection ([on] (Note& n, TimelineRegion&, int) { n.correction = on ? 1.0f : 0.0f; });
        refreshTools();
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

    void TimelineComponent::showNoteMenu (NoteHit hit, std::int64_t splitAt)
    {
        const auto& r = data.regions[(size_t) hit.region];
        const auto n = r.notes[(size_t) hit.note];
        const int count = (int) selection.size();
        const juce::String what = count > 1 ? juce::String (count) + " selected notes" : juce::String ("this note");

        juce::PopupMenu m;
        if (count > 1)
            m.addSectionHeader (juce::String (count) + " notes selected");
        else
        {
            m.addSectionHeader (Analysis::pitchClassName (((int) std::round (n.detectedMidi)) % 12) + "  "
                                + juce::String (n.centsOff() >= 0 ? "+" : "") + juce::String (juce::roundToInt (n.centsOff())) + " cents from target");
            if (const auto intent = NoteIntent::describe (n.intent); intent.isNotEmpty())
                m.addSectionHeader ("Sung as: " + intent + " (" + juce::String (juce::roundToInt (n.intentScale * 100.0f)) + "% of Correct)");
        }
        m.addItem (1, "Correct fully (100%)");
        m.addItem (2, "Correct halfway (50%)");
        m.addItem (3, "Leave as sung (0%)");
        m.addItem (9, "Snap to scale (" + data.scaleText + ")", data.scaleKey.isValid());
        m.addItem (4, "Reset target to key");
        m.addItem (5, "Reset timing");
        juce::PopupMenu vib;
        vib.addItem (20, "Flat (0%)");
        vib.addItem (21, "Half (50%)");
        vib.addItem (22, "As the style decides (100%)");
        vib.addItem (23, "More (150%)");
        vib.addItem (24, "Double (200%)");
        m.addSubMenu ("Vibrato", vib);
        juce::PopupMenu dr;
        dr.addItem (30, "Hold steady (0%)");
        dr.addItem (31, "Half (50%)");
        dr.addItem (32, "As sung (100%)");
        dr.addItem (33, "Automatic");
        m.addSubMenu ("Pitch drift", dr);
        m.addSeparator();
        m.addItem (7, "Split note here", count <= 1);
        m.addItem (8, "Merge with next note", count <= 1 && hit.note + 1 < (int) r.notes.size());
        m.addSeparator();
        m.addItem (10, "Select all notes  (Ctrl+A)");
        m.addItem (11, "Select all notes at this pitch");
        m.addSeparator();
        m.addItem (6, "Tip: Shift + drag moves " + what + " in time; arrows move pitch/time", false);

        m.showMenuAsync (juce::PopupMenu::Options(), [this, id = r.id, idx = hit.note, splitAt] (int res)
        {
            if (res == 0) return;
            if (res == 7) { if (onSplitNote) onSplitNote (id, idx, splitAt); return; }
            if (res == 8) { if (onMergeNote) onMergeNote (id, idx); return; }
            if (res == 10) { selectAll(); return; }
            if (res == 11)
            {
                const auto h = hitFor ({ id, idx });
                if (h.region < 0) return;
                const int pitch = (int) std::lround (data.regions[(size_t) h.region].notes[(size_t) h.note].targetMidi);
                selection.clear();
                for (auto& rr : data.regions)
                    for (int ni = 0; ni < (int) rr.notes.size(); ++ni)
                        if ((int) std::lround (rr.notes[(size_t) ni].targetMidi) == pitch)
                            selection.insert ({ rr.id, ni });
                refreshTools();
                repaint();
                return;
            }
            editSelection ([this, res] (Note& nt, TimelineRegion&, int)
            {
                switch (res)
                {
                    case 1: nt.correction = 1.0f; break;
                    case 2: nt.correction = 0.5f; break;
                    case 3: nt.correction = 0.0f; break;
                    case 4: nt.targetMidi = NoteSegmenter::snapToKey (nt.detectedMidi, data.scaleKey); nt.correction = juce::jmax (nt.correction, 1.0f); break;
                    case 5: nt.timeShift = 0; break;
                    case 9: nt.targetMidi = NoteSegmenter::snapToKey (nt.targetMidi, data.scaleKey); nt.correction = juce::jmax (nt.correction, 1.0f); break;
                    case 20: nt.vibrato = 0.0f; break;
                    case 21: nt.vibrato = 0.5f; break;
                    case 22: nt.vibrato = 1.0f; break;
                    case 23: nt.vibrato = 1.5f; break;
                    case 24: nt.vibrato = 2.0f; break;
                    case 30: nt.drift = 0.0f; break;
                    case 31: nt.drift = 0.5f; break;
                    case 32: nt.drift = 1.0f; break;
                    case 33: nt.drift = -1.0f; break;
                    default: break;
                }
            });
            refreshTools();
        });
    }

    void TimelineComponent::showLaneMenu()
    {
        juce::PopupMenu m;
        m.addItem (1, "Select all notes  (Ctrl+A)");
        m.addItem (2, "Clear selection  (Esc)", ! selection.empty());
        m.addSeparator();
        m.addItem (3, "Undo  (Ctrl+Z)", data.canUndo);
        m.addItem (4, "Redo  (Ctrl+Shift+Z)", data.canRedo);
        m.addSeparator();
        m.addItem (5, "Snap to scale", true, data.snapToScale);
        m.showMenuAsync (juce::PopupMenu::Options(), [this] (int res)
        {
            if (res == 1) selectAll();
            if (res == 2) selectOnly ({});
            if (res == 3 && onUndo) onUndo();
            if (res == 4 && onRedo) onRedo();
            if (res == 5) snapButton.setToggleState (! data.snapToScale, juce::sendNotification);
        });
    }

    void TimelineComponent::mouseDown (const juce::MouseEvent& e)
    {
        dragStartView = viewStartPpq;
        if (noteLane.contains (e.getPosition()))
            grabKeyboardFocus();

        if (const auto hit = noteAt (e.getPosition()); hit.region >= 0)
        {
            auto& r = data.regions[(size_t) hit.region];
            const auto key = keyFor (hit);
            const bool wasSelected = selection.count (key) > 0;
            const bool toggle = e.mods.isCommandDown() || e.mods.isCtrlDown();

            if (e.mods.isPopupMenu())
            {
                if (! wasSelected) selectOnly (hit);
                // Click position in sung time: undo this note's timing move.
                const auto& n = r.notes[(size_t) hit.note];
                const double clickOut = r.tempoMap.ppqToSample (xToPpq ((float) e.x));
                const auto splitAt = (std::int64_t) std::llround (clickOut - (outStart (r, hit.note) - (double) n.start));
                showNoteMenu (hit, splitAt);
                return;
            }

            // Ctrl/Cmd-click toggles; Shift-click adds (and Shift + drag moves in time);
            // a plain click on an unselected note selects just that note.
            pendingSelectOnly = pendingDeselect = false;
            if (toggle || e.mods.isShiftDown())
            {
                if (wasSelected) pendingDeselect = true;    // removed on mouse up if not dragged
                else selection.insert (key);
            }
            else if (! wasSelected)
                selection = { key };
            else
                pendingSelectOnly = true;                   // click (no drag) inside a group: just this one

            dragNote = hit;
            dragTiming = e.mods.isShiftDown();
            noteDragged = false;
            dragStarts.clear();
            for (auto h : selectedHits())
            {
                const auto& rr = data.regions[(size_t) h.region];
                dragStarts[keyFor (h)] = { rr.notes[(size_t) h.note].targetMidi, rr.notes[(size_t) h.note].timeShift, outStart (rr, h.note) };
            }
            dragStartTarget = r.notes[(size_t) hit.note].targetMidi;
            dragStartOut = outStart (r, hit.note);
            refreshTools();
            repaint();
            return;
        }

        if (noteLane.contains (e.getPosition()))
        {
            if (e.mods.isPopupMenu()) { showLaneMenu(); return; }
            // Box selection on empty graph space. Shift/Ctrl add to what is selected.
            marqueeActive = true;
            marquee = juce::Rectangle<float> (e.position, e.position);
            const bool add = e.mods.isShiftDown() || e.mods.isCommandDown() || e.mods.isCtrlDown();
            if (! add) selection.clear();
            marqueeBase = selection;
            refreshTools();
            repaint();
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

        if (marqueeActive)
        {
            marquee = juce::Rectangle<float> (e.mouseDownPosition, e.position);
            selection = marqueeBase;
            for (int ri = 0; ri < (int) data.regions.size(); ++ri)
                for (int ni = 0; ni < (int) data.regions[(size_t) ri].notes.size(); ++ni)
                    if (noteBounds (data.regions[(size_t) ri], ni, true).intersects (marquee))
                        selection.insert ({ data.regions[(size_t) ri].id, ni });
            refreshTools();
            repaint();
            return;
        }

        if (dragNote.region >= 0)
        {
            if (! noteDragged && e.getDistanceFromDragStart() < 3) return;   // a click, not a drag yet
            if (pendingDeselect) return;                                     // Ctrl/Shift-click on a selected note
            noteDragged = true;
            pendingSelectOnly = false;
            auto& anchorRegion = data.regions[(size_t) dragNote.region];

            if (dragTiming)
            {
                // Shift + horizontal drag moves the selection in time. The dragged note snaps
                // to 1/16 (unless Alt is held); the others move by the same musical amount.
                const double anchorPpq = anchorRegion.tempoMap.sampleToPpq (dragStartOut);
                double ppq = anchorPpq + e.getDistanceFromDragStartX() / pxPerPpq;
                if (! e.mods.isAltDown())
                    ppq = TimeWarp::nearestGridPpq (ppq, 16, 0.0f);
                const double deltaPpq = ppq - anchorPpq;
                for (auto h : selectedHits())
                {
                    auto& r = data.regions[(size_t) h.region];
                    const auto it = dragStarts.find (keyFor (h));
                    if (it == dragStarts.end()) continue;
                    const auto& st = it->second;
                    const double delta = r.tempoMap.ppqToSample (r.tempoMap.sampleToPpq (st.out) + deltaPpq) - st.out;
                    r.notes[(size_t) h.note].timeShift = st.shift + (std::int64_t) std::llround (delta);
                    if (h.note < (int) r.noteOutStart.size())
                        r.noteOutStart[(size_t) h.note] = st.out + delta;
                }
                repaint();
                return;
            }

            // Vertical drag moves the selection in pitch. The dragged note lands on the scale
            // (Snap to scale) or a semitone; the others move by the same amount and land on
            // the scale too. Alt moves freely in cents.
            const bool fine = e.mods.isAltDown();
            const float steps = (float) -e.getDistanceFromDragStartY() / juce::jmax (1.0f, noteRowH);
            const float delta = snapPitch (dragStartTarget + steps, fine) - dragStartTarget;
            for (auto h : selectedHits())
            {
                const auto it = dragStarts.find (keyFor (h));
                if (it == dragStarts.end()) continue;
                auto& n = data.regions[(size_t) h.region].notes[(size_t) h.note];
                n.targetMidi = h.region == dragNote.region && h.note == dragNote.note
                                   ? dragStartTarget + delta
                                   : snapPitch (it->second.target + delta, fine);
                n.correction = juce::jmax (n.correction, 1.0f);
            }
            repaint();
            return;
        }
        // Drag to scroll (on the ruler, takes or chords).
        viewStartPpq = juce::jmax (0.0, dragStartView - e.getDistanceFromDragStartX() / pxPerPpq);
        followPlayhead = false;
        followButton.setToggleState (false, juce::dontSendNotification);
        updateScrollRange();
        repaint();
    }

    bool TimelineComponent::keyPressed (const juce::KeyPress& k)
    {
        const auto mods = k.getModifiers();
        const bool cmd = mods.isCommandDown() || mods.isCtrlDown();
        const int code = k.getKeyCode();

        if (cmd && (code == 'Z' || code == 'z'))
        {
            if (mods.isShiftDown()) { if (onRedo) onRedo(); }
            else if (onUndo) onUndo();
            return true;
        }
        if (cmd && (code == 'Y' || code == 'y')) { if (onRedo) onRedo(); return true; }
        if (cmd && (code == 'A' || code == 'a')) { selectAll(); return true; }
        if (code == juce::KeyPress::escapeKey && ! selection.empty()) { selectOnly ({}); return true; }

        if (selection.empty()) return false;

        if (code == juce::KeyPress::upKey || code == juce::KeyPress::downKey)
        {
            transposeSelection (code == juce::KeyPress::upKey ? 1 : -1, mods.isShiftDown(), mods.isAltDown());
            return true;
        }
        if (code == juce::KeyPress::leftKey || code == juce::KeyPress::rightKey)
        {
            nudgeSelectionTime (code == juce::KeyPress::rightKey ? 1 : -1);
            return true;
        }
        if (! cmd && (code == 'S' || code == 's'))
        {
            editSelection ([this] (Note& n, TimelineRegion&, int)
            {
                n.targetMidi = NoteSegmenter::snapToKey (n.targetMidi, data.scaleKey);
                n.correction = juce::jmax (n.correction, 1.0f);
            });
            refreshTools();
            return true;
        }
        if (code == juce::KeyPress::deleteKey || code == juce::KeyPress::backspaceKey)
        {
            editSelection ([] (Note& n, TimelineRegion&, int) { n.correction = 0.0f; });
            refreshTools();
            return true;
        }
        if (! cmd && code == juce::KeyPress::returnKey)
        {
            editSelection ([] (Note& n, TimelineRegion&, int) { n.correction = 1.0f; });
            refreshTools();
            return true;
        }
        return false;
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

        // Scale rows: with Snap to scale on, the notes of the key are shaded so you can see
        // where a dragged note will land.
        if (data.snapToScale && data.scaleKey.isValid() && noteRowH >= 3.0f)
        {
            g.setColour (Theme::panelLight.withAlpha (0.35f));
            for (int m = (int) std::floor (noteLo); m <= (int) std::ceil (noteLo + lane.getHeight() / noteRowH); ++m)
                if (NoteSegmenter::isInScale (m, data.scaleKey))
                {
                    const float y = yFor ((float) m);
                    const bool tonic = ((m - data.scaleKey.tonic) % 12 + 12) % 12 == 0;
                    g.setColour (Theme::panelLight.withAlpha (tonic ? 0.6f : 0.3f));
                    g.fillRect (juce::Rectangle<float> (lane.getX(), y - noteRowH * 0.5f, lane.getWidth(), noteRowH).getIntersection (lane));
                }
        }

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
        for (int ri = 0; ri < (int) data.regions.size(); ++ri)
            for (int ni = 0; ni < (int) data.regions[(size_t) ri].notes.size(); ++ni)
            {
                const auto& r = data.regions[(size_t) ri];
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
                const auto blob = juce::Rectangle<float> (x0, yFor (shown) - h * 0.5f, juce::jmax (2.0f, x1 - x0), h);
                if (isSelected (ri, ni))
                {
                    g.setColour (juce::Colours::white.withAlpha (0.35f));
                    g.fillRoundedRectangle (blob, h * 0.4f);
                    g.setColour (juce::Colours::white);
                    g.drawRoundedRectangle (blob.expanded (1.5f), h * 0.4f + 1.0f, 2.0f);
                }
                else if (n.userEdited)
                {
                    g.setColour (Theme::text);
                    g.drawRoundedRectangle (blob, h * 0.4f, 1.0f);
                }
                // Small marks for per-note vibrato / drift settings.
                if ((std::abs (n.vibrato - 1.0f) > 0.01f || n.hasDriftOverride()) && blob.getWidth() > 14.0f)
                {
                    g.setColour (Theme::text.withAlpha (0.8f));
                    g.setFont (Theme::font (9.0f));
                    juce::String tag;
                    if (std::abs (n.vibrato - 1.0f) > 0.01f) tag << "v" << juce::roundToInt (n.vibrato * 100.0f);
                    if (n.hasDriftOverride()) tag << (tag.isEmpty() ? "" : " ") << "d" << juce::roundToInt (n.drift * 100.0f);
                    g.drawText (tag, juce::Rectangle<float> (x0, blob.getY() - 11.0f, 80.0f, 10.0f), juce::Justification::bottomLeft, false);
                }
            }

        if (marqueeActive)
        {
            g.setColour (Theme::accent.withAlpha (0.15f));
            g.fillRect (marquee);
            g.setColour (Theme::accent);
            g.drawRect (marquee, 1.0f);
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

        // Note graph tool row.
        g.setColour (Theme::panelLight.withAlpha (0.4f));
        g.fillRect (noteTools);
        if (data.scaleText.isNotEmpty())
        {
            g.setColour (data.snapToScale ? Theme::text : Theme::textDim);
            g.drawText ("Scale: " + data.scaleText, noteTools.reduced (8, 0), juce::Justification::centredRight);
        }

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
