#include "HostInfoComponent.h"
#include "Theme.h"

namespace ve::ui
{
    void HostInfoComponent::setSnapshot (const HostSnapshot& s)
    {
        snap = s;
        repaint();
    }

    void HostInfoComponent::paint (juce::Graphics& g)
    {
        // One compact line in the footer: label in dim ink, value in ink.
        auto area = getLocalBounds();
        const juce::String none ("--");

        if (! snap.hasPlayHead)
        {
            g.setFont (Theme::font (12.0f));
            g.setColour (Theme::inkFaint);
            g.drawText ("No host transport", area, juce::Justification::centredRight);
            return;
        }

        struct Field { juce::String label, value; juce::Colour colour; };
        std::vector<Field> fields;
        const bool derivedPpq = (snap.derivedFlags & HostSnapshot::derivedPpq) != 0;
        const bool derivedSamples = (snap.derivedFlags & HostSnapshot::derivedTimeSamples) != 0;

        fields.push_back ({ "BPM", snap.hasBpm ? juce::String (snap.bpm, 2) : none, Theme::ink });
        fields.push_back ({ "SIG", snap.hasTimeSig ? juce::String (snap.timeSigNumerator) + "/" + juce::String (snap.timeSigDenominator) : none, Theme::ink });

        juce::String barBeat = none;
        if (snap.hasPpq)
        {
            const double barLen = snap.timeSigNumerator * snap.ppqPerBeat();
            const double barStart = snap.hasBarStart ? snap.ppqLastBarStart : std::floor (snap.ppqPosition / barLen) * barLen;
            const int bar = (int) std::floor (barStart / barLen + 1.0e-6) + 1;
            const double beat = (snap.ppqPosition - barStart) / snap.ppqPerBeat() + 1.0;
            barBeat = juce::String (bar) + " | " + juce::String (beat, 2);
        }
        fields.push_back ({ "BAR", barBeat, Theme::ink });
        fields.push_back ({ derivedPpq ? "PPQ*" : "PPQ", snap.hasPpq ? juce::String (snap.ppqPosition, 3) : none, Theme::ink });
        fields.push_back ({ derivedSamples ? "SAMPLE*" : "SAMPLE", snap.hasTimeSamples ? juce::String (snap.timeInSamples) : none, Theme::ink });
        fields.push_back ({ "", snap.isPlaying ? "Playing" : "Stopped", snap.isPlaying ? Theme::accentDeep : Theme::inkDim });
        fields.push_back ({ "REC", snap.isRecording ? "On" : "Off", snap.isRecording ? Theme::record : Theme::inkDim });
        juce::String loop = snap.isLooping ? "On" : "Off";
        if (snap.hasLoopPoints)
            loop << " " << juce::String (snap.loopStartPpq, 2) << "-" << juce::String (snap.loopEndPpq, 2);
        fields.push_back ({ "LOOP", loop, snap.isLooping ? Theme::accentDeep : Theme::inkDim });
        fields.push_back ({ "RATE", snap.sampleRate > 0 ? juce::String (snap.sampleRate / 1000.0, 1) + "k" : none, Theme::ink });

        // Lay out right to left so the line hugs the right edge.
        const auto labelFont = Theme::font (10.0f, true), valueFont = Theme::font (12.0f, true);
        int x = area.getRight();
        for (auto it = fields.rbegin(); it != fields.rend(); ++it)
        {
            const int vw = juce::GlyphArrangement::getStringWidthInt (valueFont, it->value) + 2;
            const int lw = it->label.isEmpty() ? 0 : juce::GlyphArrangement::getStringWidthInt (labelFont, it->label) + 4;
            x -= vw;
            g.setFont (valueFont);
            g.setColour (it->colour);
            g.drawText (it->value, x, area.getY(), vw, area.getHeight(), juce::Justification::centredLeft, false);
            x -= lw;
            if (lw > 0)
            {
                g.setFont (labelFont);
                g.setColour (Theme::inkFaint);
                g.drawText (it->label, x, area.getY(), lw, area.getHeight(), juce::Justification::centredLeft, false);
            }
            x -= 12;
            if (x < area.getX()) break;
        }
    }
}
