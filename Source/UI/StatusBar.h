#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "Theme.h"

namespace ve::ui
{
    class StatusBar final : public juce::Component
    {
    public:
        void setLeftText (const juce::String& t)  { if (t != left)  { left = t;  repaint(); } }
        void setRightText (const juce::String& t) { if (t != right) { right = t; repaint(); } }
        void setLeftColour (juce::Colour c)       { if (c != leftColour) { leftColour = c; repaint(); } }

        // Room kept free in the middle for the transport read-out (set by the editor).
        void setMiddleGap (juce::Range<int> r) { middle = r; repaint(); }

        void paint (juce::Graphics& g) override
        {
            auto r = getLocalBounds().reduced (10, 0);
            g.setFont (Theme::font (12.5f));
            const int rightW = juce::jmin (r.getWidth() / 3, juce::GlyphArrangement::getStringWidthInt (g.getCurrentFont(), right) + 8);
            auto rightArea = r.removeFromRight (rightW);
            g.setColour (Theme::inkDim);
            g.drawText (right, rightArea, juce::Justification::centredRight, true);
            if (! middle.isEmpty())
                r.setRight (juce::jmin (r.getRight(), middle.getStart() - 16));
            g.setColour (leftColour);
            g.drawText (left, r, juce::Justification::centredLeft, true);
        }

        int getRightTextWidth() const
        {
            return juce::GlyphArrangement::getStringWidthInt (Theme::font (12.5f), right) + 18;
        }

    private:
        juce::String left, right;
        juce::Colour leftColour { Theme::ink };
        juce::Range<int> middle;
    };
}
