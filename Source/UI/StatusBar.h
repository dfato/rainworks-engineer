#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ve::ui
{
    class StatusBar final : public juce::Component
    {
    public:
        void setLeftText (const juce::String& t)  { if (t != left)  { left = t;  repaint(); } }
        void setRightText (const juce::String& t) { if (t != right) { right = t; repaint(); } }
        void setLeftColour (juce::Colour c)       { if (c != leftColour) { leftColour = c; repaint(); } }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().reduced (10, 0);
            g.setFont (juce::FontOptions (12.0f));
            g.setColour (leftColour);
            g.drawText (left, r, juce::Justification::centredLeft, true);
            g.setColour (juce::Colour (0xff8a909c));
            g.drawText (right, r, juce::Justification::centredRight, true);
        }

    private:
        juce::String left, right;
        juce::Colour leftColour { 0xffe8eaee };
    };
}
