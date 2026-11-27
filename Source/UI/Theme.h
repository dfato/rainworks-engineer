#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

// Brushed aluminium chassis, dark glass screens, pink glow.
// Two kinds of text: `ink` sits on metal, `text` sits on the dark screens.
namespace ve::ui::Theme
{
    // ---- chassis (metal) ----
    inline const juce::Colour metalLight   { 0xffeeeef0 };
    inline const juce::Colour metal        { 0xffd6d6da };
    inline const juce::Colour metalDark    { 0xffa9a9b0 };
    inline const juce::Colour metalShadow  { 0xff6e6e76 };
    inline const juce::Colour metalEdge    { 0xff8d8d95 };
    inline const juce::Colour ink          { 0xff26262b };   // text on metal
    inline const juce::Colour inkDim       { 0xff5f5f68 };
    inline const juce::Colour inkFaint     { 0xff8b8b94 };

    // ---- screens (dark glass) ----
    inline const juce::Colour screen       { 0xff141518 };
    inline const juce::Colour screenLight  { 0xff1f2125 };
    inline const juce::Colour background   { 0xff141518 };   // lane background inside screens
    inline const juce::Colour panel        { 0xff1b1c20 };
    inline const juce::Colour panelLight   { 0xff2c2e34 };
    inline const juce::Colour text         { 0xffeceaf0 };   // text on screens
    inline const juce::Colour textDim      { 0xff9a98a3 };

    // ---- glow ----
    inline const juce::Colour accent       { 0xffff6fa3 };   // pink
    inline const juce::Colour accentSoft   { 0xffffb3cf };
    inline const juce::Colour accentDeep   { 0xffe0457f };

    inline const juce::Colour good         { 0xffe6e4ec };   // in tune: pale silver (pink is reserved for corrected)
    inline const juce::Colour warn         { 0xfff0b44c };
    inline const juce::Colour bad          { 0xffff6060 };
    inline const juce::Colour record       { 0xffff4f7e };
    inline const juce::Colour region       { 0x0effffff };
    inline const juce::Colour regionEdge   { 0x30ffffff };
    inline const juce::Colour waveform     { 0xffcfcad6 };
    inline const juce::Colour gridBar      { 0x2effffff };
    inline const juce::Colour gridBeat     { 0x12ffffff };
    inline const juce::Colour playhead     { 0xffff4f8b };

    // Inter, embedded (Resources/Fonts), so the UI looks the same on every machine.
    juce::Typeface::Ptr typeface (bool bold);

    inline juce::FontOptions font (float size, bool bold = false)
    {
        return juce::FontOptions (typeface (bold)).withHeight (size);
    }
}
