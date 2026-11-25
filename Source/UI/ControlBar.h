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
        std::function<void (const juce::String&)> onStyleChanged;
        std::function<void (bool)> onIntentChanged;
        void setIntent (bool);

        struct StyleItem { juce::String id, name, category; };
        void setStyles (const std::vector<StyleItem>&);
        void setStyle (const juce::String& id);
        // Stage 5 voice models. Empty id = the recorded voice.
        struct VoiceItem { juce::String id, name; };
        std::function<void (const juce::String&)> onVoiceChanged;
        std::function<void()> onRescanVoices, onOpenVoicesFolder;
        void setVoices (const std::vector<VoiceItem>&, bool available, const juce::String& unavailableReason);
        void setVoice (const juce::String& id);

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
        juce::TextButton analyseButton { "Analyse beat" };
        juce::TextButton importButton { "Import song..." };
        juce::Label correctionLabel { {}, "Correct" };
        juce::Label styleLabel { {}, "Style" };
        juce::ComboBox styleBox;
        juce::StringArray styleIds;
        static constexpr int rescanId = 9001, openFolderId = 9002;
        juce::TextButton intentButton { "Intent" };
        juce::Label voiceLabel { {}, "Voice" };
        juce::ComboBox voiceBox;
        juce::StringArray voiceIds;   // index i = combo id i + 1; "" = as recorded
        juce::String currentVoice;
        juce::Slider correctionSlider { juce::Slider::LinearHorizontal, juce::Slider::TextBoxRight };
        juce::TextButton folderButton { "Data folder" };
        juce::String folderText;
    };
}
