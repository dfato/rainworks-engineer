#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include "HostContext.h"
#include "RenderCache.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>

namespace ve
{
    // One region as the audio thread sees it: positions already converted to
    // the current engine sample rate, audio owned by the snapshot.
    struct PlaybackRegion
    {
        std::int64_t start = 0;      // host timeline sample
        std::int64_t length = 0;
        std::int64_t order = 0;
        const juce::AudioBuffer<float>* audio = nullptr;
    };

    // Immutable once published. Built on the message thread.
    struct PlaybackSnapshot
    {
        double sampleRate = 0.0;
        std::vector<PlaybackRegion> regions;                     // sorted by order, oldest first
        std::vector<std::shared_ptr<const RenderedAudio>> keepAlive;

        void add (const RegionDescriptor& d, std::shared_ptr<const RenderedAudio> audio, double engineRate);
        void finalise();
    };

    // Plays cached region audio aligned to the host timeline.
    //
    // Snapshots are handed to the audio thread through a single atomic slot;
    // the audio thread hands the previous one back through a lock-free FIFO so
    // nothing is ever freed on the audio thread.
    class PlaybackEngine
    {
    public:
        PlaybackEngine();
        ~PlaybackEngine();

        // Message thread.
        void publish (std::unique_ptr<PlaybackSnapshot>);
        void collectGarbage();

        // Audio thread. Replaces `out` where regions cover the host timeline and
        // leaves it untouched elsewhere. Returns true if any region was played.
        bool render (juce::AudioBuffer<float>& out, const HostSnapshot& host) noexcept;

        // Pure function used by render(); exposed for tests.
        static bool renderSnapshot (const PlaybackSnapshot&, juce::AudioBuffer<float>& out, const HostSnapshot& host) noexcept;

    private:
        void acquireLatest() noexcept;

        std::atomic<PlaybackSnapshot*> pending { nullptr };
        PlaybackSnapshot* active = nullptr; // audio thread owned

        static constexpr int retireCapacity = 64;
        std::array<PlaybackSnapshot*, retireCapacity> retired {};
        juce::AbstractFifo retireFifo { retireCapacity };

        JUCE_DECLARE_NON_COPYABLE (PlaybackEngine)
    };
}
