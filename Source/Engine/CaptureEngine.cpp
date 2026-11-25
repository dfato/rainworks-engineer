#include "CaptureEngine.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace ve
{
    //==========================================================================
    class CaptureEngine::Writer final : public juce::Thread
    {
    public:
        explicit Writer (CaptureEngine& o) : juce::Thread ("VE Capture Writer"), owner (o) {}

        ~Writer() override { stopThread (4000); }

        void run() override
        {
            while (! threadShouldExit())
            {
                drain();
                wait (10);
            }
            drain();
            if (open)
                finishRegion();
        }

    private:
        struct OpenRegion
        {
            CapturedRegion info;
            std::unique_ptr<juce::AudioFormatWriter> vocal, sidechain;
            std::uint64_t lastFlushFrame = 0;
        };

        void drain()
        {
            auto& events = *owner.eventFifo;
            auto& audio = *owner.audioFifo;

            for (;;)
            {
                if (events.getNumReady() > 0)
                {
                    int s1, z1, s2, z2;
                    events.prepareToRead (1, s1, z1, s2, z2);
                    const auto ev = owner.eventRing[(size_t) s1];

                    if (ev.streamFrame > consumedFrames)
                    {
                        // Write audio up to the event first.
                        const auto need = (int) juce::jmin<std::uint64_t> (ev.streamFrame - consumedFrames, 1u << 20);
                        if (writeAudio (juce::jmin (need, audio.getNumReady())) == 0)
                            return; // audio for this event not here yet
                        continue;
                    }

                    events.finishedRead (1);
                    handleEvent (ev);
                    continue;
                }

                if (writeAudio (audio.getNumReady()) == 0)
                    return;
            }
        }

        int writeAudio (int numFrames)
        {
            if (numFrames <= 0)
                return 0;

            auto& audio = *owner.audioFifo;
            int s1, z1, s2, z2;
            audio.prepareToRead (numFrames, s1, z1, s2, z2);
            writeChunk (s1, z1);
            writeChunk (s2, z2);
            audio.finishedRead (z1 + z2);
            consumedFrames += (std::uint64_t) (z1 + z2);
            return z1 + z2;
        }

        void writeChunk (int start, int num)
        {
            if (num <= 0 || ! open || ! region.info.ok)
                return;

            const float* vocal[2] = { owner.ring.getReadPointer (0, start), owner.ring.getReadPointer (1, start) };
            const float* sc[2]    = { owner.ring.getReadPointer (2, start), owner.ring.getReadPointer (3, start) };

            if (region.vocal != nullptr && ! region.vocal->writeFromFloatArrays (vocal, region.info.numChannels, num))
                fail ("Could not write vocal audio (disk full?)");
            if (region.sidechain != nullptr && ! region.sidechain->writeFromFloatArrays (sc, 2, num))
                fail ("Could not write sidechain audio (disk full?)");

            region.info.lengthSamples += num;

            if (consumedFrames - region.lastFlushFrame > (std::uint64_t) owner.sampleRate)
            {
                // Keeps the WAV headers valid if the host crashes mid-take.
                if (region.vocal) region.vocal->flush();
                if (region.sidechain) region.sidechain->flush();
                region.lastFlushFrame = consumedFrames;
            }
        }

        void writeSilence (std::int64_t frames)
        {
            if (! open || ! region.info.ok)
                return;

            std::vector<float> zeros ((size_t) juce::jmin<std::int64_t> (frames, 8192), 0.0f);
            const float* ch[2] = { zeros.data(), zeros.data() };
            region.info.droppedSamples += frames;

            while (frames > 0)
            {
                const auto n = (int) juce::jmin<std::int64_t> (frames, (std::int64_t) zeros.size());
                if (region.vocal) region.vocal->writeFromFloatArrays (ch, region.info.numChannels, n);
                if (region.sidechain) region.sidechain->writeFromFloatArrays (ch, 2, n);
                region.info.lengthSamples += n;
                frames -= n;
            }
        }

        void handleEvent (const Event& ev)
        {
            switch (ev.type)
            {
                case Event::Type::start: startRegion (ev); break;
                case Event::Type::tempo:
                    if (open)
                        region.info.tempoMap.addPoint ({ region.info.lengthSamples, ev.ppq, ev.bpm, ev.numerator, ev.denominator });
                    break;
                case Event::Type::gap:   writeSilence (ev.gapFrames); break;
                case Event::Type::end:   if (open) finishRegion(); break;
            }
        }

        static std::unique_ptr<juce::AudioFormatWriter> makeWriter (const juce::File& file, double sr, int channels)
        {
            file.deleteFile();
            std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
            if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
                return nullptr;

            juce::WavAudioFormat wav;
            const auto opts = juce::AudioFormatWriterOptions{}
                                  .withSampleRate (sr)
                                  .withNumChannels (channels)
                                  .withBitsPerSample (32)
                                  .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint);
            return wav.createWriterFor (stream, opts);
        }

        void startRegion (const Event& ev)
        {
            if (open)
                finishRegion();

            region = {};
            open = true;

            auto& info = region.info;
            info.id = juce::Uuid().toDashedString();
            const auto now = juce::Time::currentTimeMillis();
            lastOrder = juce::jmax<std::int64_t> (lastOrder + 1, (std::int64_t) now);
            info.order = lastOrder;
            info.startSample = ev.hostSample;
            info.startPpq = ev.ppq;
            info.sampleRate = owner.sampleRate;
            info.numChannels = juce::jlimit (1, 2, ev.mainChannels);
            info.hasSidechain = ev.hasSidechain;
            info.tempoMap = TempoMap (owner.sampleRate);
            info.tempoMap.addPoint ({ 0, ev.ppq, ev.bpm, ev.numerator, ev.denominator });

            const auto folder = owner.getDataFolder().getChildFile (regionsSubfolder());
            if (! folder.createDirectory())
            {
                fail ("Could not create " + folder.getFullPathName());
                return;
            }

            info.vocalFile = regionsSubfolder() + "/" + info.id + "_vocal.wav";
            region.vocal = makeWriter (owner.getDataFolder().getChildFile (info.vocalFile), info.sampleRate, info.numChannels);
            if (region.vocal == nullptr)
            {
                fail ("Could not open " + info.vocalFile);
                return;
            }

            if (info.hasSidechain)
            {
                info.sidechainFile = regionsSubfolder() + "/" + info.id + "_sidechain.wav";
                region.sidechain = makeWriter (owner.getDataFolder().getChildFile (info.sidechainFile), info.sampleRate, 2);
                if (region.sidechain == nullptr)
                    fail ("Could not open " + info.sidechainFile);
            }
        }

        void fail (const juce::String& msg)
        {
            if (! region.info.ok)
                return;
            region.info.ok = false;
            region.info.error = msg;
            region.vocal.reset();
            region.sidechain.reset();
        }

        void finishRegion()
        {
            region.vocal.reset();     // finalises WAV headers
            region.sidechain.reset();
            open = false;

            if (region.info.lengthSamples <= 0 && region.info.ok)
            {
                // Nothing captured: remove empty files.
                const auto folder = owner.getDataFolder();
                folder.getChildFile (region.info.vocalFile).deleteFile();
                if (region.info.sidechainFile.isNotEmpty())
                    folder.getChildFile (region.info.sidechainFile).deleteFile();
                return;
            }

            std::lock_guard<std::mutex> lock (owner.finishedMutex);
            owner.finished.push_back (std::move (region.info));
        }

        CaptureEngine& owner;
        OpenRegion region;
        bool open = false;
        std::uint64_t consumedFrames = 0;
        std::int64_t lastOrder = 0;
    };

    //==========================================================================
    CaptureEngine::CaptureEngine()
    {
        dataFolder = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("VocalEngine");
    }

    CaptureEngine::~CaptureEngine() { release(); }

    void CaptureEngine::prepare (double sr, int maxBlockSize)
    {
        release();

        sampleRate = sr;
        const int capacity = juce::jmax (maxBlockSize * 8, (int) (sr * 10.0)); // ~10 s of headroom
        ring.setSize (ringChannels, capacity, false, true, false);
        audioFifo = std::make_unique<juce::AbstractFifo> (capacity);
        eventRing.assign (4096, Event{});
        eventFifo = std::make_unique<juce::AbstractFifo> ((int) eventRing.size());

        capturing = false;
        endPending = false;
        streamFrames = 0;
        pendingGap = 0;
        capturingFlag = false;

        writer = std::make_unique<Writer> (*this);
        writer->startThread (juce::Thread::Priority::normal);
        prepared = true;
    }

    void CaptureEngine::release()
    {
        prepared = false;
        if (writer != nullptr)
        {
            writer->signalThreadShouldExit();
            writer->notify();
            writer.reset(); // joins after flushing
        }
        capturingFlag = false;
        capturing = false;
    }

    void CaptureEngine::setDataFolder (const juce::File& folder)
    {
        std::lock_guard<std::mutex> lock (folderMutex);
        dataFolder = folder;
    }

    juce::File CaptureEngine::getDataFolder() const
    {
        std::lock_guard<std::mutex> lock (folderMutex);
        return dataFolder;
    }

    bool CaptureEngine::pushEvent (const Event& ev) noexcept
    {
        if (eventFifo->getFreeSpace() < 1)
            return false;
        int s1, z1, s2, z2;
        eventFifo->prepareToWrite (1, s1, z1, s2, z2);
        eventRing[(size_t) s1] = ev;
        eventFifo->finishedWrite (1);
        return true;
    }

    bool CaptureEngine::pushAudio (const juce::AudioBuffer<float>& mainIn, const juce::AudioBuffer<float>* sc, int n) noexcept
    {
        if (audioFifo->getFreeSpace() < n)
            return false;

        int s1, z1, s2, z2;
        audioFifo->prepareToWrite (n, s1, z1, s2, z2);

        auto copy = [&] (int ringStart, int srcStart, int num)
        {
            if (num <= 0) return;
            const int mainCh = mainIn.getNumChannels();
            for (int ch = 0; ch < 2; ++ch)
            {
                if (mainCh > 0)
                    ring.copyFrom (ch, ringStart, mainIn, juce::jmin (ch, mainCh - 1), srcStart, num);
                else
                    ring.clear (ch, ringStart, num);
            }
            for (int ch = 0; ch < 2; ++ch)
            {
                if (sc != nullptr && sc->getNumChannels() > 0)
                    ring.copyFrom (2 + ch, ringStart, *sc, juce::jmin (ch, sc->getNumChannels() - 1), srcStart, num);
                else
                    ring.clear (2 + ch, ringStart, num);
            }
        };

        copy (s1, 0, z1);
        copy (s2, z1, z2);
        audioFifo->finishedWrite (z1 + z2);
        streamFrames += (std::uint64_t) (z1 + z2);
        return true;
    }

    void CaptureEngine::process (const HostSnapshot& host,
                                 const juce::AudioBuffer<float>& mainIn,
                                 const juce::AudioBuffer<float>* sidechain,
                                 bool armed,
                                 bool offline) noexcept
    {
        if (! prepared.load (std::memory_order_relaxed))
            return;

        const int n = mainIn.getNumSamples();
        if (offline)
            waitForSpace (n, 4);
        const bool want = armed && host.isPlaying && host.hasTimelinePosition() && n > 0;

        auto makeEvent = [&] (Event::Type t)
        {
            Event e;
            e.type = t;
            e.streamFrame = streamFrames;
            e.hostSample = host.timeInSamples;
            e.ppq = host.hasPpq ? host.ppqPosition : 0.0;
            e.bpm = host.bpm;
            e.numerator = host.timeSigNumerator;
            e.denominator = host.timeSigDenominator;
            e.mainChannels = mainIn.getNumChannels();
            e.hasSidechain = sidechain != nullptr;
            return e;
        };

        auto endCapture = [&]
        {
            if (pendingGap > 0)
            {
                auto g = makeEvent (Event::Type::gap);
                g.gapFrames = pendingGap;
                if (pushEvent (g)) pendingGap = 0;
            }
            endPending = ! pushEvent (makeEvent (Event::Type::end));
            capturing = false;
            capturingFlag.store (false, std::memory_order_relaxed);
        };

        if (endPending)
        {
            endPending = ! pushEvent (makeEvent (Event::Type::end));
            if (endPending)
                return;
        }

        // A jump in the timeline (seek, loop wrap) closes the current take.
        if (capturing && (! want || host.timeInSamples != expectedNextSample))
            endCapture();

        if (! want || endPending)
            return;

        if (! capturing)
        {
            if (! pushEvent (makeEvent (Event::Type::start)))
                return; // try again next block
            capturing = true;
            pendingGap = 0;
            lastBpm = host.bpm;
            lastNum = host.timeSigNumerator;
            lastDen = host.timeSigDenominator;
            liveStartSample.store (host.timeInSamples, std::memory_order_relaxed);
            liveStartPpq.store (host.hasPpq ? host.ppqPosition : 0.0, std::memory_order_relaxed);
            liveFrames.store (0, std::memory_order_relaxed);
            liveDropped.store (0, std::memory_order_relaxed);
            capturingFlag.store (true, std::memory_order_relaxed);
        }
        else if (std::abs (host.bpm - lastBpm) > 1.0e-9 || host.timeSigNumerator != lastNum || host.timeSigDenominator != lastDen)
        {
            if (pushEvent (makeEvent (Event::Type::tempo)))
            {
                lastBpm = host.bpm;
                lastNum = host.timeSigNumerator;
                lastDen = host.timeSigDenominator;
            }
        }

        if (pendingGap > 0)
        {
            auto g = makeEvent (Event::Type::gap);
            g.gapFrames = pendingGap;
            if (pushEvent (g))
                pendingGap = 0;
        }

        if (pendingGap > 0 || ! pushAudio (mainIn, sidechain, n))
        {
            pendingGap += n; // writer fills this with silence so timing stays exact
            liveDropped.fetch_add (n, std::memory_order_relaxed);
        }

        liveFrames.fetch_add (n, std::memory_order_relaxed);
        expectedNextSample = host.timeInSamples + n;
    }

    void CaptureEngine::waitForSpace (int frames, int events) noexcept
    {
        // Offline rendering only: the host is not waiting on a sound card, so
        // letting the writer catch up is better than dropping audio.
        for (int i = 0; i < 10000; ++i) // gives up after ~10 s
        {
            if (audioFifo->getFreeSpace() >= frames && eventFifo->getFreeSpace() >= events)
                return;
            if (writer != nullptr)
                writer->notify();
            juce::Thread::sleep (1);
        }
    }

    CaptureEngine::LiveStatus CaptureEngine::getLiveStatus() const noexcept
    {
        LiveStatus s;
        s.capturing = capturingFlag.load (std::memory_order_relaxed);
        s.startSample = liveStartSample.load (std::memory_order_relaxed);
        s.startPpq = liveStartPpq.load (std::memory_order_relaxed);
        s.frames = liveFrames.load (std::memory_order_relaxed);
        s.droppedSamples = liveDropped.load (std::memory_order_relaxed);
        return s;
    }

    std::vector<CapturedRegion> CaptureEngine::popFinishedRegions()
    {
        std::lock_guard<std::mutex> lock (finishedMutex);
        std::vector<CapturedRegion> out;
        out.swap (finished);
        return out;
    }
}
