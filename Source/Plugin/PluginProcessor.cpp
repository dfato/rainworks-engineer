#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Engine/EngineConfig.h"

VocalEngineProcessor::BusesProperties VocalEngineProcessor::makeBusesProperties()
{
    return BusesProperties()
        .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
        .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), true);
}

VocalEngineProcessor::VocalEngineProcessor()
    : juce::AudioProcessor (makeBusesProperties())
{
}

void VocalEngineProcessor::prepareToPlay (double, int)
{
    // Playback comes from cache and capture is offline, so nothing adds delay.
    setLatencySamples (0);
    sidechainPeak.store (0.0f, std::memory_order_relaxed);
}

void VocalEngineProcessor::releaseResources() {}

bool VocalEngineProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn  = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    // Main: mono or stereo vocal, output must match input.
    if (mainIn != juce::AudioChannelSet::mono() && mainIn != juce::AudioChannelSet::stereo())
        return false;
    if (mainOut != mainIn)
        return false;

    // Sidechain: stereo, or disabled so the plugin works without it.
    if (layouts.inputBuses.size() > ve::Bus::sidechain)
    {
        const auto sc = layouts.getChannelSet (true, ve::Bus::sidechain);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

void VocalEngineProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // Main output shares channels with main input, so passthrough is implicit.
    // Clear any output channels that have no matching main input channel.
    const auto mainIn  = getBusBuffer (buffer, true,  ve::Bus::mainInput);
    auto       mainOut = getBusBuffer (buffer, false, ve::Bus::mainOutput);
    for (int ch = mainIn.getNumChannels(); ch < mainOut.getNumChannels(); ++ch)
        mainOut.clear (ch, 0, mainOut.getNumSamples());

    mainInputChannels.store (mainIn.getNumChannels(), std::memory_order_relaxed);

    // Sidechain is read only for a routing indicator in Step 1.
    auto* scBus = getBus (true, ve::Bus::sidechain);
    const bool scOn = scBus != nullptr && scBus->isEnabled();
    sidechainEnabled.store (scOn, std::memory_order_relaxed);

    float peak = 0.0f;
    if (scOn)
    {
        const auto sc = getBusBuffer (buffer, true, ve::Bus::sidechain);
        if (sc.getNumChannels() > 0)
            peak = sc.getMagnitude (0, sc.getNumSamples());
    }

    // Simple decay so the UI (polling at ~15 Hz) does not miss short signals.
    const float held = sidechainPeak.load (std::memory_order_relaxed) * 0.9f;
    sidechainPeak.store (juce::jmax (peak, held), std::memory_order_relaxed);
}

VocalEngineProcessor::Status VocalEngineProcessor::getStatus() const noexcept
{
    Status s;
    s.sidechainEnabled  = sidechainEnabled.load (std::memory_order_relaxed);
    s.sidechainPeak     = sidechainPeak.load (std::memory_order_relaxed);
    s.mainInputChannels = mainInputChannels.load (std::memory_order_relaxed);
    return s;
}

//==============================================================================
void VocalEngineProcessor::getStateInformation (juce::MemoryBlock&)
{
    // Session ValueTree serialisation arrives in Step 5.
}

void VocalEngineProcessor::setStateInformation (const void*, int)
{
}

juce::AudioProcessorEditor* VocalEngineProcessor::createEditor()
{
    return new VocalEngineEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VocalEngineProcessor();
}
