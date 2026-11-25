#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ve::ui::Theme
{
    inline const juce::Colour background   { 0xff15171c };
    inline const juce::Colour panel        { 0xff1d2027 };
    inline const juce::Colour panelLight   { 0xff262a33 };
    inline const juce::Colour text         { 0xffe8eaee };
    inline const juce::Colour textDim      { 0xff8a909c };
    inline const juce::Colour accent       { 0xff5aa9ff };
    inline const juce::Colour good         { 0xff4cd97b };
    inline const juce::Colour warn         { 0xffe0b040 };
    inline const juce::Colour bad          { 0xffe0505a };
    inline const juce::Colour record       { 0xffe23c3c };
    inline const juce::Colour region       { 0xff2f5d8a };
    inline const juce::Colour regionEdge   { 0xff6fb1f0 };
    inline const juce::Colour waveform     { 0xffcfe6ff };
    inline const juce::Colour gridBar      { 0x40ffffff };
    inline const juce::Colour gridBeat     { 0x18ffffff };
    inline const juce::Colour playhead     { 0xffff5a5a };

    inline juce::FontOptions font (float size, bool bold = false)
    {
        return juce::FontOptions (size, bold ? juce::Font::bold : juce::Font::plain);
    }
}
