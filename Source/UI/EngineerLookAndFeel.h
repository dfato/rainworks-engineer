#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ve::ui
{
    // Shared drawing for the aluminium chassis. Everything is drawn in vector,
    // so the editor stays sharp at any size and on high-DPI screens.
    namespace Paint
    {
        // Brushed metal plate: a vertical sheen plus fine horizontal grain.
        void brushedMetal (juce::Graphics&, juce::Rectangle<float>, float corner, float brightness = 1.0f);
        // A raised plate: brushed metal with a light top edge and a soft shadow below.
        void raisedPlate (juce::Graphics&, juce::Rectangle<float>, float corner);
        // A dark glass screen set into the metal.
        void screen (juce::Graphics&, juce::Rectangle<float>, float corner);
        // A shallow recessed channel in the metal (groups a label with its control).
        void groove (juce::Graphics&, juce::Rectangle<float>, float corner);
        // A convex pill button face.
        void pill (juce::Graphics&, juce::Rectangle<float>, bool down, bool hover, bool lit, bool enabled = true);
        // A small light: pink and glowing when on, a dark recessed dot when off.
        void led (juce::Graphics&, juce::Point<float> centre, float radius, bool on);
        // A vertical pink light tube in the side rails.
        void lightTube (juce::Graphics&, juce::Rectangle<float>, float level);
        // A grid of drilled holes (speaker grille).
        void grille (juce::Graphics&, juce::Rectangle<float>);
        // A countersunk screw head.
        void screw (juce::Graphics&, juce::Point<float> centre, float radius);
        // A rounded glowing bar (pink notes, scroll thumb, playhead).
        void glowBar (juce::Graphics&, juce::Rectangle<float>, juce::Colour core, float glow, float corner);
        // Small line icons drawn in the given colour.
        void headphones (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
        void chevronDown (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
        void waveformGlyph (juce::Graphics&, juce::Rectangle<float>, juce::Colour);
    }

    class EngineerLookAndFeel final : public juce::LookAndFeel_V4
    {
    public:
        EngineerLookAndFeel();

        // Buttons
        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool hover, bool down) override;
        void drawButtonText (juce::Graphics&, juce::TextButton&, bool hover, bool down) override;
        juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
        void drawToggleButton (juce::Graphics&, juce::ToggleButton&, bool hover, bool down) override;

        // Combo boxes
        void drawComboBox (juce::Graphics&, int w, int h, bool down, int, int, int, int, juce::ComboBox&) override;
        juce::Font getComboBoxFont (juce::ComboBox&) override;
        void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

        // Sliders
        void drawLinearSlider (juce::Graphics&, int x, int y, int w, int h, float pos, float minPos, float maxPos,
                               juce::Slider::SliderStyle, juce::Slider&) override;
        void drawRotarySlider (juce::Graphics&, int x, int y, int w, int h, float pos, float start, float end, juce::Slider&) override;
        int getSliderThumbRadius (juce::Slider&) override { return 8; }
        juce::Label* createSliderTextBox (juce::Slider&) override;
        juce::Slider::SliderLayout getSliderLayout (juce::Slider&) override;

        // Labels and text
        void drawLabel (juce::Graphics&, juce::Label&) override;
        juce::Font getLabelFont (juce::Label&) override;
        juce::Typeface::Ptr getTypefaceForFont (const juce::Font&) override;

        // Menus
        void drawPopupMenuBackground (juce::Graphics&, int w, int h) override;
        void drawPopupMenuItem (juce::Graphics&, const juce::Rectangle<int>&, bool isSeparator, bool isActive, bool isHighlighted,
                                bool isTicked, bool hasSubMenu, const juce::String& text, const juce::String& shortcut,
                                const juce::Drawable* icon, const juce::Colour* textColour) override;
        void drawPopupMenuSectionHeader (juce::Graphics&, const juce::Rectangle<int>&, const juce::String&) override;
        juce::Font getPopupMenuFont() override;
        void getIdealPopupMenuItemSize (const juce::String&, bool isSeparator, int standardHeight, int& w, int& h) override;
        int getPopupMenuBorderSize() override { return 5; }

        // Scroll bar, tooltips, resizer
        void drawScrollbar (juce::Graphics&, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                            int thumbStart, int thumbSize, bool over, bool down) override;
        int getDefaultScrollbarWidth() override { return 10; }
        void drawTooltip (juce::Graphics&, const juce::String&, int w, int h) override;
        juce::Rectangle<int> getTooltipBounds (const juce::String&, juce::Point<int>, juce::Rectangle<int>) override;
        void drawCornerResizer (juce::Graphics&, int w, int h, bool over, bool dragging) override;

        // Text editor (the "Learn style" name field) and alert windows
        void fillTextEditorBackground (juce::Graphics&, int w, int h, juce::TextEditor&) override;
        void drawTextEditorOutline (juce::Graphics&, int w, int h, juce::TextEditor&) override;
        void drawAlertBox (juce::Graphics&, juce::AlertWindow&, const juce::Rectangle<int>& textArea, juce::TextLayout&) override;
        juce::Font getAlertWindowTitleFont() override;
        juce::Font getAlertWindowMessageFont() override;
        juce::Font getAlertWindowFont() override;
    };
}
