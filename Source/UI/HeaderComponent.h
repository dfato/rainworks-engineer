#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ve::ui
{
    // Plugin name, version and a routing status line.
    // Knows nothing about the processor: the editor pushes plain values in.
    class HeaderComponent final : public juce::Component
    {
    public:
        struct Status
        {
            bool  sidechainEnabled  = false;
            float sidechainPeak     = 0.0f;
            int   mainInputChannels = 0;
        };

        HeaderComponent (juce::String productName, juce::String version);

        void setStatus (const Status&);
        void paint (juce::Graphics&) override;

        static juce::Colour backgroundColour() { return juce::Colour (0xff15171c); }

    private:
        juce::String name, versionText;
        Status status;
    };
}
