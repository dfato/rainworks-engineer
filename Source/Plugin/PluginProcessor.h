#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <atomic>

class VocalEngineProcessor final : public juce::AudioProcessor
{
public:
    VocalEngineProcessor();
    ~VocalEngineProcessor() override = default;

    //==========================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using juce::AudioProcessor::processBlock;

    //==========================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override  { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==========================================================================
    // Lock-free status read by the UI on the message thread.
    struct Status
    {
        bool  sidechainEnabled  = false; // host has the sidechain bus active
        float sidechainPeak     = 0.0f;  // decaying peak of the last blocks
        int   mainInputChannels = 0;
    };
    Status getStatus() const noexcept;

private:
    static BusesProperties makeBusesProperties();

    std::atomic<float> sidechainPeak { 0.0f };
    std::atomic<bool>  sidechainEnabled { false };
    std::atomic<int>   mainInputChannels { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalEngineProcessor)
};
