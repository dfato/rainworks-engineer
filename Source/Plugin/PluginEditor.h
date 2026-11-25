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
                                public juce::FileDragAndDropTarget,
                                private juce::Timer,
                                private juce::ChangeListener
{
public:
    explicit VocalEngineEditor (VocalEngineProcessor&);
    ~VocalEngineEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    bool isInterestedInFileDrag (const juce::StringArray&) override;
    void fileDragEnter (const juce::StringArray&, int, int) override { dragHover = true; repaint(); }
    void fileDragExit (const juce::StringArray&) override              { dragHover = false; repaint(); }
    void filesDropped (const juce::StringArray&, int, int) override;
    void paintOverChildren (juce::Graphics&) override;

private:
    void timerCallback() override;
    void changeListenerCallback (juce::ChangeBroadcaster*) override;
    void rebuildFromSession();
    void refreshVoices();

    VocalEngineProcessor& processor;
    ve::ui::HeaderComponent header;
    ve::ui::ControlBar controls;
    ve::ui::HostInfoComponent hostInfo;
    ve::ui::TimelineComponent timeline;
    ve::ui::StatusBar statusBar;
    juce::TooltipWindow tooltips { this };
    std::unique_ptr<juce::FileChooser> chooser;

    ve::HostSnapshot lastHost;
    bool dragHover = false;

    void importFiles (const juce::Array<juce::File>&);

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalEngineEditor)
};
