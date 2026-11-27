#include "ControlBar.h"
#include "Theme.h"
#include "EngineerLookAndFeel.h"
#include "Engine/SidechainAnalysis.h"
#include <map>

namespace ve::ui
{
    ControlBar::ControlBar()
    {
        armButton.setClickingTogglesState (true);
        armButton.getProperties().set ("led", true);
        armButton.setTooltip ("While armed, playing the host transport records the vocal and sidechain into a new take");
        armButton.onClick = [this] { if (onArmChanged) onArmChanged (armButton.getToggleState()); };
        addAndMakeVisible (armButton);

        correctionSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        correctionSlider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        correctionSlider.setRange (0.0, 100.0, 1.0);
        correctionSlider.setTextValueSuffix ("%");
        correctionSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 46, 22);
        correctionSlider.setTooltip ("Pitch correction for every note you haven't edited by hand. 0% = as sung, 100% = on the key.");
        correctionSlider.onValueChange = [this] { if (onCorrectionChanged) onCorrectionChanged ((float) correctionSlider.getValue() / 100.0f); };
        addAndMakeVisible (correctionSlider);

        // The big centre knob. Its read-out ("Amount 100%") can be clicked and typed into.
        amountSlider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
        amountSlider.setRotaryParameters (juce::MathConstants<float>::pi * 1.25f, juce::MathConstants<float>::pi * 2.75f, true);
        amountSlider.getProperties().set ("bigKnob", true);
        amountSlider.setRange (0.0, 200.0, 1.0);
        amountSlider.setValue (100.0, juce::dontSendNotification);
        amountSlider.setDoubleClickReturnValue (true, 100.0);
        amountSlider.textFromValueFunction = [] (double v) { return "Amount " + juce::String (juce::roundToInt (v)) + "%"; };
        amountSlider.valueFromTextFunction = [] (const juce::String& t) { return t.retainCharacters ("0123456789.").getDoubleValue(); };
        amountSlider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, 104, 22);
        amountSlider.updateText();
        amountSlider.setTooltip ("How strongly the style is applied: its vibrato, scoops, runs, falls and tone. "
                                 "0% = tuning only, 100% = as designed, 200% = exaggerated. Double-click for 100%.");
        amountSlider.onValueChange = [this] { if (onStyleAmountChanged) onStyleAmountChanged ((float) amountSlider.getValue() / 100.0f); };
        addAndMakeVisible (amountSlider);

        styleBox.setTooltip ("Genre style: how hard notes snap, how fast, slides, scoops and vibrato. Picking one sets Correct to its default.");
        styleBox.onChange = [this]
        {
            if (styleBox.getSelectedId() == learnStyleId)
            {
                setStyle (currentStyleId);   // the menu entry is an action, not a style
                if (onLearnStyle) onLearnStyle();
                return;
            }
            const int idx = styleBox.getSelectedId() - 1;
            if (idx >= 0 && idx < styleIds.size())
            {
                currentStyleId = styleIds[idx];
                if (onStyleChanged) onStyleChanged (styleIds[idx]);
            }
        };
        addAndMakeVisible (styleBox);

        intentButton.setClickingTogglesState (true);
        intentButton.setToggleState (true, juce::dontSendNotification);
        intentButton.getProperties().set ("led", true);
        intentButton.setTooltip ("Intent: read what each note was meant to be. Slides, vibrato, scoops, passing notes, "
                                 "chosen out-of-key notes and spoken lines are kept as sung or corrected gently. "
                                 "Off: every note is pulled to the key the same way.");
        intentButton.onClick = [this] { if (onIntentChanged) onIntentChanged (intentButton.getToggleState()); };
        addAndMakeVisible (intentButton);

        voiceBox.onChange = [this]
        {
            const int sel = voiceBox.getSelectedId();
            if (sel == rescanId) { setVoice (currentVoice); if (onRescanVoices) onRescanVoices(); return; }
            if (sel == openFolderId) { setVoice (currentVoice); if (onOpenVoicesFolder) onOpenVoicesFolder(); return; }
            if (sel == gpuId)
            {
                setVoice (currentVoice);
                setGpu (! gpuOn, gpuSupported);
                if (onGpuChanged) onGpuChanged (gpuOn);
                return;
            }
            const int idx = sel - 1;
            if (idx >= 0 && idx < voiceIds.size())
            {
                currentVoice = voiceIds[idx];
                if (onVoiceChanged) onVoiceChanged (currentVoice);
            }
        };
        addAndMakeVisible (voiceBox);
        setVoices ({}, false, {});

        for (auto* l : { &modeLabel, &keyLabel, &correctionLabel, &styleLabel, &voiceLabel, &amountLabel })
        {
            l->setColour (juce::Label::textColourId, Theme::ink);
            l->setFont (Theme::font (13.0f));
            l->setJustificationType (juce::Justification::centredLeft);
            l->setBorderSize ({ 0, 4, 0, 0 });
            l->setInterceptsMouseClicks (false, false);
            addAndMakeVisible (*l);
        }

        modeBox.setName ("monitor");
        keyBox.setName ("key");
        styleBox.setName ("style");
        voiceBox.setName ("voice");
        modeBox.addItem ("Input", 1);
        modeBox.addItem ("Rendered", 2);
        modeBox.addItem ("Bypass", 3);
        modeBox.setTooltip ("Input: hear the live input. Rendered: hear cached takes in sync with the host. Bypass: untouched input, capture disabled.");
        modeBox.onChange = [this] { if (onModeChanged) onModeChanged (modeBox.getSelectedId() - 1); };
        addAndMakeVisible (modeBox);

        keyBox.addItem ("Auto", 1);
        for (int i = 0; i < 12; ++i) keyBox.addItem (Analysis::keyName (i, false), 2 + i);
        for (int i = 0; i < 12; ++i) keyBox.addItem (Analysis::keyName (i, true), 14 + i);
        keyBox.setTooltip ("Manual key override. Auto uses the key detected from the sidechain.");
        keyBox.onChange = [this] { if (onKeyOverrideChanged) onKeyOverrideChanged (keyBox.getSelectedId() - 2); };
        addAndMakeVisible (keyBox);

        analyseButton.onClick = [this] { if (onAnalyse) onAnalyse(); };
        setCanAnalyse (false);

        importButton.setTooltip ("Bring in a whole song at once: pick the vocal file, and optionally the beat. You can also drag files onto the plugin.");
        importButton.onClick = [this] { if (onImport) onImport(); };
        addAndMakeVisible (importButton);
        addAndMakeVisible (analyseButton);

        folderButton.onClick = [this]
        {
            juce::PopupMenu m;
            m.addSectionHeader (folderText.isNotEmpty() ? folderText : "Data folder");
            m.addItem (1, "Show in file browser");
            m.addItem (2, "Locate / change data folder...");
            m.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (&folderButton), [this] (int r)
            {
                if (r == 1 && onOpenDataFolder) onOpenDataFolder();
                if (r == 2 && onLocateDataFolder) onLocateDataFolder();
            });
        };
        addAndMakeVisible (folderButton);
    }

    void ControlBar::setArmed (bool a) { armButton.setToggleState (a, juce::dontSendNotification); armButton.setButtonText (a ? "Armed" : "Arm"); }
    void ControlBar::setMode (int m) { modeBox.setSelectedId (m + 1, juce::dontSendNotification); }
    void ControlBar::setKeyOverride (int k) { keyBox.setSelectedId (k + 2, juce::dontSendNotification); }
    void ControlBar::setEstimatedKeyText (const juce::String& t) { keyBox.changeItemText (1, "Auto (" + t + ")"); if (keyBox.getSelectedId() == 1) keyBox.setSelectedId (1, juce::dontSendNotification); }
    void ControlBar::setCanAnalyse (bool anyTakeHasBeat)
    {
        analyseButton.setEnabled (anyTakeHasBeat);
        analyseButton.setTooltip (anyTakeHasBeat
            ? "Re-run key and chord detection on the beat saved with every take. It already runs by itself after each "
              "take; use this after changing the beat. The status bar shows the result."
            : "No take has a beat yet. Import the song with a beat file, or route the beat into the sidechain and "
              "record a take; key and chords are then detected automatically.");
    }

    void ControlBar::setStyles (const std::vector<StyleItem>& items)
    {
        styleBox.clear (juce::dontSendNotification);
        styleIds.clear();
        juce::String category;
        for (auto& it : items)
        {
            if (it.category != category)
            {
                category = it.category;
                styleBox.addSectionHeading (category);
            }
            styleIds.add (it.id);
            styleBox.addItem (it.name, styleIds.size());
        }
        styleBox.addSeparator();
        styleBox.addItem ("Learn style from vocals...", learnStyleId);
        setStyle (currentStyleId);
    }

    void ControlBar::setStyle (const juce::String& id)
    {
        currentStyleId = id;
        const int idx = styleIds.indexOf (id);
        styleBox.setSelectedId (idx >= 0 ? idx + 1 : 0, juce::dontSendNotification);
    }

    void ControlBar::setVoices (const std::vector<VoiceItem>& items, bool available, const juce::String& reason)
    {
        voiceBox.clear (juce::dontSendNotification);
        voiceIds.clear();
        voiceIds.add ({});
        voiceBox.addItem ("As recorded", 1);
        if (! items.empty())
            voiceBox.addSectionHeading ("Voice models");
        for (auto& v : items)
        {
            voiceIds.add (v.id);
            voiceBox.addItem (v.name, voiceIds.size());
            voiceBox.setItemEnabled (voiceIds.size(), available);
        }
        voiceBox.addSeparator();
        voiceBox.addItem ("Rescan voices folder", rescanId);
        voiceBox.addItem ("Open voices folder...", openFolderId);
        voiceBox.addItem (gpuLabel(), gpuId);
        voiceBox.setItemEnabled (gpuId, gpuSupported);
        voiceBox.setTooltip (available
            ? juce::String ("Re-sing every take in a trained voice model, following the notes, style and timing. Models live in Documents\\RainWorks\\Engineer\\Voices.")
            : "Voice models can't run in this build: " + reason);
        setVoice (currentVoice);
    }

    juce::String ControlBar::gpuLabel() const
    {
        if (! gpuSupported) return "Render on GPU: not in this build";
        return gpuOn ? "Render on GPU: on (click for CPU)" : "Render on GPU: off (click to use GPU)";
    }

    void ControlBar::setGpu (bool on, bool supported)
    {
        gpuOn = on;
        gpuSupported = supported;
        voiceBox.changeItemText (gpuId, gpuLabel());
        voiceBox.setItemEnabled (gpuId, gpuSupported);
    }

    void ControlBar::setVoice (const juce::String& id)
    {
        currentVoice = id;
        const int idx = voiceIds.indexOf (id);
        voiceBox.setSelectedId (idx >= 0 ? idx + 1 : 1, juce::dontSendNotification);
    }

    void ControlBar::setIntent (bool on) { intentButton.setToggleState (on, juce::dontSendNotification); }

    void ControlBar::setCorrection (float a) { correctionSlider.setValue (a * 100.0, juce::dontSendNotification); }
    void ControlBar::setStyleAmount (float a) { amountSlider.setValue (a * 100.0, juce::dontSendNotification); }
    void ControlBar::setDataFolderText (const juce::String& t) { folderText = t; folderButton.setTooltip (t); }

    void ControlBar::paint (juce::Graphics& g)
    {
        for (auto& r : grooves)
            Paint::groove (g, r.toFloat(), (float) r.getHeight() * 0.5f);
        if (! tray.isEmpty())
            Paint::groove (g, tray.toFloat(), (float) tray.getHeight() * 0.5f);
        Paint::headphones (g, headphoneIcon.toFloat(), Theme::ink);
    }

    void ControlBar::resized()
    {
        // The Amount knob sits in the plate's centre tab; the control row runs either side of it.
        const int w = getWidth(), cx = w / 2;
        const int rowH = 34;
        const int rowY = getHeight() - rowH - 14;
        const int knobHalf = 60;
        amountSlider.setBounds (cx - knobHalf, 6, knobHalf * 2, getHeight() - 10);

        grooves.clear();
        tray = {};

        // A run of items laid out left to right. Component widths shrink together
        // (down to 80%) if the editor is narrow; gaps stay fixed.
        // A gap of width -1 is elastic: on a wide editor it takes the spare room.
        struct Item { juce::Component* c; int width; int group; };   // c == nullptr: gap
        auto layout = [&] (std::vector<Item> items, int x0, int x1)
        {
            int fixed = 0, flex = 0, elastic = 0;
            for (auto& it : items)
            {
                if (it.width < 0) ++elastic;
                else (it.c != nullptr ? flex : fixed) += it.width;
            }
            const float scale = juce::jlimit (0.8f, 1.0f, (float) (x1 - x0 - fixed) / (float) juce::jmax (1, flex));
            const int spare = juce::jmax (0, x1 - x0 - fixed - juce::roundToInt ((float) flex * scale));
            for (auto& it : items)
                if (it.width < 0) it.width = spare / juce::jmax (1, elastic);
            int x = x0;
            std::map<int, juce::Rectangle<int>> groups;
            for (auto& it : items)
            {
                const int iw = it.c != nullptr ? juce::roundToInt ((float) it.width * scale) : it.width;
                const juce::Rectangle<int> b (x, rowY, iw, rowH);
                if (it.c != nullptr) it.c->setBounds (b);
                if (it.group > 0) groups[it.group] = groups[it.group].isEmpty() ? b : groups[it.group].getUnion (b);
                x += iw;
            }
            for (auto& [id, r] : groups)
            {
                if (id == 99) tray = r.expanded (2, 1);
                else grooves.push_back (r.expanded (2, 1));
            }
        };

        layout ({ { &armButton, 86, 0 }, { nullptr, 12, 0 },
                  { nullptr, 4, 1 }, { &modeLabel, 84, 1 }, { &modeBox, 102, 1 },
                  { nullptr, 10, 0 },
                  { nullptr, 4, 2 }, { &keyLabel, 34, 2 }, { &keyBox, 142, 2 },
                  { nullptr, 10, 0 },
                  { nullptr, 4, 3 }, { &styleLabel, 44, 3 }, { &styleBox, 134, 3 } },
                18, cx - knobHalf - 6);

        layout ({ { nullptr, 4, 4 }, { &voiceLabel, 44, 4 }, { &voiceBox, 128, 4 },
                  { nullptr, 10, 0 },
                  { nullptr, 4, 5 }, { &correctionLabel, 58, 5 }, { &correctionSlider, 80, 5 }, { nullptr, 2, 5 },
                  { nullptr, 10, 0 },
                  { &intentButton, 82, 0 },
                  { nullptr, 12, 0 }, { nullptr, -1, 0 },
                  { nullptr, 4, 99 }, { &importButton, 106, 99 }, { nullptr, 2, 99 }, { &analyseButton, 100, 99 },
                  { nullptr, 2, 99 }, { &folderButton, 92, 99 }, { nullptr, 4, 99 } },
                cx + knobHalf + 6, w - 18);

        // "Monitor" + headphones: the icon follows the word.
        auto ml = modeLabel.getBounds();
        const int textW = juce::GlyphArrangement::getStringWidthInt (Theme::font (13.0f), modeLabel.getText());
        headphoneIcon = juce::Rectangle<int> (ml.getX() + 4 + textW + 6, ml.getY(), 18, ml.getHeight()).withSizeKeepingCentre (16, 16);
    }
}
