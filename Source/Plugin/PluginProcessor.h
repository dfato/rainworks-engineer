#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "Engine/CaptureEngine.h"
#include "Engine/HostContext.h"
#include "Engine/JobQueue.h"
#include "Engine/PlaybackEngine.h"
#include "Engine/RenderCache.h"
#include "Engine/TripleBuffer.h"
#include "State/SessionModel.h"
#include <atomic>
#include <set>

class VocalEngineProcessor final : public juce::AudioProcessor,
                                   public juce::ChangeBroadcaster, // session/model changes, message thread
                                   private juce::Timer,
                                   private juce::ValueTree::Listener
{
public:
    VocalEngineProcessor();
    ~VocalEngineProcessor() override;

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
    // UI-facing API. Message thread only unless noted.

    struct Status
    {
        bool  sidechainEnabled  = false;
        float sidechainPeak     = 0.0f;
        int   mainInputChannels = 0;
    };
    Status getStatus() const noexcept; // any thread

    // Single reader (the editor). Returns true when a newer snapshot arrived.
    bool readHostSnapshot (ve::HostSnapshot& out) noexcept { return hostSnapshots.read (out); }

    void setArmed (bool shouldArm) noexcept { armed.store (shouldArm); }
    bool isArmed() const noexcept           { return armed.load(); }
    ve::CaptureEngine::LiveStatus getCaptureStatus() const noexcept { return capture.getLiveStatus(); }

    void setMonitoringMode (ve::MonitoringMode);
    ve::MonitoringMode getMonitoringMode() const noexcept { return (ve::MonitoringMode) monitoringMode.load(); }

    void setKeyOverride (int keyIndexOrMinusOne);

    void setDataFolder (const juce::File&);
    // Imports a whole song from files (vocal + optional beat) as one take starting at bar 1.
    void importTake (const juce::File& vocal, const juce::File& beat);
    void analyseAll();
    void analyseRegion (const juce::String& regionId);
    void deleteRegion (const juce::String& regionId);

    const ve::SessionModel& getSession() const noexcept { return session; }
    const ve::RenderCache& getRenderCache() const noexcept { return cache; }
    bool isRegionLoading (const juce::String& id) const { return loadingRegions.count (id) > 0; }
    std::vector<ve::JobQueue::Status> getJobStatus() const { return jobs->getStatus(); }
    juce::String getLastMessage() const { return lastMessage; }
    double getEngineSampleRate() const noexcept { return engineSampleRate.load(); }

private:
    static BusesProperties makeBusesProperties();

    void timerCallback() override;
    void valueTreePropertyChanged (juce::ValueTree&, const juce::Identifier&) override { stateDirty = true; }
    void valueTreeChildAdded (juce::ValueTree&, juce::ValueTree&) override             { stateDirty = true; }
    void valueTreeChildRemoved (juce::ValueTree&, juce::ValueTree&, int) override      { stateDirty = true; }

    void applyState (const juce::XmlElement&);
    void refreshCachedState();
    void reloadAllRegions();
    void scheduleRender (const ve::RegionDescriptor&);
    void scheduleAnalysis (const ve::RegionDescriptor&);
    void schedulePitchTracking (const ve::RegionDescriptor&);
    void resnapNotes();
    void rebuildPlayback();
    void postMessage (const juce::String&);

    // ---- audio-thread shared state ------------------------------------------
    std::atomic<float> sidechainPeak { 0.0f };
    std::atomic<bool>  sidechainEnabled { false };
    std::atomic<int>   mainInputChannels { 0 };
    std::atomic<bool>  armed { false };
    std::atomic<int>   monitoringMode { (int) ve::MonitoringMode::input };
    std::atomic<double> engineSampleRate { 0.0 };
    std::atomic<bool>  sampleRateChanged { false };
    std::atomic<double> lastHostBpm { 120.0 };
    std::atomic<int>   lastHostNum { 4 }, lastHostDen { 4 };
    std::uint64_t blockCounter = 0;

    ve::TripleBuffer<ve::HostSnapshot> hostSnapshots;
    ve::CaptureEngine capture;
    ve::PlaybackEngine playback;

    // ---- message-thread state -----------------------------------------------
    ve::SessionModel session;
    ve::RenderCache cache;
    std::shared_ptr<ve::RegionRenderer> renderer;
    std::set<juce::String> loadingRegions;
    juce::String lastMessage;
    bool stateDirty = true;

    juce::CriticalSection cachedStateLock; // never taken on the audio thread
    juce::MemoryBlock cachedState;

    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>> (true);

    // Declared last so it is destroyed first: workers stop before anything they touch.
    std::unique_ptr<ve::JobQueue> jobs;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VocalEngineProcessor)
};
