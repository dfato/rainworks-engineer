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
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);

        auto area = getLocalBounds().reduced (8, 2);
        const juce::String none ("--");

        auto field = [&] (const juce::String& label, const juce::String& value, int width, juce::Colour c = Theme::text)
        {
            auto r = area.removeFromLeft (width);
            g.setFont (Theme::font (10.0f));
            g.setColour (Theme::textDim);
            g.drawText (label, r.removeFromTop (r.getHeight() / 2 - 1), juce::Justification::bottomLeft);
            g.setFont (Theme::font (14.0f, true));
            g.setColour (c);
            g.drawText (value, r, juce::Justification::topLeft);
            area.removeFromLeft (6);
        };

        if (! snap.hasPlayHead)
        {
            g.setFont (Theme::font (13.0f));
            g.setColour (Theme::textDim);
            g.drawText ("No host transport (standalone or host gives no play head)", area, juce::Justification::centredLeft);
            return;
        }

        const bool derivedPpq = (snap.derivedFlags & HostSnapshot::derivedPpq) != 0;
        const bool derivedSamples = (snap.derivedFlags & HostSnapshot::derivedTimeSamples) != 0;

        field ("BPM", snap.hasBpm ? juce::String (snap.bpm, 2) : none, 62);
        field ("SIG", snap.hasTimeSig ? juce::String (snap.timeSigNumerator) + "/" + juce::String (snap.timeSigDenominator) : none, 40);

        juce::String barBeat = none;
        if (snap.hasPpq)
        {
            const double barLen = snap.timeSigNumerator * snap.ppqPerBeat();
            const double barStart = snap.hasBarStart ? snap.ppqLastBarStart : std::floor (snap.ppqPosition / barLen) * barLen;
            const int bar = (int) std::floor (barStart / barLen + 1.0e-6) + 1;
            const double beat = (snap.ppqPosition - barStart) / snap.ppqPerBeat() + 1.0;
            barBeat = juce::String (bar) + " | " + juce::String (beat, 2);
        }
        field ("BAR | BEAT", barBeat, 82);
        field (derivedPpq ? "PPQ*" : "PPQ", snap.hasPpq ? juce::String (snap.ppqPosition, 3) : none, 82);
        field (derivedSamples ? "SAMPLE*" : "SAMPLE", snap.hasTimeSamples ? juce::String (snap.timeInSamples) : none, 92);

        field ("PLAY", snap.isPlaying ? "Playing" : "Stopped", 62, snap.isPlaying ? Theme::good : Theme::textDim);
        field ("REC", snap.isRecording ? "Rec" : "Off", 38, snap.isRecording ? Theme::record : Theme::textDim);

        juce::String loop = snap.isLooping ? "On" : "Off";
        if (snap.hasLoopPoints)
            loop << "  " << juce::String (snap.loopStartPpq, 2) << " - " << juce::String (snap.loopEndPpq, 2);
        field ("LOOP (PPQ)", loop, 130, snap.isLooping ? Theme::accent : Theme::textDim);

        field ("RATE", snap.sampleRate > 0 ? juce::String (snap.sampleRate / 1000.0, 1) + "k" : none, 46);
    }
}
