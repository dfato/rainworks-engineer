#pragma once

#include "../RenderCache.h"
#include "VoiceConverter.h"
#include <functional>
#include <atomic>
#include <mutex>

namespace ve
{
    // Stage 5 renderer: applies the plan's pitch and timing (as the pitch-correction
    // renderer does), then re-sings the result in the chosen voice model with the
    // planned pitch. Models are loaded on first use and kept until the voice changes.
    class VoiceRenderer final : public RegionRenderer
    {
    public:
        using BackendFactory = std::function<std::unique_ptr<ml::InferenceBackend>()>;
        using VoiceLookup = std::function<bool (const juce::String& id, VoiceModelInfo&)>;

        VoiceRenderer();                                      // ONNX Runtime + the shared voice library
        VoiceRenderer (BackendFactory, VoiceLookup);          // for tests

        juce::Result render (const RegionDescriptor&, const RenderPlan&, double targetSampleRate,
                             JobContext&, RenderedAudio& out) override;

        VoiceConverter::Settings settings;

        // GPU (DirectML) when the build supports it; takes effect on the next render.
        void setUseGpu (bool);
        bool getUseGpu() const noexcept { return useGpu.load(); }
        // Where the voice model ran last ("GPU (DirectML)", "CPU", ...).
        juce::String getLastDevice() const;

    private:
        juce::Result ensureLoaded (const VoiceModelInfo&);

        std::atomic<bool> useGpu { true };
        BackendFactory makeBackend;
        VoiceLookup lookup;

        std::mutex lock;  // one conversion at a time; models are large
        juce::String encoderPath, loadedVoice, loadedVersion;
        std::unique_ptr<ml::InferenceBackend> encoder, synth;
        FeatureBank bank;
        bool loadedOnGpu = true;
        mutable std::mutex deviceLock;
        juce::String lastDevice;
    };
}
