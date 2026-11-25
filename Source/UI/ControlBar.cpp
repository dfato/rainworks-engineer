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

        for (auto* l : { &modeLabel, &keyLabel })
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
        modeBox.setBounds (r.removeFromLeft (110));
        r.removeFromLeft (8);
        keyLabel.setBounds (r.removeFromLeft (36));
        keyBox.setBounds (r.removeFromLeft (160));
        r.removeFromLeft (8);
        folderButton.setBounds (r.removeFromRight (100));
        r.removeFromRight (8);
        analyseButton.setBounds (r.removeFromRight (140));
    }
}
