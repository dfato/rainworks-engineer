#include "EngineerLookAndFeel.h"
#include "Theme.h"
#include "VEUiData.h"

namespace ve::ui
{
    juce::Typeface::Ptr Theme::typeface (bool bold)
    {
        static juce::Typeface::Ptr regular = juce::Typeface::createSystemTypefaceFor (VEUi::InterRegular_otf, (size_t) VEUi::InterRegular_otfSize);
        static juce::Typeface::Ptr semi    = juce::Typeface::createSystemTypefaceFor (VEUi::InterSemiBold_otf, (size_t) VEUi::InterSemiBold_otfSize);
        return bold ? semi : regular;
    }

    namespace
    {
        // Fine horizontal grain, made once: streaks of slightly lighter and darker metal.
        const juce::Image& grain()
        {
            static juce::Image img = []
            {
                constexpr int w = 512, h = 256;
                juce::Image im (juce::Image::ARGB, w, h, true);
                juce::Random rng (0x5eed);
                juce::Image::BitmapData bd (im, juce::Image::BitmapData::writeOnly);
                for (int y = 0; y < h; ++y)
                {
                    float rowBase = (rng.nextFloat() - 0.5f) * 0.10f;
                    float v = 0.0f;
                    for (int x = 0; x < w; ++x)
                    {
                        // Low-passed noise along the row gives long streaks.
                        v = v * 0.93f + (rng.nextFloat() - 0.5f) * 0.07f;
                        const float s = rowBase + v * 1.6f;
                        const auto c = s >= 0.0f ? juce::Colours::white.withAlpha (juce::jlimit (0.0f, 1.0f, s * 2.2f))
                                                 : juce::Colours::black.withAlpha (juce::jlimit (0.0f, 1.0f, -s * 1.6f));
                        bd.setPixelColour (x, y, c);
                    }
                }
                return im;
            }();
            return img;
        }

        juce::Path rounded (juce::Rectangle<float> r, float corner)
        {
            juce::Path p;
            p.addRoundedRectangle (r, corner);
            return p;
        }

        bool flag (const juce::Component& c, const char* name)
        {
            return (bool) c.getProperties().getWithDefault (name, false);
        }
    }

    //==========================================================================
    void Paint::brushedMetal (juce::Graphics& g, juce::Rectangle<float> r, float corner, float brightness)
    {
        const auto path = rounded (r, corner);
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (path);

        auto shade = [brightness] (juce::Colour c) { return brightness >= 1.0f ? c.brighter (brightness - 1.0f) : c.darker (1.0f - brightness); };
        juce::ColourGradient grad (shade (Theme::metalLight), r.getX(), r.getY(), shade (Theme::metalDark), r.getX(), r.getBottom(), false);
        grad.addColour (0.18, shade (Theme::metal.brighter (0.06f)));
        grad.addColour (0.55, shade (Theme::metal));
        grad.addColour (0.82, shade (Theme::metal.darker (0.05f)));
        g.setGradientFill (grad);
        g.fillRect (r);

        g.setTiledImageFill (grain(), 0, 0, 0.32f);
        g.fillRect (r);
    }

    void Paint::raisedPlate (juce::Graphics& g, juce::Rectangle<float> r, float corner)
    {
        // Soft shadow under the plate.
        for (int i = 3; i >= 1; --i)
        {
            g.setColour (juce::Colours::black.withAlpha (0.05f * (float) (4 - i)));
            g.fillRoundedRectangle (r.translated (0.0f, (float) i).expanded ((float) i * 0.5f), corner + (float) i);
        }
        brushedMetal (g, r, corner, 1.02f);
        g.setColour (juce::Colours::white.withAlpha (0.85f));
        g.drawRoundedRectangle (r.reduced (1.0f), corner - 1.0f, 1.0f);
        g.setColour (Theme::metalEdge.withAlpha (0.9f));
        g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
    }

    void Paint::screen (juce::Graphics& g, juce::Rectangle<float> r, float corner)
    {
        // Machined lip around the glass: dark on top (in shadow), light at the bottom.
        juce::ColourGradient lip (Theme::metalShadow, r.getX(), r.getY(), juce::Colours::white, r.getX(), r.getBottom(), false);
        g.setGradientFill (lip);
        g.fillRoundedRectangle (r.expanded (2.5f), corner + 2.5f);
        g.setColour (juce::Colour (0xff4a4b52));
        g.fillRoundedRectangle (r.expanded (1.0f), corner + 1.0f);

        juce::ColourGradient glass (Theme::screenLight, r.getX(), r.getY(), Theme::screen, r.getX(), r.getBottom(), false);
        g.setGradientFill (glass);
        g.fillRoundedRectangle (r, corner);

        // Diagonal sheen across the upper glass.
        {
            juce::Graphics::ScopedSaveState s (g);
            g.reduceClipRegion (rounded (r, corner));
            juce::Path sheen;
            sheen.startNewSubPath (r.getX() + r.getWidth() * 0.18f, r.getY());
            sheen.lineTo (r.getX() + r.getWidth() * 0.42f, r.getY());
            sheen.lineTo (r.getX() + r.getWidth() * 0.30f, r.getBottom());
            sheen.lineTo (r.getX() + r.getWidth() * 0.06f, r.getBottom());
            sheen.closeSubPath();
            g.setColour (juce::Colours::white.withAlpha (0.018f));
            g.fillPath (sheen);
            // Inner shadow along the top edge.
            juce::ColourGradient top (juce::Colours::black.withAlpha (0.55f), r.getX(), r.getY(),
                                      juce::Colours::transparentBlack, r.getX(), r.getY() + 10.0f, false);
            g.setGradientFill (top);
            g.fillRect (r.withHeight (10.0f));
        }
        g.setColour (juce::Colours::white.withAlpha (0.06f));
        g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
    }

