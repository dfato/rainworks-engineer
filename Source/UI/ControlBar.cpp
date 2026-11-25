#include "ControlBar.h"
#include "Theme.h"
#include "Engine/SidechainAnalysis.h"

namespace ve::ui
{
    ControlBar::ControlBar()
    {
        armButton.setClickingTogglesState (true);
        armButton.setColour (juce::TextButton::buttonOnColourId, Theme::record);
        armButton.setTooltip ("While armed, playing the host transport records the vocal and sidechain into a new take");
        armButton.onClick = [this] { if (onArmChanged) onArmChanged (armButton.getToggleState()); };
        addAndMakeVisible (armButton);

        correctionSlider.setRange (0.0, 100.0, 1.0);
        correctionSlider.setTextValueSuffix ("%");
        correctionSlider.setTextBoxStyle (juce::Slider::TextBoxRight, false, 44, 20);
        correctionSlider.setTooltip ("Pitch correction for every note you haven't edited by hand. 0% = as sung, 100% = on the key.");
        correctionSlider.onValueChange = [this] { if (onCorrectionChanged) onCorrectionChanged ((float) correctionSlider.getValue() / 100.0f); };
        addAndMakeVisible (correctionSlider);

        styleBox.setTooltip ("Genre style: how hard notes snap, how fast, slides, scoops and vibrato. Picking one sets Correct to its default.");
        styleBox.onChange = [this]
        {
            const int idx = styleBox.getSelectedId() - 1;
            if (idx >= 0 && idx < styleIds.size() && onStyleChanged) onStyleChanged (styleIds[idx]);
        };
        addAndMakeVisible (styleBox);

        for (auto* l : { &modeLabel, &keyLabel, &correctionLabel, &styleLabel })
        {
            l->setColour (juce::Label::textColourId, Theme::textDim);
            l->setJustificationType (juce::Justification::centredRight);
            addAndMakeVisible (*l);
        }

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
    }

    void ControlBar::setStyle (const juce::String& id)
    {
        const int idx = styleIds.indexOf (id);
        styleBox.setSelectedId (idx >= 0 ? idx + 1 : 0, juce::dontSendNotification);
    }

    void ControlBar::setCorrection (float a) { correctionSlider.setValue (a * 100.0, juce::dontSendNotification); }
    void ControlBar::setDataFolderText (const juce::String& t) { folderText = t; folderButton.setTooltip (t); }

    void ControlBar::paint (juce::Graphics& g)
    {
        g.setColour (Theme::panel);
        g.fillRoundedRectangle (getLocalBounds().toFloat(), 4.0f);
    }

    void ControlBar::resized()
    {
        auto r = getLocalBounds().reduced (6, 5);
        armButton.setBounds (r.removeFromLeft (80));
        r.removeFromLeft (8);
        modeLabel.setBounds (r.removeFromLeft (60));
        modeBox.setBounds (r.removeFromLeft (100));
        r.removeFromLeft (8);
        keyLabel.setBounds (r.removeFromLeft (36));
        keyBox.setBounds (r.removeFromLeft (130));
        r.removeFromLeft (8);
        styleLabel.setBounds (r.removeFromLeft (40));
        styleBox.setBounds (r.removeFromLeft (150));
        r.removeFromLeft (8);
        correctionLabel.setBounds (r.removeFromLeft (52));
        correctionSlider.setBounds (r.removeFromLeft (140));
        r.removeFromLeft (8);
        folderButton.setBounds (r.removeFromRight (100));
        r.removeFromRight (8);
        analyseButton.setBounds (r.removeFromRight (128));
        r.removeFromRight (8);
        importButton.setBounds (r.removeFromRight (110));
    }
}
