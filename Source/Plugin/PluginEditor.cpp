#include "PluginEditor.h"

VocalEngineEditor::VocalEngineEditor (VocalEngineProcessor& p)
    : juce::AudioProcessorEditor (p),
      processor (p),
      header (JucePlugin_Name, JucePlugin_VersionString)
{
    addAndMakeVisible (header);
    setResizable (true, true);
    setResizeLimits (480, 240, 2400, 1600);
    setSize (720, 360);
    startTimerHz (15);
}

VocalEngineEditor::~VocalEngineEditor()
{
    stopTimer();
}

void VocalEngineEditor::paint (juce::Graphics& g)
{
    g.fillAll (ve::ui::HeaderComponent::backgroundColour());
}

void VocalEngineEditor::resized()
{
    header.setBounds (getLocalBounds());
}

void VocalEngineEditor::timerCallback()
{
    const auto s = processor.getStatus();
    header.setStatus ({ s.sidechainEnabled, s.sidechainPeak, s.mainInputChannels });
}
