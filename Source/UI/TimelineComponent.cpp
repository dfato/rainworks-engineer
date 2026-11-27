#include "TimelineComponent.h"
#include "Theme.h"
#include "EngineerLookAndFeel.h"
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
        followButton.setColour (juce::ToggleButton::textColourId, Theme::text);
        followButton.onClick = [this] { followPlayhead = followButton.getToggleState(); };
        addAndMakeVisible (followButton);

        // ---- note graph tool row ----
        snapButton.setToggleState (true, juce::dontSendNotification);
        snapButton.getProperties().set ("led", true);
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

        selectionLabel.setColour (juce::Label::textColourId, Theme::inkDim);
        selectionLabel.setFont (Theme::font (12.0f));
        addAndMakeVisible (selectionLabel);

        auto setupSlider = [this] (juce::Label& label, juce::Slider& slider, double lo, double hi, const juce::String& tip)
        {
            label.setColour (juce::Label::textColourId, Theme::ink);
            label.setFont (Theme::font (12.5f));
            label.setJustificationType (juce::Justification::centredLeft);
            label.setBorderSize ({ 0, 6, 0, 0 });
            label.setInterceptsMouseClicks (false, false);
            addAndMakeVisible (label);
            slider.setRange (lo, hi, 1.0);
            slider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 50, 22);
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
        // Two glass screens on the metal: takes (ruler + waveform) on top, the note graph
        // below. Between them, on the metal: the chord strip and the note tool row.
        auto r = getLocalBounds();
        const int chordH = 30, toolsH = 38, gap = 8;
        // The note graph gets half the room (at least 150 px): it is where the editing happens.
        const int graphH = juce::jmax (150, (r.getHeight() - chordH - toolsH - 2 * gap) / 2 + 20);

        notesScreen = r.removeFromBottom (graphH).reduced (3, 3);
        r.removeFromBottom (2);
        noteTools = r.removeFromBottom (toolsH);
        chordLane = r.removeFromBottom (chordH).withTrimmedLeft (gutter);
        r.removeFromBottom (gap - 2);
        takesScreen = r.reduced (3, 3);

        {
            auto t = takesScreen.reduced (2, 2);
            ruler = t.removeFromTop (24).withTrimmedLeft (gutter);
            takesLane = t.withTrimmedLeft (gutter).withTrimmedRight (6).withTrimmedBottom (2);
            ruler.setRight (takesLane.getRight());
        }
        {
            auto n = notesScreen.reduced (2, 2);
            auto scroll = n.removeFromBottom (14);
            hScroll.setBounds (scroll.withTrimmedLeft (gutter).withTrimmedRight (10).reduced (0, 2));
            noteLane = n.withTrimmedLeft (gutter).withTrimmedRight (6).withTrimmedTop (4);
        }
        chordLane.setRight (takesLane.getRight());
        followButton.setBounds (takesScreen.getX() + 6, takesScreen.getY() + 3, gutter, 22);

        toolGrooves.clear();
        auto t = noteTools.reduced (0, 4);
        snapButton.setBounds (t.removeFromLeft (132));
        t.removeFromLeft (8);
        undoButton.setBounds (t.removeFromLeft (62));
        t.removeFromLeft (4);
        redoButton.setBounds (t.removeFromLeft (62));
        t.removeFromLeft (12);
        selectionLabel.setBounds (t.removeFromLeft (150));
        auto slot = [&t, this] (juce::Label& l, juce::Slider& s)
        {
            t.removeFromLeft (10);
            auto g = t.removeFromLeft (58 + 180 + 4);
            toolGrooves.push_back (g.expanded (2, 1));
            l.setBounds (g.removeFromLeft (58));
            s.setBounds (g.removeFromLeft (180));
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

        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [this, id = r.id, idx = hit.note, splitAt] (int res)
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
        m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [this] (int res)
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
            m.addItem (3, "This is the beat, not a vocal (use it as the beat)", ! r->missing);
            m.addItem (2, "Remove take from session");
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this).withMousePosition(), [this, id] (int res)
            {
                if (res == 1 && onAnalyseRegion) onAnalyseRegion (id);
                if (res == 2 && onDeleteRegion) onDeleteRegion (id);
                if (res == 3 && onUseTakeAsBeat) onUseTakeAsBeat (id);
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

        g.setColour (r.missing ? Theme::bad.withAlpha (0.18f) : Theme::region);
        g.fillRoundedRectangle (box, 6.0f);
        g.setColour (r.missing ? Theme::bad : Theme::regionEdge);
        g.drawRoundedRectangle (box, 6.0f, 1.0f);

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
        // A faint centre line, then the waveform in pale lilac-silver.
        g.setColour (Theme::waveform.withAlpha (0.25f));
        g.drawHorizontalLine ((int) midY, clip.getX(), clip.getRight());
        g.setColour (Theme::waveform.withAlpha (0.92f));

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
        // Chord strip on the metal: a recessed channel with a silver key per chord.
        Paint::groove (g, chordLane.toFloat().reduced (0.0f, 2.0f), 7.0f);
        g.setFont (Theme::font (13.0f, true));
        for (auto& r : data.regions)
            for (auto& c : r.chords)
            {
                const float x0 = juce::jmax (ppqToX (c.startPpq), (float) chordLane.getX());
                const float x1 = juce::jmin (ppqToX (c.endPpq), (float) chordLane.getRight());
                if (x1 <= x0) continue;
                auto box = juce::Rectangle<float> (x0, (float) chordLane.getY() + 4.0f, x1 - x0, (float) chordLane.getHeight() - 8.0f).reduced (1.5f, 0.0f);
                const bool nc = c.root < 0;
                if (nc)
                {
                    g.setColour (Theme::metalShadow.withAlpha (0.18f));
                    g.fillRoundedRectangle (box, 5.0f);
                    continue;
                }
                const float conf = juce::jlimit (0.0f, 1.0f, c.confidence);
                juce::ColourGradient grad (juce::Colour (0xfffbfbfc), box.getX(), box.getY(), juce::Colour (0xffc9c9cf), box.getX(), box.getBottom(), false);
                g.setGradientFill (grad);
                g.setOpacity (0.55f + 0.45f * conf);
                g.fillRoundedRectangle (box, 5.0f);
                g.setOpacity (1.0f);
                g.setColour (Theme::metalShadow.withAlpha (0.6f));
                g.drawRoundedRectangle (box.reduced (0.5f), 5.0f, 1.0f);
                if (box.getWidth() > 18.0f)
                {
                    g.setColour (Theme::ink.withAlpha (0.6f + 0.4f * conf));
                    g.drawText (Analysis::chordName (c.root, c.quality), box.reduced (8.0f, 0.0f).toNearestInt(),
                                juce::Justification::centredLeft, true);
                }
            }
    }

    void TimelineComponent::drawNotes (juce::Graphics& g)
    {
        octaveLabels.clear();
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
            g.setColour (Theme::textDim.withAlpha (0.7f));
            g.setFont (Theme::font (12.5f));
            g.drawText (tracking ? "Tracking pitch..." : "Notes appear here after a take is analysed",
                        noteLane, juce::Justification::centred);
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

        // Octave C lines, labelled in the gutter.
        for (int m = (int) lo; m <= (int) hi; ++m)
        {
            if (m % 12 != 0) continue;
            const float y = yFor ((float) m);
            g.setColour (Theme::gridBar);
            g.drawHorizontalLine ((int) y, lane.getX(), lane.getRight());
            octaveLabels.push_back ({ m, y });
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
                    g.drawRoundedRectangle (juce::Rectangle<float> (g0, yFor (n.detectedMidi) - h * 0.5f, juce::jmax (2.0f, g1 - g0), h), h * 0.5f, 1.0f);
                }

                const float cents = std::abs (n.centsOff());
                const auto colour = cents < 15.0f ? Theme::good : (cents < 35.0f ? Theme::warn : Theme::bad);

                // Target pitch as an outline, detected pitch as the filled blob.
                if (std::abs (n.targetMidi - n.detectedMidi) > 0.05f)
                {
                    g.setColour (Theme::text.withAlpha (0.35f));
                    g.drawRoundedRectangle (juce::Rectangle<float> (x0, yFor (n.targetMidi) - h * 0.5f, juce::jmax (2.0f, x1 - x0), h), h * 0.5f, 1.0f);
                }
                // Corrected notes are drawn at their target (where they will sound), with a
                // faint ghost where they were sung.
                const bool corrected = n.correction > 0.0f;
                const float shown = n.detectedMidi + (n.targetMidi - n.detectedMidi) * n.correction;
                if (corrected)
                {
                    g.setColour (colour.withAlpha (0.22f));
                    g.fillRoundedRectangle (juce::Rectangle<float> (x0, yFor (n.detectedMidi) - h * 0.5f, juce::jmax (2.0f, x1 - x0), h), h * 0.5f);
                }
                // Corrected notes glow pink; untouched notes show how far off they were sung
                // (silver = in tune, amber = a little off, red = well off).
                const auto blob = juce::Rectangle<float> (x0, yFor (shown) - h * 0.5f, juce::jmax (2.0f, x1 - x0), h);
                Paint::glowBar (g, blob, corrected ? Theme::accent : colour, corrected ? 3.5f : 1.5f, h * 0.5f);
                if (isSelected (ri, ni))
                {
                    g.setColour (juce::Colours::white.withAlpha (0.35f));
                    g.fillRoundedRectangle (blob, h * 0.5f);
                    g.setColour (juce::Colours::white);
                    g.drawRoundedRectangle (blob.expanded (1.5f), h * 0.5f + 1.0f, 2.0f);
                }
                else if (n.userEdited)
                {
                    g.setColour (Theme::text);
                    g.drawRoundedRectangle (blob, h * 0.5f, 1.0f);
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
            g.setColour (Theme::accent.withAlpha (0.12f));
            g.fillRect (marquee);
            g.setColour (Theme::accent);
            g.drawRect (marquee, 1.0f);
        }
    }

    void TimelineComponent::paint (juce::Graphics& g)
    {
        // No background fill: the editor's metal shows through around the screens.
        Paint::screen (g, takesScreen.toFloat(), 14.0f);
        Paint::screen (g, notesScreen.toFloat(), 14.0f);

        g.setFont (Theme::font (12.0f));
        g.setColour (Theme::textDim);
        g.drawText ("Takes", takesScreen.getX() + 10, takesLane.getY() + 2, gutter - 12, 16, juce::Justification::centredLeft);
        g.setColour (Theme::inkDim);
        g.setFont (Theme::font (12.5f, true));
        g.drawText ("Chords", takesScreen.getX() + 4, chordLane.getY(), gutter - 6, chordLane.getHeight(), juce::Justification::centredLeft);

        // Note graph tool row (on the metal).
        for (auto& r : toolGrooves)
            Paint::groove (g, r.toFloat(), (float) r.getHeight() * 0.5f);
        if (data.scaleText.isNotEmpty())
        {
            g.setFont (Theme::font (12.5f, true));
            const auto text = "Scale: " + data.scaleText;
            const int tw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), text) + 28;
            const int left = driftSlider.getRight() + 16;
            auto pillArea = noteTools.reduced (0, 6).withLeft (juce::jmax (left, noteTools.getRight() - tw));
            if (pillArea.getWidth() > 40)
            {
                Paint::pill (g, pillArea.toFloat(), true, false, false, true);
                g.setColour (data.snapToScale ? Theme::ink : Theme::inkFaint);
                g.drawFittedText (text, pillArea.reduced (12, 0), juce::Justification::centred, 1, 0.8f);
            }
        }

        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (ruler.getUnion (noteLane));
            drawGrid (g, ruler, true);
            drawGrid (g, takesLane, false);

            for (auto& r : data.regions)
                drawRegion (g, r);

            if (data.regions.empty() && ! liveCapturing)
            {
                // Empty state: nothing recorded or imported yet.
                auto c = takesLane.toFloat().withSizeKeepingCentre (360.0f, 110.0f);
                Paint::waveformGlyph (g, c.removeFromTop (46.0f).withSizeKeepingCentre (64.0f, 40.0f), Theme::textDim.withAlpha (0.8f));
                g.setColour (Theme::text.withAlpha (0.85f));
                g.setFont (Theme::font (16.0f, true));
                g.drawText ("No audio loaded", c.removeFromTop (30.0f), juce::Justification::centred);
                g.setColour (Theme::textDim);
                g.setFont (Theme::font (12.5f));
                g.drawText ("Import a song, drop files here, or arm and record", c.removeFromTop (20.0f), juce::Justification::centred);
            }

            if (liveCapturing)
            {
                const float x0 = ppqToX (liveStartPpq), x1 = ppqToX (liveStartPpq + liveLengthPpq);
                g.setColour (Theme::record.withAlpha (0.30f));
                g.fillRoundedRectangle (juce::Rectangle<float> (x0, (float) takesLane.getY() + 6.0f, juce::jmax (1.0f, x1 - x0), (float) takesLane.getHeight() - 12.0f), 6.0f);
                g.setColour (Theme::record);
                g.setFont (Theme::font (12.5f, true));
                g.drawText ("Recording", juce::Rectangle<float> (x0 + 6.0f, (float) takesLane.getY() + 8.0f, 90.0f, 16.0f), juce::Justification::centredLeft);
            }

            drawGrid (g, noteLane, false);
            drawNotes (g);
        }

        // Chords sit between the screens, outside the clip above.
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (chordLane.expanded (0, 2));
            drawChords (g);
        }

        // Octave labels in the note screen's gutter.
        g.setFont (Theme::font (12.0f, true));
        g.setColour (Theme::text.withAlpha (0.85f));
        for (auto& [m, y] : octaveLabels)
            if (y > (float) noteLane.getY() + 6.0f && y < (float) noteLane.getBottom() - 2.0f)
                g.drawText ("C" + juce::String (m / 12 - 1), notesScreen.getX() + 12, (int) y - 8, gutter - 18, 16, juce::Justification::centredLeft);

        // Playhead: a pink line with a soft glow, in both screens.
        if (host.hasPpq)
        {
            const float x = ppqToX (host.ppqPosition);
            if (x >= (float) takesLane.getX() && x <= (float) takesLane.getRight())
                for (auto span : { juce::Range<float> ((float) ruler.getY(), (float) takesLane.getBottom()),
                                   juce::Range<float> ((float) noteLane.getY(), (float) noteLane.getBottom()) })
                {
                    for (int i = 3; i >= 1; --i)
                    {
                        g.setColour (Theme::playhead.withAlpha (0.10f * (float) (4 - i)));
                        g.fillRect (juce::Rectangle<float> (x - (float) i * 1.5f, span.getStart(), (float) i * 3.0f, span.getLength()));
                    }
                    g.setColour (Theme::playhead);
                    g.fillRect (juce::Rectangle<float> (x - 0.75f, span.getStart(), 1.5f, span.getLength()));
                }
        }

        // Key display, top right of the ruler (read-only; the Key menu above changes it).
        g.setFont (Theme::font (12.5f, true));
        const auto keyText = "Key: " + data.keyText + (data.keyIsOverride ? " (manual)" : "");
        const int kw = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), keyText) + 24;
        auto keyArea = juce::Rectangle<int> (ruler.getRight() - kw - 2, ruler.getY() + 2, kw, ruler.getHeight() - 4).toFloat();
        g.setColour (juce::Colour (0xff2a2b30));
        g.fillRoundedRectangle (keyArea, keyArea.getHeight() * 0.5f);
        g.setColour (juce::Colours::white.withAlpha (0.12f));
        g.drawRoundedRectangle (keyArea.reduced (0.5f), keyArea.getHeight() * 0.5f, 1.0f);
        g.setColour (data.keyIsOverride ? Theme::warn : Theme::text);
        g.drawText (keyText, keyArea, juce::Justification::centred);
    }
}