    void Paint::groove (juce::Graphics& g, juce::Rectangle<float> r, float corner)
    {
        juce::ColourGradient grad (Theme::metalDark.withAlpha (0.55f), r.getX(), r.getY(),
                                   Theme::metalLight.withAlpha (0.35f), r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r, corner);
        g.setColour (Theme::metalShadow.withAlpha (0.45f));
        g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
        g.setColour (juce::Colours::white.withAlpha (0.7f));
        g.drawLine (r.getX() + corner, r.getBottom() + 0.5f, r.getRight() - corner, r.getBottom() + 0.5f, 1.0f);
    }

    void Paint::pill (juce::Graphics& g, juce::Rectangle<float> r, bool down, bool hover, bool lit, bool enabled)
    {
        const float corner = r.getHeight() * 0.5f;
        // Drop shadow.
        g.setColour (juce::Colours::black.withAlpha (enabled ? 0.20f : 0.08f));
        g.fillRoundedRectangle (r.translated (0.0f, 1.2f), corner);

        juce::Colour top = lit ? juce::Colour (0xffffe3ee) : juce::Colour (0xfffafafb);
        juce::Colour bottom = lit ? juce::Colour (0xffff9fc3) : juce::Colour (0xffc6c6cc);
        if (hover && enabled) { top = top.brighter (0.05f); bottom = bottom.brighter (0.06f); }
        if (down) std::swap (top, bottom);
        if (! enabled) { top = top.withMultipliedAlpha (0.55f); bottom = bottom.withMultipliedAlpha (0.55f); }

        if (lit && enabled)
        {
            // Pink halo.
            for (int i = 4; i >= 1; --i)
            {
                g.setColour (Theme::accent.withAlpha (0.07f * (float) (5 - i)));
                g.fillRoundedRectangle (r.expanded ((float) i), corner + (float) i);
            }
        }

        juce::ColourGradient grad (top, r.getX(), r.getY(), bottom, r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r, corner);

        g.setColour (juce::Colours::white.withAlpha (enabled ? 0.9f : 0.4f));
        g.drawRoundedRectangle (r.reduced (1.0f), corner - 1.0f, 1.0f);
        g.setColour ((lit ? Theme::accentDeep : Theme::metalShadow).withAlpha (enabled ? 0.75f : 0.35f));
        g.drawRoundedRectangle (r.reduced (0.5f), corner, 1.0f);
    }

