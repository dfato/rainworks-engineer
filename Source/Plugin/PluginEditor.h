#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "UI/HeaderComponent.h"

class VocalEngineEditor final : public juce::AudioProcessorEditor,
                                private juce::Timer
{
public:
    explicit VocalEngineEditor (VocalEngineProcessor&);
    ~VocalEngineEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;

    VocalEngineProcessor& processor;
    ve::ui::HeaderComponent header;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalEngineEditor)
};
