#include "HeaderComponent.h"
#include "Theme.h"
#include "Engine/EngineConfig.h"
#include <cmath>

namespace ve::ui
{
    HeaderComponent::HeaderComponent (juce::String productName, juce::String version)
        : name (std::move (productName)), versionText ("v" + std::move (version)) {}

    juce::Colour HeaderComponent::backgroundColour() { return Theme::background; }

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
        auto area = getLocalBounds().reduced (12, 4);

        g.setColour (Theme::text);
        g.setFont (Theme::font (20.0f, true));
        const int nameW = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name) + 12;
        g.drawText (name, area.removeFromLeft (nameW), juce::Justification::centredLeft);

        g.setColour (Theme::textDim);
        g.setFont (Theme::font (13.0f));
        g.drawText (versionText, area.removeFromLeft (60), juce::Justification::centredLeft);

        // Right side: sidechain light, then input format.
        juce::String scText;
        juce::Colour scColour;
        if (! status.sidechainEnabled)
        {
            scText = "Sidechain: bus disabled";
            scColour = juce::Colours::grey;
        }
        else if (status.sidechainPeak > ve::sidechainSignalFloor)
        {
            const auto db = 20.0f * std::log10 (status.sidechainPeak);
            scText = "Sidechain: " + juce::String (db, 1) + " dBFS";
            scColour = Theme::good;
        }
        else
        {
            scText = "Sidechain: no signal";
            scColour = Theme::warn;
        }

        auto right = area.removeFromRight (230);
        g.setColour (scColour);
        g.fillEllipse (right.removeFromLeft (14).withSizeKeepingCentre (10, 10).toFloat());
        right.removeFromLeft (6);
        g.drawText (scText, right, juce::Justification::centredLeft);

        const auto mainText = status.mainInputChannels == 1 ? juce::String ("Input: mono")
                            : status.mainInputChannels == 2 ? juce::String ("Input: stereo")
                                                            : juce::String ("Input: waiting");
        area.removeFromRight (16);
        g.setColour (Theme::textDim);
        g.drawText (mainText, area.removeFromRight (110), juce::Justification::centredRight);
    }
}
