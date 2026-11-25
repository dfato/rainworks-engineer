#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "JobQueue.h"
#include "TempoMap.h"
#include "NotePlan.h"
#include <map>
#include <memory>
#include <mutex>

namespace ve
{
    // What to do to a region's audio. Stage 0/1 has only the identity plan
    // (play the captured audio). Stage 2 adds the note plan; its hash changes
    // whenever the plan does, which is what triggers selective re-rendering.
    struct RenderPlan
    {
        static constexpr const char* capturedId = "captured";
        static constexpr const char* pitchCorrectId = "pitch-correct";

        juce::String rendererId = capturedId;
        int rendererVersion = 1;
        NotePlan notes;   // Stage 2. Stage 3 adds style profile + expression params.

        std::uint64_t hash() const noexcept;
        static RenderPlan identity() { return {}; }
        // Picks the renderer for a note plan: captured audio if nothing would change.
        static RenderPlan forNotes (NotePlan);
    };

    // Everything a renderer needs to know about a region. Plain data.
    struct RegionDescriptor
    {
        juce::String id;
        std::int64_t order = 0;
        std::int64_t startSample = 0;   // at captureSampleRate
        double startPpq = 0.0;
        std::int64_t lengthSamples = 0; // at captureSampleRate
        double captureSampleRate = 0.0;
        int numChannels = 1;
        juce::File vocalFile;
        juce::File sidechainFile;       // may be empty
        TempoMap tempoMap;
        juce::File pitchFile;           // Stage 2 pitch track, may be empty
        RenderPlan plan;
        std::uint64_t planHash = 0;     // == plan.hash()
        bool missingFiles = false;
    };

    // Min/max overview for drawing. Mono (max over channels).
    struct WaveformPeaks
    {
        int samplesPerPeak = 256;
        double sampleRate = 0.0;
        std::vector<float> mins, maxs;

        static std::shared_ptr<const WaveformPeaks> compute (const juce::AudioBuffer<float>&, double sampleRate, int samplesPerPeak = 256);
    };

    struct RenderedAudio
    {
        juce::String regionId;
        std::uint64_t planHash = 0;
        double sampleRate = 0.0;                 // equals the engine rate it was rendered for
        juce::AudioBuffer<float> audio;
        std::shared_ptr<const WaveformPeaks> peaks;
    };

    // Turns a region + plan into audio. Runs on a JobQueue worker.
    class RegionRenderer
    {
    public:
        virtual ~RegionRenderer() = default;
        virtual juce::Result render (const RegionDescriptor&, const RenderPlan&, double targetSampleRate,
                                     JobContext&, RenderedAudio& out) = 0;
    };

    // Stage 0/1 renderer: the "render" is the captured take, resampled if
    // the session runs at a different rate than it was captured at.
    class CapturedAudioRenderer final : public RegionRenderer
    {
    public:
        juce::Result render (const RegionDescriptor&, const RenderPlan&, double targetSampleRate,
                             JobContext&, RenderedAudio& out) override;
    };

    // Loads a WAV fully into memory, resampling to targetSampleRate (0 = keep).
    juce::Result loadAudioFile (const juce::File&, double targetSampleRate, JobContext*, juce::AudioBuffer<float>& out, double& outRate);

    // Rendered audio keyed by region id + plan hash. Message thread and workers only.
    class RenderCache
    {
    public:
        std::shared_ptr<const RenderedAudio> find (const juce::String& regionId, std::uint64_t planHash) const;
        // Any cached render of the region (used to keep playing the old render while a new one is built).
        std::shared_ptr<const RenderedAudio> findAny (const juce::String& regionId) const;
        void store (std::shared_ptr<const RenderedAudio>);
        // Drops every entry for the region whose hash differs from keepHash.
        void removeStale (const juce::String& regionId, std::uint64_t keepHash);
        void removeRegion (const juce::String& regionId);
        void clear();
        std::size_t size() const;
        std::size_t memoryBytes() const;

    private:
        using Key = std::pair<juce::String, std::uint64_t>;
        mutable std::mutex mutex;
        std::map<Key, std::shared_ptr<const RenderedAudio>> entries;
    };
}
