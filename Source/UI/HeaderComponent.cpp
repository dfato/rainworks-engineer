#include "HeaderComponent.h"
#include "Theme.h"
#include "EngineerLookAndFeel.h"
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
        // Sits on the top plate: dark ink on metal. The centre is left free for the Amount knob.
        auto area = getLocalBounds();

        g.setColour (Theme::ink);
        g.setFont (Theme::font (17.0f, true));
        const int nameW = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), name) + 10;
        g.drawText (name, area.removeFromLeft (nameW), juce::Justification::centredLeft);

        g.setColour (Theme::inkDim);
        g.setFont (Theme::font (12.5f));
        g.drawText (versionText, area.removeFromLeft (60), juce::Justification::centredLeft);

        // Right side: input format, then the sidechain light.
        juce::String scText;
        bool scLit = false;
        juce::Colour scColour = Theme::inkFaint;
        if (! status.sidechainEnabled)
            scText = "Sidechain: bus disabled";
        else if (status.sidechainPeak > ve::sidechainSignalFloor)
        {
            const auto db = 20.0f * std::log10 (status.sidechainPeak);
            scText = "Sidechain: " + juce::String (db, 1) + " dBFS";
            scLit = true;
        }
        else
        {
            scText = "Sidechain: no signal";
            scColour = Theme::warn;
        }

        g.setFont (Theme::font (12.5f));
        const int scW = juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), scText) + 4;
        g.setColour (Theme::ink);
        g.drawText (scText, area.removeFromRight (scW), juce::Justification::centredRight);
        auto light = area.removeFromRight (20).toFloat();
        if (scLit)
            Paint::led (g, light.getCentre(), 3.5f, true);
        else
        {
            Paint::led (g, light.getCentre(), 3.5f, false);
            g.setColour (scColour.withAlpha (0.9f));
            g.fillEllipse (juce::Rectangle<float> (5.0f, 5.0f).withCentre (light.getCentre()));
        }

        const auto mainText = status.mainInputChannels == 1 ? juce::String ("Input: mono")
                            : status.mainInputChannels == 2 ? juce::String ("Input: stereo")
                                                            : juce::String ("Input: waiting");
        area.removeFromRight (10);
        g.setColour (Theme::inkDim);
        g.drawText (mainText, area.removeFromRight (100), juce::Justification::centredRight);
    }
}
