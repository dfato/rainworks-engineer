#include "HeaderComponent.h"
#include "Engine/EngineConfig.h"
#include <cmath>

namespace ve::ui
{
    HeaderComponent::HeaderComponent (juce::String productName, juce::String version)
        : name (std::move (productName)), versionText ("v" + std::move (version))
    {
        setOpaque (false);
    }

    void HeaderComponent::setStatus (const Status& s)
    {
        const bool changed = s.sidechainEnabled != status.sidechainEnabled
                          || s.mainInputChannels != status.mainInputChannels
                          || std::abs (s.sidechainPeak - status.sidechainPeak) > 1.0e-4f;
        status = s;
        if (changed)
            repaint();
    }

    void HeaderComponent::paint (juce::Graphics& g)
    {
        auto area = getLocalBounds().reduced (24);

        g.setColour (juce::Colours::white);
        g.setFont (juce::FontOptions (30.0f, juce::Font::bold));
        g.drawText (name, area.removeFromTop (40), juce::Justification::centredLeft);

        g.setColour (juce::Colours::white.withAlpha (0.55f));
        g.setFont (juce::FontOptions (15.0f));
        g.drawText (versionText + "  |  VocalEngine foundation", area.removeFromTop (22),
                    juce::Justification::centredLeft);

        area.removeFromTop (24);

        const auto mainText = status.mainInputChannels == 1 ? juce::String ("Main input: mono")
                            : status.mainInputChannels == 2 ? juce::String ("Main input: stereo")
                                                            : juce::String ("Main input: waiting for audio");
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.drawText (mainText, area.removeFromTop (22), juce::Justification::centredLeft);

        juce::String scText;
        juce::Colour scColour;
        if (! status.sidechainEnabled)
        {
            scText = "Sidechain: bus disabled by host";
            scColour = juce::Colours::grey;
        }
        else if (status.sidechainPeak > ve::sidechainSignalFloor)
        {
            const auto db = 20.0f * std::log10 (status.sidechainPeak);
            scText = "Sidechain: receiving signal (" + juce::String (db, 1) + " dBFS peak)";
            scColour = juce::Colour (0xff4cd97b);
        }
        else
        {
            scText = "Sidechain: enabled, no signal";
            scColour = juce::Colour (0xffe0b040);
        }

        auto row = area.removeFromTop (22);
        g.setColour (scColour);
        g.fillEllipse (row.removeFromLeft (14).withSizeKeepingCentre (10, 10).toFloat());
        row.removeFromLeft (8);
        g.drawText (scText, row, juce::Justification::centredLeft);
    }
}
