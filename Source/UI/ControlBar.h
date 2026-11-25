#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace ve::ui
{
    // Arm, monitoring mode, key override, analysis and data folder controls.
    // Talks to the outside only through the std::function callbacks.
    class ControlBar final : public juce::Component
    {
    public:
        ControlBar();

        std::function<void (bool)> onArmChanged;
        std::function<void (int)> onModeChanged;          // 0 input, 1 rendered, 2 bypass
        std::function<void (int)> onKeyOverrideChanged;   // -1 auto, 0-11 major, 12-23 minor
        std::function<void()> onAnalyse;
        std::function<void()> onImport;
        std::function<void (float)> onCorrectionChanged;  // 0..1
        std::function<void()> onOpenDataFolder;
        std::function<void()> onLocateDataFolder;

        void setArmed (bool);
        void setMode (int);
        void setKeyOverride (int);
        void setEstimatedKeyText (const juce::String&);
        void setDataFolderText (const juce::String&);
        void setCorrection (float);

        void resized() override;
        void paint (juce::Graphics&) override;

    private:
        juce::TextButton armButton { "Arm" };
        juce::Label modeLabel { {}, "Monitor" };
        juce::ComboBox modeBox;
        juce::Label keyLabel { {}, "Key" };
        juce::ComboBox keyBox;
        juce::TextButton analyseButton { "Analyse sidechain" };
        juce::TextButton importButton { "Import song..." };
        juce::Label correctionLabel { {}, "Correct" };
        juce::Slider correctionSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::TextButton folderButton { "Data folder" };
        juce::String folderText;
    };
}
