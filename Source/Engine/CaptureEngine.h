#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>
#include "HostContext.h"
#include "TempoMap.h"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace ve
{
    // A finished capture, handed from the writer thread to the message thread.
    struct CapturedRegion
    {
        juce::String id;
        std::int64_t order = 0;            // later takes win where regions overlap (punch-in)
        std::int64_t startSample = 0;      // host timeline sample of the first captured sample
        double startPpq = 0.0;             // host PPQ of the first captured sample
        std::int64_t lengthSamples = 0;
        double sampleRate = 0.0;
        int numChannels = 0;               // vocal channels (1 or 2)
        bool hasSidechain = false;
        juce::String vocalFile;            // relative to the data folder
        juce::String sidechainFile;        // relative to the data folder, empty if none
        TempoMap tempoMap;                 // region-local, sample 0 = region start
        std::int64_t droppedSamples = 0;   // samples replaced by silence after FIFO overflow
        bool ok = true;
        juce::String error;
    };

    // Real-time capture of the main input and sidechain.
    //
    // Audio thread: process() pushes audio and timeline events into two
    // lock-free FIFOs (juce::AbstractFifo). It never allocates, locks or
    // touches the disk.
    // Writer thread: drains the FIFOs and streams 32-bit float WAV files.
    class CaptureEngine
    {
    public:
        static constexpr int ringChannels = 4; // 0-1 vocal, 2-3 sidechain

        CaptureEngine();
        ~CaptureEngine();

        // Message thread, while audio is not running. Allocates the FIFOs.
        void prepare (double sampleRate, int maxBlockSize);
        // Message thread. Stops the writer after flushing what it has.
        void release();

        // Message thread. Where new regions are written.
        void setDataFolder (const juce::File& folder);
        juce::File getDataFolder() const;

        // Audio thread. When `offline` is true (host is rendering faster than
        // real time, e.g. FL Studio export) a full FIFO waits for the writer
        // instead of dropping audio. Never pass true for real-time playback.
        void process (const HostSnapshot& host,
                      const juce::AudioBuffer<float>& mainIn,
                      const juce::AudioBuffer<float>* sidechain,
                      bool armed,
                      bool offline = false) noexcept;

        bool isCapturing() const noexcept { return capturingFlag.load (std::memory_order_relaxed); }

        struct LiveStatus
        {
            bool capturing = false;
            std::int64_t startSample = 0;
            double startPpq = 0.0;
            std::int64_t frames = 0;
            std::int64_t droppedSamples = 0;
        };
        LiveStatus getLiveStatus() const noexcept;

        // Message thread. Returns regions finished since the last call.
        std::vector<CapturedRegion> popFinishedRegions();

        static juce::String regionsSubfolder() { return "Regions"; }

        struct Event
        {
            enum class Type : std::uint8_t { start, tempo, gap, end };
            Type type = Type::start;
            std::uint64_t streamFrame = 0;  // frames pushed before this event
            std::int64_t hostSample = 0;
            double ppq = 0.0;
            double bpm = 120.0;
            int numerator = 4, denominator = 4;
            int mainChannels = 0;
            bool hasSidechain = false;
            std::int64_t gapFrames = 0;
        };

    private:
        class Writer;
        friend class Writer;

        bool pushEvent (const Event&) noexcept;
        void waitForSpace (int audioFrames, int events) noexcept; // offline only
        bool pushAudio (const juce::AudioBuffer<float>& mainIn, const juce::AudioBuffer<float>* sc, int numSamples) noexcept;

        // FIFOs (allocated in prepare).
        juce::AudioBuffer<float> ring;
        std::unique_ptr<juce::AbstractFifo> audioFifo;
        std::vector<Event> eventRing;
        std::unique_ptr<juce::AbstractFifo> eventFifo;

        double sampleRate = 0.0;
        std::atomic<bool> prepared { false };

        // Audio-thread state.
        bool capturing = false;
        bool endPending = false;
        std::int64_t expectedNextSample = 0;
        double lastBpm = 0.0;
        int lastNum = 0, lastDen = 0;
        std::uint64_t streamFrames = 0;
        std::int64_t pendingGap = 0;

        std::atomic<bool> capturingFlag { false };
        std::atomic<std::int64_t> liveStartSample { 0 }, liveFrames { 0 }, liveDropped { 0 };
        std::atomic<double> liveStartPpq { 0.0 };

        std::unique_ptr<Writer> writer;

        mutable std::mutex folderMutex;
        juce::File dataFolder;

        std::mutex finishedMutex;
        std::vector<CapturedRegion> finished;

        JUCE_DECLARE_NON_COPYABLE (CaptureEngine)
    };
}
