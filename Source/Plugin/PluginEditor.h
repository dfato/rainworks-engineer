#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "UI/ControlBar.h"
#include "UI/HeaderComponent.h"
#include "UI/HostInfoComponent.h"
#include "UI/StatusBar.h"
#include "UI/TimelineComponent.h"

// Wires processor state to UI components. The components themselves never
// see the processor.
class VocalEngineEditor final : public juce::AudioProcessorEditor,
                                private juce::Timer,
                                private juce::ChangeListener
{
public:
    explicit VocalEngineEditor (VocalEngineProcessor&);
    ~VocalEngineEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildFromSession();

    VocalEngineProcessor& processor;
    ve::ui::HeaderComponent header;
    ve::ui::ControlBar controls;
    ve::ui::HostInfoComponent hostInfo;
    ve::ui::TimelineComponent timeline;
    ve::ui::StatusBar statusBar;
    juce::TooltipWindow tooltips { this };
    std::unique_ptr<juce::FileChooser> chooser;

    ve::HostSnapshot lastHost;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalEngineEditor)
};