    void Paint::led (juce::Graphics& g, juce::Point<float> c, float radius, bool on)
    {
        // Recessed cup.
        g.setColour (Theme::metalShadow.withAlpha (0.6f));
        g.fillEllipse (juce::Rectangle<float> (radius * 2.6f, radius * 2.6f).withCentre (c));
        g.setColour (juce::Colours::white.withAlpha (0.6f));
        g.drawEllipse (juce::Rectangle<float> (radius * 2.6f, radius * 2.6f).withCentre (c.translated (0.0f, 0.6f)), 0.8f);
        if (on)
        {
            juce::ColourGradient halo (Theme::accent.withAlpha (0.55f), c.x, c.y, Theme::accent.withAlpha (0.0f), c.x + radius * 3.2f, c.y, true);
            g.setGradientFill (halo);
            g.fillEllipse (juce::Rectangle<float> (radius * 6.4f, radius * 6.4f).withCentre (c));
            juce::ColourGradient core (juce::Colours::white, c.x - radius * 0.3f, c.y - radius * 0.3f, Theme::accent, c.x + radius, c.y + radius, true);
            g.setGradientFill (core);
        }
        else
        {
            juce::ColourGradient core (juce::Colour (0xff8e8e96), c.x - radius * 0.3f, c.y - radius * 0.3f, juce::Colour (0xff4e4e55), c.x + radius, c.y + radius, true);
            g.setGradientFill (core);
        }
        g.fillEllipse (juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c));
    }

    void Paint::lightTube (juce::Graphics& g, juce::Rectangle<float> r, float level)
    {
        const float corner = r.getWidth() * 0.5f;
        groove (g, r.expanded (3.0f), corner + 3.0f);
        g.setColour (juce::Colour (0xff3b3436));
        g.fillRoundedRectangle (r, corner);
        auto litArea = r.withTrimmedTop (r.getHeight() * (1.0f - juce::jlimit (0.0f, 1.0f, level)));
        glowBar (g, litArea.reduced (1.0f, 2.0f), Theme::accentSoft, 6.0f, corner);
    }

    void Paint::grille (juce::Graphics& g, juce::Rectangle<float> r)
    {
        groove (g, r, juce::jmin (r.getWidth(), r.getHeight()) * 0.5f);
        const float step = 5.0f, hole = 2.2f;
        const auto inner = r.reduced (r.getWidth() * 0.22f, r.getWidth() * 0.4f);
        int row = 0;
        for (float y = inner.getY(); y <= inner.getBottom(); y += step * 0.866f, ++row)
            for (float x = inner.getX() + ((row & 1) ? step * 0.5f : 0.0f); x <= inner.getRight(); x += step)
            {
                g.setColour (juce::Colour (0xff2a2a2e));
                g.fillEllipse (x - hole * 0.5f, y - hole * 0.5f, hole, hole);
                g.setColour (juce::Colours::white.withAlpha (0.5f));
                g.drawLine (x - hole * 0.4f, y + hole * 0.6f, x + hole * 0.4f, y + hole * 0.6f, 0.6f);
            }
    }

    void Paint::screw (juce::Graphics& g, juce::Point<float> c, float radius)
    {
        auto r = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f).withCentre (c);
        juce::ColourGradient grad (Theme::metalShadow, r.getX(), r.getY(), juce::Colours::white, r.getRight(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillEllipse (r);
        juce::ColourGradient face (Theme::metalLight, r.getX(), r.getY(), Theme::metalDark, r.getRight(), r.getBottom(), false);
        g.setGradientFill (face);
        g.fillEllipse (r.reduced (radius * 0.18f));
        g.setColour (Theme::metalShadow);
        g.drawLine (c.x - radius * 0.5f, c.y, c.x + radius * 0.5f, c.y, juce::jmax (1.0f, radius * 0.22f));
    }

    void Paint::glowBar (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour core, float glow, float corner)
    {
        if (r.isEmpty()) return;
        const int steps = 4;
        for (int i = steps; i >= 1; --i)
        {
            const float e = glow * (float) i / (float) steps;
            g.setColour (core.withAlpha (0.10f * (float) (steps - i + 1) / (float) steps));
            g.fillRoundedRectangle (r.expanded (e), juce::jmin (corner + e, (r.getHeight() + 2.0f * e) * 0.5f));
        }
        juce::ColourGradient grad (core.interpolatedWith (juce::Colours::white, 0.55f), r.getX(), r.getY(),
                                   core, r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r, juce::jmin (corner, r.getHeight() * 0.5f));
    }

    void Paint::headphones (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        r = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), r.getHeight()), juce::jmin (r.getWidth(), r.getHeight()));
        const float w = r.getWidth();
        juce::Path band;
        band.addCentredArc (r.getCentreX(), r.getCentreY() + w * 0.08f, w * 0.40f, w * 0.40f, 0.0f,
                            -juce::MathConstants<float>::halfPi * 1.05f, juce::MathConstants<float>::halfPi * 1.05f, true);
        g.setColour (c);
        g.strokePath (band, juce::PathStrokeType (w * 0.11f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        g.fillRoundedRectangle (r.getX() + w * 0.05f, r.getY() + w * 0.48f, w * 0.22f, w * 0.42f, w * 0.08f);
        g.fillRoundedRectangle (r.getRight() - w * 0.27f, r.getY() + w * 0.48f, w * 0.22f, w * 0.42f, w * 0.08f);
    }

    void Paint::chevronDown (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        r = r.withSizeKeepingCentre (juce::jmin (r.getWidth(), 9.0f), juce::jmin (r.getHeight(), 5.0f));
        juce::Path p;
        p.startNewSubPath (r.getX(), r.getY());
        p.lineTo (r.getCentreX(), r.getBottom());
        p.lineTo (r.getRight(), r.getY());
        g.setColour (c);
        g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
    }

    void Paint::waveformGlyph (juce::Graphics& g, juce::Rectangle<float> r, juce::Colour c)
    {
        static const float heights[] = { 0.25f, 0.5f, 0.8f, 0.45f, 1.0f, 0.6f, 0.85f, 0.4f, 0.65f, 0.3f };
        const int n = (int) std::size (heights);
        const float step = r.getWidth() / (float) n;
        g.setColour (c);
        for (int i = 0; i < n; ++i)
        {
            const float h = r.getHeight() * heights[i];
            g.fillRoundedRectangle (r.getX() + step * (float) i + step * 0.3f, r.getCentreY() - h * 0.5f, step * 0.4f, h, step * 0.2f);
        }
    }

    //==========================================================================
    EngineerLookAndFeel::EngineerLookAndFeel()
    {
        setColourScheme ({ Theme::metal,            // windowBackground
                           Theme::metalLight,       // widgetBackground
                           Theme::metalLight,       // menuBackground
                           Theme::metalEdge,        // outline
                           Theme::ink,              // defaultText
                           Theme::accent,           // defaultFill
                           juce::Colours::white,    // highlightedText
                           Theme::accent,           // highlightedFill
                           Theme::ink });           // menuText

        setColour (juce::Label::textColourId, Theme::ink);
        setColour (juce::TextButton::textColourOffId, Theme::ink);
        setColour (juce::TextButton::textColourOnId, Theme::ink);
        setColour (juce::ComboBox::textColourId, Theme::ink);
        setColour (juce::ToggleButton::textColourId, Theme::ink);
        setColour (juce::ToggleButton::tickColourId, juce::Colours::white);
        setColour (juce::Slider::textBoxTextColourId, Theme::ink);
        setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
        setColour (juce::Slider::textBoxHighlightColourId, Theme::accentSoft);
        setColour (juce::PopupMenu::backgroundColourId, Theme::metalLight);
        setColour (juce::PopupMenu::textColourId, Theme::ink);
        setColour (juce::PopupMenu::headerTextColourId, Theme::ink);
        setColour (juce::PopupMenu::highlightedTextColourId, Theme::ink);
        setColour (juce::PopupMenu::highlightedBackgroundColourId, Theme::accentSoft);
        setColour (juce::TooltipWindow::textColourId, Theme::ink);
        setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xfff7f7f9));
        setColour (juce::TextEditor::textColourId, Theme::ink);
        setColour (juce::TextEditor::highlightColourId, Theme::accentSoft);
        setColour (juce::TextEditor::outlineColourId, Theme::metalEdge);
        setColour (juce::TextEditor::focusedOutlineColourId, Theme::accent);
        setColour (juce::CaretComponent::caretColourId, Theme::accentDeep);
        setColour (juce::AlertWindow::backgroundColourId, Theme::metal);
        setColour (juce::AlertWindow::textColourId, Theme::ink);
        setColour (juce::AlertWindow::outlineColourId, Theme::metalEdge);
        setColour (juce::ScrollBar::thumbColourId, Theme::accent);
        setColour (juce::ResizableWindow::backgroundColourId, Theme::metal);
    }

    juce::Typeface::Ptr EngineerLookAndFeel::getTypefaceForFont (const juce::Font& f)
    {
        if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
            return Theme::typeface (f.isBold());
        return LookAndFeel_V4::getTypefaceForFont (f);
    }

    //==========================================================================
    void EngineerLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& b, const juce::Colour&, bool hover, bool down)
    {
        auto r = b.getLocalBounds().toFloat().reduced (1.5f, 2.0f);
        // "glow": always lit pink (Hunt), the graph's one generative action.
        const bool lit = (b.getToggleState() && flag (b, "led")) || flag (b, "glow");
        Paint::pill (g, r, down, hover, lit, b.isEnabled());
        if (flag (b, "led"))
            Paint::led (g, { r.getX() + r.getHeight() * 0.55f, r.getCentreY() }, juce::jmin (4.0f, r.getHeight() * 0.16f), b.getToggleState());
    }

    juce::Font EngineerLookAndFeel::getTextButtonFont (juce::TextButton&, int h)
    {
        return Theme::font (juce::jmin (14.0f, (float) h * 0.48f), true);
    }

    void EngineerLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& b, bool, bool down)
    {
        auto r = b.getLocalBounds().reduced (2, 2);
        const bool lit = (b.getToggleState() && flag (b, "led")) || flag (b, "glow");
        if (flag (b, "chevron"))
        {
            // A menu arrow instead of text (Hunt options).
            Paint::chevronDown (g, r.toFloat().translated (0.0f, down ? 1.5f : 0.5f),
                                (lit ? Theme::accentDeep.darker (0.35f) : Theme::ink).withMultipliedAlpha (b.isEnabled() ? 0.9f : 0.4f));
            return;
        }
        if (flag (b, "led"))
            r.removeFromLeft (juce::roundToInt ((float) r.getHeight() * 0.9f));
        g.setFont (getTextButtonFont (b, b.getHeight()));
        g.setColour ((lit ? Theme::accentDeep.darker (0.35f) : Theme::ink).withMultipliedAlpha (b.isEnabled() ? 1.0f : 0.4f));
        g.drawText (b.getButtonText(), r.translated (0, down ? 1 : 0), juce::Justification::centred, false);
    }

    void EngineerLookAndFeel::drawToggleButton (juce::Graphics& g, juce::ToggleButton& b, bool hover, bool down)
    {
        if (flag (b, "led"))
        {
            // Pill with a light: used for Snap to scale.
            auto r = b.getLocalBounds().toFloat().reduced (1.5f, 2.0f);
            Paint::pill (g, r, down, hover, b.getToggleState(), b.isEnabled());
            Paint::led (g, { r.getX() + r.getHeight() * 0.55f, r.getCentreY() }, juce::jmin (4.0f, r.getHeight() * 0.16f), b.getToggleState());
            g.setFont (Theme::font (juce::jmin (13.5f, r.getHeight() * 0.5f), true));
            g.setColour (b.getToggleState() ? Theme::accentDeep.darker (0.35f) : Theme::ink);
            g.drawText (b.getButtonText(), r.withTrimmedLeft (r.getHeight() * 1.05f).withTrimmedRight (8.0f), juce::Justification::centredLeft, false);
            return;
        }

        // Check box on a dark screen (Follow).
        auto r = b.getLocalBounds().toFloat();
        const float s = juce::jmin (15.0f, r.getHeight() - 4.0f);
        auto box = juce::Rectangle<float> (r.getX() + 3.0f, r.getCentreY() - s * 0.5f, s, s);
        if (b.getToggleState())
        {
            Paint::glowBar (g, box, Theme::accent, 3.0f, s * 0.5f);
            juce::Path tick;
            tick.startNewSubPath (box.getX() + s * 0.27f, box.getCentreY());
            tick.lineTo (box.getX() + s * 0.44f, box.getY() + s * 0.70f);
            tick.lineTo (box.getX() + s * 0.74f, box.getY() + s * 0.32f);
            g.setColour (juce::Colours::white);
            g.strokePath (tick, juce::PathStrokeType (1.8f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else
        {
            g.setColour (juce::Colours::white.withAlpha (hover ? 0.5f : 0.35f));
            g.drawEllipse (box.reduced (0.5f), 1.2f);
        }
        g.setColour (b.findColour (juce::ToggleButton::textColourId));
        g.setFont (Theme::font (12.5f, true));
        g.drawText (b.getButtonText(), r.withTrimmedLeft (s + 9.0f), juce::Justification::centredLeft, false);
    }

    //==========================================================================
    void EngineerLookAndFeel::drawComboBox (juce::Graphics& g, int w, int h, bool down, int, int, int, int, juce::ComboBox& box)
    {
        auto r = juce::Rectangle<float> ((float) w, (float) h).reduced (1.5f, 2.0f);
        Paint::pill (g, r, down, box.isMouseOver (true), false, box.isEnabled());
        Paint::chevronDown (g, r.removeFromRight (r.getHeight() * 1.05f).translated (-2.0f, 0.5f),
                            Theme::ink.withMultipliedAlpha (box.isEnabled() ? 0.85f : 0.35f));
    }

    juce::Font EngineerLookAndFeel::getComboBoxFont (juce::ComboBox& box)
    {
        return Theme::font (juce::jmin (13.5f, (float) box.getHeight() * 0.46f), true);
    }

    void EngineerLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
    {
        label.setBounds (8, 1, box.getWidth() - 8 - juce::roundToInt ((float) box.getHeight() * 1.05f), box.getHeight() - 2);
        label.setFont (getComboBoxFont (box));
        label.setColour (juce::Label::textColourId, Theme::ink);
        label.setMinimumHorizontalScale (0.9f);
    }

    //==========================================================================
    void EngineerLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float, float,
                                                juce::Slider::SliderStyle style, juce::Slider& s)
    {
        if (style != juce::Slider::LinearHorizontal)
        {
            LookAndFeel_V4::drawLinearSlider (g, x, y, w, h, pos, 0.0f, 0.0f, style, s);
            return;
        }
        const bool on = s.isEnabled();
        const float cy = (float) y + (float) h * 0.5f;
        auto track = juce::Rectangle<float> ((float) x, cy - 2.5f, (float) w, 5.0f);
        g.setColour (Theme::metalShadow.withAlpha (0.55f));
        g.fillRoundedRectangle (track.expanded (1.0f), 3.5f);
        g.setColour (juce::Colour (0xff2b2a2e));
        g.fillRoundedRectangle (track, 2.5f);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.drawLine (track.getX() + 2.0f, track.getBottom() + 1.5f, track.getRight() - 2.0f, track.getBottom() + 1.5f, 0.8f);

        // Lit part: from the slider's zero (or its minimum) to the thumb.
        const double zero = juce::jlimit (s.getMinimum(), s.getMaximum(), 0.0);
        const float zeroX = (float) s.getPositionOfValue (zero) + (float) x - (float) s.getLocalBounds().getX();
        const float x0 = juce::jmin (zeroX, pos), x1 = juce::jmax (zeroX, pos);
        if (on && s.getValue() >= 0.0 && x1 - x0 > 0.5f)   // negative = "Auto" (Drift): nothing lit
            Paint::glowBar (g, juce::Rectangle<float> (x0, track.getY() + 0.5f, x1 - x0, track.getHeight() - 1.0f), Theme::accent, 3.5f, 2.0f);

        // Thumb: a polished ball with a pink centre.
        const float r = 7.5f;
        auto thumb = juce::Rectangle<float> (r * 2.0f, r * 2.0f).withCentre ({ pos, cy });
        g.setColour (juce::Colours::black.withAlpha (0.25f));
        g.fillEllipse (thumb.translated (0.0f, 1.5f));
        juce::ColourGradient ball (juce::Colours::white, thumb.getX() + r * 0.6f, thumb.getY() + r * 0.4f,
                                   Theme::metalDark, thumb.getRight(), thumb.getBottom(), true);
        g.setGradientFill (ball);
        g.fillEllipse (thumb);
        g.setColour (Theme::metalShadow);
        g.drawEllipse (thumb.reduced (0.4f), 0.8f);
        if (on)
            Paint::led (g, thumb.getCentre(), 2.6f, true);
    }

    void EngineerLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int w, int h, float pos, float start, float end, juce::Slider& s)
    {
        const bool big = flag (s, "bigKnob");
        auto bounds = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        const float size = juce::jmin (bounds.getWidth(), bounds.getHeight());
        const auto c = bounds.getCentre();
        const float ringR = size * 0.5f - (big ? 3.0f : 1.5f);
        const float knobR = ringR * (big ? 0.72f : 0.70f);
        const float angle = start + pos * (end - start);

        // Recessed ring the value glows in.
        juce::Path ringBg;
        ringBg.addCentredArc (c.x, c.y, ringR - 2.0f, ringR - 2.0f, 0.0f, start, end, true);
        g.setColour (juce::Colour (0xff2b2a2e).withAlpha (0.85f));
        g.strokePath (ringBg, juce::PathStrokeType (big ? 4.0f : 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));

        if (s.isEnabled() && pos > 0.001f)
        {
            juce::Path arc;
            arc.addCentredArc (c.x, c.y, ringR - 2.0f, ringR - 2.0f, 0.0f, start, angle, true);
            for (int i = 3; i >= 1; --i)
            {
                g.setColour (Theme::accent.withAlpha (0.12f * (float) (4 - i)));
                g.strokePath (arc, juce::PathStrokeType ((big ? 4.0f : 3.0f) + (float) i * 3.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            }
            g.setColour (Theme::accentSoft);
            g.strokePath (arc, juce::PathStrokeType (big ? 3.0f : 2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }

        // Tick dots at the ends and the middle (100% on the Amount knob).
        if (big)
            for (float t : { 0.0f, 0.5f, 1.0f })
            {
                const float a = start + t * (end - start);
                const auto p = c.getPointOnCircumference (ringR + 1.0f, a);
                g.setColour (Theme::ink.withAlpha (0.7f));
                g.fillEllipse (juce::Rectangle<float> (3.0f, 3.0f).withCentre (p));
            }

        // Knob body: shadow, bevel, spun-metal face.
        const auto body = juce::Rectangle<float> (knobR * 2.0f, knobR * 2.0f).withCentre (c);
        g.setColour (juce::Colours::black.withAlpha (0.28f));
        g.fillEllipse (body.translated (0.0f, big ? 3.0f : 1.5f).expanded (1.0f));
        juce::ColourGradient bevel (juce::Colours::white, body.getX(), body.getY(), Theme::metalShadow, body.getRight(), body.getBottom(), false);
        g.setGradientFill (bevel);
        g.fillEllipse (body);
        const auto face = body.reduced (knobR * 0.10f);
        juce::ColourGradient faceGrad (Theme::metalLight, face.getX(), face.getY(), Theme::metalDark, face.getRight(), face.getBottom(), false);
        g.setGradientFill (faceGrad);
        g.fillEllipse (face);
        // Concentric spin marks.
        const float fr = face.getWidth() * 0.5f;
        for (float rr = fr * 0.15f; rr < fr; rr += 1.6f)
        {
            const float a = 0.05f + 0.06f * std::sin (rr * 1.7f);
            g.setColour ((((int) (rr * 2.0f)) & 1) ? juce::Colours::white.withAlpha (a) : juce::Colours::black.withAlpha (a * 0.6f));
            g.drawEllipse (juce::Rectangle<float> (rr * 2.0f, rr * 2.0f).withCentre (c), 0.8f);
        }
        // Light sweep across the face.
        juce::ColourGradient sweep (juce::Colours::white.withAlpha (0.45f), c.x - fr * 0.6f, c.y - fr * 0.7f,
                                    juce::Colours::transparentWhite, c.x + fr * 0.2f, c.y + fr * 0.2f, true);
        g.setGradientFill (sweep);
        g.fillEllipse (face);
        g.setColour (Theme::metalShadow.withAlpha (0.6f));
        g.drawEllipse (face, 0.8f);

        // Pointer.
        const auto p0 = c.getPointOnCircumference (fr * 0.35f, angle);
        const auto p1 = c.getPointOnCircumference (fr * 0.88f, angle);
        g.setColour (s.isEnabled() ? Theme::accent : Theme::inkFaint);
        g.drawLine ({ p0, p1 }, big ? 2.6f : 1.8f);
    }

    juce::Slider::SliderLayout EngineerLookAndFeel::getSliderLayout (juce::Slider& s)
    {
        auto layout = LookAndFeel_V4::getSliderLayout (s);
        if (s.getSliderStyle() == juce::Slider::LinearHorizontal)
            layout.sliderBounds = layout.sliderBounds.reduced (2, 0);
        return layout;
    }

    namespace
    {
        // Slider value read-out drawn as a small recessed pill.
        struct PillValueLabel final : juce::Label
        {
            void paint (juce::Graphics& g) override
            {
                auto r = getLocalBounds().toFloat().reduced (1.0f, 1.0f);
                if (! isBeingEdited())
                {
                    Paint::pill (g, r, true, false, false, isEnabled());
                    g.setColour (Theme::ink.withMultipliedAlpha (isEnabled() ? 1.0f : 0.45f));
                    g.setFont (Theme::font (12.0f, true));
                    g.drawText (getText(), r, juce::Justification::centred, false);
                }
            }
        };
    }

    juce::Label* EngineerLookAndFeel::createSliderTextBox (juce::Slider& s)
    {
        auto* l = new PillValueLabel();
        l->setJustificationType (juce::Justification::centred);
        l->setKeyboardType (juce::TextInputTarget::decimalKeyboard);
        l->setColour (juce::Label::textColourId, Theme::ink);
        l->setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
        l->setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);
        l->setColour (juce::TextEditor::textColourId, Theme::ink);
        l->setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xfff7f7f9));
        l->setColour (juce::TextEditor::highlightColourId, Theme::accentSoft);
        l->setFont (Theme::font (12.0f, true));
        juce::ignoreUnused (s);
        return l;
    }

    //==========================================================================
    juce::Font EngineerLookAndFeel::getLabelFont (juce::Label& l)
    {
        const auto f = l.getFont();
        if (f.getTypefaceName() == juce::Font::getDefaultSansSerifFontName())
            return Theme::font (f.getHeight(), f.isBold());
        return f;
    }

    void EngineerLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
    {
        g.fillAll (label.findColour (juce::Label::backgroundColourId));
        if (label.isBeingEdited())
            return;
        const auto alpha = label.isEnabled() ? 1.0f : 0.5f;
        g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (alpha));
        g.setFont (getLabelFont (label));
        const auto area = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());
        g.drawFittedText (label.getText(), area, label.getJustificationType(),
                          juce::jmax (1, (int) ((float) area.getHeight() / g.getCurrentFont().getHeight())),
                          label.getMinimumHorizontalScale());
    }

    //==========================================================================
    void EngineerLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int w, int h)
    {
        const auto r = juce::Rectangle<float> ((float) w, (float) h);
        Paint::brushedMetal (g, r, 0.0f, 1.04f);
        g.setColour (juce::Colours::white.withAlpha (0.75f));
        g.drawRect (r.reduced (1.0f), 1.0f);
        g.setColour (Theme::metalEdge);
        g.drawRect (r, 1.0f);
    }

    juce::Font EngineerLookAndFeel::getPopupMenuFont()
    {
        return Theme::font (14.5f);
    }

    void EngineerLookAndFeel::getIdealPopupMenuItemSize (const juce::String& text, bool isSeparator, int standardHeight, int& w, int& h)
    {
        if (isSeparator)
        {
            w = 50;
            h = 9;
            return;
        }
        const auto f = getPopupMenuFont();
        h = standardHeight > 0 ? standardHeight : 26;
        w = juce::GlyphArrangement::getStringWidthInt (f, text) + 30 + 32;
    }

    void EngineerLookAndFeel::drawPopupMenuItem (juce::Graphics& g, const juce::Rectangle<int>& area, bool isSeparator, bool isActive,
                                                 bool isHighlighted, bool isTicked, bool hasSubMenu, const juce::String& text,
                                                 const juce::String& shortcut, const juce::Drawable* icon, const juce::Colour* textColour)
    {
        auto r = area.toFloat();
        if (isSeparator)
        {
            const float y = r.getCentreY();
            g.setColour (Theme::metalShadow.withAlpha (0.45f));
            g.drawLine (r.getX() + 10.0f, y, r.getRight() - 10.0f, y, 1.0f);
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            g.drawLine (r.getX() + 10.0f, y + 1.0f, r.getRight() - 10.0f, y + 1.0f, 1.0f);
            return;
        }

        if (isHighlighted && isActive)
        {
            auto hl = r.reduced (4.0f, 1.0f);
            juce::ColourGradient grad (juce::Colour (0xffffe6ef), hl.getX(), hl.getY(), juce::Colour (0xffffc2d8), hl.getX(), hl.getBottom(), false);
            g.setGradientFill (grad);
            g.fillRoundedRectangle (hl, 5.0f);
            g.setColour (Theme::accent.withAlpha (0.6f));
            g.drawRoundedRectangle (hl.reduced (0.5f), 5.0f, 1.0f);
            Paint::glowBar (g, juce::Rectangle<float> (hl.getX() + 2.0f, hl.getY() + 4.0f, 3.0f, hl.getHeight() - 8.0f), Theme::accent, 2.0f, 1.5f);
        }

        auto content = r.reduced (10.0f, 0.0f);
        auto tickArea = content.removeFromLeft (22.0f);
        if (isTicked)
        {
            const auto c = tickArea.getCentre();
            juce::Path tick;
            tick.startNewSubPath (c.x - 5.5f, c.y + 0.5f);
            tick.lineTo (c.x - 1.5f, c.y + 4.5f);
            tick.lineTo (c.x + 6.0f, c.y - 4.5f);
            g.setColour (Theme::accent.withAlpha (0.35f));
            g.strokePath (tick, juce::PathStrokeType (5.0f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
            g.setColour (Theme::accentDeep);
            g.strokePath (tick, juce::PathStrokeType (2.2f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        else if (icon != nullptr)
            icon->drawWithin (g, tickArea.reduced (4.0f), juce::RectanglePlacement::centred, 1.0f);

        auto colour = textColour != nullptr ? *textColour : Theme::ink;
        if (! isActive) colour = Theme::inkFaint;
        g.setColour (colour);
        g.setFont (getPopupMenuFont());

        if (hasSubMenu)
        {
            auto arrow = content.removeFromRight (16.0f);
            const auto c = arrow.getCentre();
            juce::Path p;
            p.startNewSubPath (c.x - 2.0f, c.y - 4.5f);
            p.lineTo (c.x + 2.5f, c.y);
            p.lineTo (c.x - 2.0f, c.y + 4.5f);
            g.strokePath (p, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved, juce::PathStrokeType::rounded));
        }
        g.drawFittedText (text, content.toNearestInt(), juce::Justification::centredLeft, 1, 0.9f);
        if (shortcut.isNotEmpty())
        {
            g.setColour (Theme::inkDim);
            g.setFont (Theme::font (12.5f));
            g.drawText (shortcut, content.toNearestInt(), juce::Justification::centredRight, true);
        }
    }

    void EngineerLookAndFeel::drawPopupMenuSectionHeader (juce::Graphics& g, const juce::Rectangle<int>& area, const juce::String& text)
    {
        auto r = area.toFloat().reduced (4.0f, 2.0f);
        juce::ColourGradient grad (Theme::metalDark.withAlpha (0.35f), r.getX(), r.getY(), Theme::metalDark.withAlpha (0.15f), r.getX(), r.getBottom(), false);
        g.setGradientFill (grad);
        g.fillRoundedRectangle (r, 4.0f);
        g.setColour (Theme::metalShadow.withAlpha (0.35f));
        g.drawRoundedRectangle (r.reduced (0.5f), 4.0f, 1.0f);
        g.setColour (Theme::ink);
        g.setFont (Theme::font (13.5f, true));
        g.drawFittedText (text, r.reduced (12.0f, 0.0f).toNearestInt(), juce::Justification::centredLeft, 1, 0.85f);
    }

    //==========================================================================
    void EngineerLookAndFeel::drawScrollbar (juce::Graphics& g, juce::ScrollBar&, int x, int y, int w, int h, bool vertical,
                                             int thumbStart, int thumbSize, bool over, bool down)
    {
        auto track = juce::Rectangle<float> ((float) x, (float) y, (float) w, (float) h);
        track = vertical ? track.withSizeKeepingCentre (5.0f, track.getHeight()) : track.withSizeKeepingCentre (track.getWidth(), 5.0f);
        g.setColour (juce::Colours::black.withAlpha (0.6f));
        g.fillRoundedRectangle (track, 2.5f);
        if (thumbSize <= 0) return;
        auto thumb = vertical ? juce::Rectangle<float> (track.getX(), (float) thumbStart, track.getWidth(), (float) thumbSize)
                              : juce::Rectangle<float> ((float) thumbStart, track.getY(), (float) thumbSize, track.getHeight());
        Paint::glowBar (g, thumb.reduced (vertical ? 0.0f : 1.0f, vertical ? 1.0f : 0.0f),
                        (over || down) ? Theme::accentSoft : Theme::accent, 4.0f, 2.5f);
    }

    juce::Rectangle<int> EngineerLookAndFeel::getTooltipBounds (const juce::String& tip, juce::Point<int> pos, juce::Rectangle<int> parent)
    {
        juce::AttributedString s;
        s.append (tip, Theme::font (13.0f), Theme::ink);
        juce::TextLayout tl;
        tl.createLayoutWithBalancedLineLengths (s, 360.0f);
        const int w = (int) (tl.getWidth() + 22.0f), h = (int) (tl.getHeight() + 14.0f);
        return juce::Rectangle<int> (pos.x > parent.getCentreX() ? pos.x - (w + 12) : pos.x + 24,
                                     pos.y > parent.getCentreY() ? pos.y - (h + 6) : pos.y + 6, w, h)
                   .constrainedWithin (parent);
    }

    void EngineerLookAndFeel::drawTooltip (juce::Graphics& g, const juce::String& text, int w, int h)
    {
        const auto r = juce::Rectangle<float> ((float) w, (float) h);
        Paint::brushedMetal (g, r, 0.0f, 1.06f);
        g.setColour (Theme::metalEdge);
        g.drawRect (r, 1.0f);
        juce::AttributedString s;
        s.setJustification (juce::Justification::centredLeft);
        s.append (text, Theme::font (13.0f), Theme::ink);
        juce::TextLayout tl;
        tl.createLayoutWithBalancedLineLengths (s, (float) w - 22.0f);
        tl.draw (g, r.reduced (11.0f, 7.0f));
    }

    void EngineerLookAndFeel::drawCornerResizer (juce::Graphics& g, int w, int h, bool over, bool dragging)
    {
        const float s = (float) juce::jmin (w, h);
        for (float i = 0.3f; i < 1.0f; i += 0.25f)
        {
            g.setColour (juce::Colours::white.withAlpha (0.8f));
            g.drawLine ((float) w - s * i + 1.0f, (float) h, (float) w, (float) h - s * i + 1.0f, 1.0f);
            g.setColour ((over || dragging ? Theme::accentDeep : Theme::metalShadow).withAlpha (0.8f));
            g.drawLine ((float) w - s * i, (float) h, (float) w, (float) h - s * i, 1.0f);
        }
    }

    //==========================================================================
    void EngineerLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int w, int h, juce::TextEditor& e)
    {
        auto r = juce::Rectangle<float> ((float) w, (float) h).reduced (0.5f);
        g.setColour (e.findColour (juce::TextEditor::backgroundColourId));
        g.fillRoundedRectangle (r, 5.0f);
        juce::ColourGradient inner (juce::Colours::black.withAlpha (0.12f), 0.0f, 0.0f, juce::Colours::transparentBlack, 0.0f, 6.0f, false);
        g.setGradientFill (inner);
        g.fillRoundedRectangle (r.withHeight (8.0f), 5.0f);
    }

    void EngineerLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int w, int h, juce::TextEditor& e)
    {
        auto r = juce::Rectangle<float> ((float) w, (float) h).reduced (0.5f);
        const bool focus = e.hasKeyboardFocus (true) && ! e.isReadOnly();
        g.setColour (focus ? Theme::accent : Theme::metalEdge);
        g.drawRoundedRectangle (r, 5.0f, focus ? 1.5f : 1.0f);
    }

    void EngineerLookAndFeel::drawAlertBox (juce::Graphics& g, juce::AlertWindow& alert, const juce::Rectangle<int>& textArea, juce::TextLayout& layout)
    {
        const auto r = alert.getLocalBounds().toFloat();
        Paint::brushedMetal (g, r, 0.0f, 1.03f);
        g.setColour (juce::Colours::white.withAlpha (0.8f));
        g.drawRect (r.reduced (1.0f), 1.0f);
        g.setColour (Theme::metalEdge);
        g.drawRect (r, 1.0f);
        g.setColour (Theme::ink);
        layout.draw (g, textArea.toFloat());
    }

    juce::Font EngineerLookAndFeel::getAlertWindowTitleFont()   { return Theme::font (17.0f, true); }
    juce::Font EngineerLookAndFeel::getAlertWindowMessageFont() { return Theme::font (14.0f); }
    juce::Font EngineerLookAndFeel::getAlertWindowFont()        { return Theme::font (13.5f); }
}
