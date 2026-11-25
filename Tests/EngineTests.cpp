// Headless tests for Source/Engine and Source/State. Built without any GUI
// module, which also proves the engine has no UI dependency.

#include <juce_audio_formats/juce_audio_formats.h>
#include "Engine/CaptureEngine.h"
#include "Engine/HostContext.h"
#include "Engine/JobQueue.h"
#include "Engine/ML/InferenceBackend.h"
#include "Engine/PlaybackEngine.h"
#include "Engine/RenderCache.h"
#include "Engine/SidechainAnalysis.h"
#include "Engine/TakeImporter.h"
#include "Engine/NotePlan.h"
#include "Engine/PitchTracker.h"
#include "Engine/TempoMap.h"
#include "Engine/TripleBuffer.h"
#include "State/SessionModel.h"

using namespace ve;

namespace
{
    struct FakePlayHead final : juce::AudioPlayHead
    {
        PositionInfo info;
        juce::Optional<PositionInfo> getPosition() const override { return info; }
    };

    HostSnapshot playingAt (std::int64_t sample, double sr, double bpm = 120.0)
    {
        HostSnapshot s;
        s.sampleRate = sr;
        s.hasPlayHead = true;
        s.hasBpm = true; s.bpm = bpm;
        s.hasTimeSig = true;
        s.hasTimeSamples = true; s.timeInSamples = sample;
        s.hasPpq = true; s.ppqPosition = (double) sample / (sr * 60.0 / bpm);
        s.isPlaying = true;
        return s;
    }

    juce::File tempFolder (const juce::String& name)
    {
        auto f = juce::File::getSpecialLocation (juce::File::tempDirectory).getChildFile ("ve-tests").getChildFile (name);
        f.deleteRecursively();
        f.createDirectory();
        return f;
    }

    // Value written at host sample t: distinct per sample so alignment errors show up.
    float marker (std::int64_t t, int ch) { return (float) ((t % 1000) + 1) / 2000.0f * (ch == 0 ? 1.0f : -1.0f); }
}

//==============================================================================
class CoreTests final : public juce::UnitTest
{
public:
    CoreTests() : juce::UnitTest ("Core", "VocalEngine") {}

    void runTest() override
    {
        beginTest ("TripleBuffer delivers latest value");
        {
            TripleBuffer<int> tb;
            int v = -1;
            expect (! tb.read (v));
            tb.write (1); tb.write (2); tb.write (3);
            expect (tb.read (v)); expectEquals (v, 3);
            expect (! tb.read (v)); expectEquals (v, 3);
        }

        beginTest ("TempoMap converts both ways across a tempo change");
        {
            TempoMap m (48000.0);
            m.addPoint ({ 0, 8.0, 120.0, 4, 4 });        // 24000 samples per quarter
            m.addPoint ({ 96000, 12.0, 60.0, 3, 4 });    // 48000 samples per quarter
            expectWithinAbsoluteError (m.sampleToPpq (48000.0), 10.0, 1e-9);
            expectWithinAbsoluteError (m.sampleToPpq (144000.0), 13.0, 1e-9);
            expectWithinAbsoluteError (m.ppqToSample (13.0), 144000.0, 1e-6);
            expectWithinAbsoluteError (m.ppqToSample (9.0), 24000.0, 1e-6);
            expectEquals (m.pointAtSample (100000.0).numerator, 3);
        }

        beginTest ("HostContext handles missing fields");
        {
            FakePlayHead ph;
            auto none = HostContext::read (nullptr, 48000.0, 512, 1);
            expect (! none.hasPlayHead && ! none.hasTimelinePosition());

            ph.info.setIsPlaying (true);
            ph.info.setBpm (120.0);
            ph.info.setTimeInSamples (48000);
            auto s = HostContext::read (&ph, 48000.0, 512, 2);
            expect (s.hasPpq && (s.derivedFlags & HostSnapshot::derivedPpq));
            expectWithinAbsoluteError (s.ppqPosition, 2.0, 1e-9);
            expect (! s.hasTimeSig && s.timeSigNumerator == 4);

            FakePlayHead ph2;
            ph2.info.setBpm (90.0);
            ph2.info.setPpqPosition (3.0);
            auto s2 = HostContext::read (&ph2, 44100.0, 512, 3);
            expect (s2.hasTimeSamples && (s2.derivedFlags & HostSnapshot::derivedTimeSamples));
            expectEquals ((juce::int64) s2.timeInSamples, (juce::int64) std::llround (3.0 * 44100.0 * 60.0 / 90.0));

            FakePlayHead ph3; // no tempo at all: nothing can be derived
            ph3.info.setPpqPosition (1.0);
            auto s3 = HostContext::read (&ph3, 44100.0, 512, 4);
            expect (s3.hasPpq && ! s3.hasTimeSamples && ! s3.hasBpm);
        }

        beginTest ("JobQueue runs by priority and cancels groups");
        {
            JobQueue q (1, std::make_unique<ImmediateDispatcher>());
            std::mutex m;
            std::vector<int> order;
            std::atomic<bool> release { false };

            // Block the single worker so the rest queue up.
            q.submit ("blocker", {}, JobPriority::urgent, [&] (JobContext&) { while (! release) juce::Thread::sleep (1); return juce::Result::ok(); });
            juce::Thread::sleep (20);

            auto add = [&] (int tag, JobPriority p, juce::String group = {})
            {
                return q.submit ("job", group, p, [&, tag] (JobContext&) { std::lock_guard<std::mutex> l (m); order.push_back (tag); return juce::Result::ok(); });
            };
            add (1, JobPriority::low);
            add (2, JobPriority::high);
            add (3, JobPriority::normal, "g");
            add (4, JobPriority::normal, "g"); // cancels 3
            int cancelled = 0;
            q.submit ("x", {}, JobPriority::low, [] (JobContext&) { return juce::Result::ok(); },
                      [&] (const JobResult& r) { cancelled += r.success ? 0 : 100; });
            release = true;
            expect (q.waitUntilIdle (5000));
            expect (order == std::vector<int> ({ 2, 4, 1 }), "order was wrong");
            expectEquals (cancelled, 0);
        }

        beginTest ("Smoke test model is well formed");
        {
            const auto mb = ml::makeSmokeTestModel();
            expect (mb.getSize() > 40);
            expect (ml::createOnnxBackend() == nullptr || ml::isOnnxAvailable());
        }
    }
};

//==============================================================================
class CaptureTests final : public juce::UnitTest
{
public:
    CaptureTests() : juce::UnitTest ("Capture", "VocalEngine") {}

    void runTest() override
    {
        beginTest ("Capture writes aligned regions, splits on seek, records tempo map");

        const double sr = 48000.0;
        const int block = 480;
        const auto folder = tempFolder ("capture");

        CaptureEngine cap;
        cap.setDataFolder (folder);
        cap.prepare (sr, block);

        juce::AudioBuffer<float> main (1, block), sc (2, block);
        auto runBlocks = [&] (std::int64_t startSample, int numBlocks, double bpm, bool armed)
        {
            for (int b = 0; b < numBlocks; ++b)
            {
                const auto t0 = startSample + (std::int64_t) b * block;
                for (int i = 0; i < block; ++i)
                {
                    main.setSample (0, i, marker (t0 + i, 0));
                    sc.setSample (0, i, marker (t0 + i, 0) * 0.5f);
                    sc.setSample (1, i, marker (t0 + i, 1) * 0.5f);
                }
                cap.process (playingAt (t0, sr, bpm), main, &sc, armed);
            }
        };

        runBlocks (48000, 100, 120.0, true);   // take A: 1 s .. 2 s
        runBlocks (96000, 50, 100.0, true);    // continues, tempo change at 2 s
        runBlocks (240000, 40, 100.0, true);   // seek -> take B at 5 s
        {
            auto stopped = playingAt (259200, sr);
            stopped.isPlaying = false;
            cap.process (stopped, main, &sc, true); // stop ends take B
        }
        runBlocks (300000, 10, 120.0, false);  // not armed: nothing

        // Let the writer drain, then stop it.
        juce::Thread::sleep (200);
        cap.release();

        auto regions = cap.popFinishedRegions();
        expectEquals ((int) regions.size(), 2);
        if (regions.size() != 2) return;

        auto& a = regions[0];
        auto& b = regions[1];
        expect (a.ok && b.ok, a.error + b.error);
        expectEquals ((juce::int64) a.startSample, (juce::int64) 48000);
        expectEquals ((juce::int64) a.lengthSamples, (juce::int64) 150 * block);
        expectWithinAbsoluteError (a.startPpq, 2.0, 1e-9);
        expectEquals ((int) a.tempoMap.getPoints().size(), 2);
        expectEquals ((juce::int64) a.tempoMap.getPoints()[1].sample, (juce::int64) 48000);
        expectWithinAbsoluteError (a.tempoMap.getPoints()[1].bpm, 100.0, 1e-9);
        expectEquals ((juce::int64) b.startSample, (juce::int64) 240000);
        expectEquals ((juce::int64) b.lengthSamples, (juce::int64) 40 * block);
        expect (b.order > a.order);
        expectEquals (a.droppedSamples, (std::int64_t) 0);

        // Read back and compare with what the host "played".
        juce::AudioBuffer<float> audio;
        double rate = 0;
        expect (loadAudioFile (folder.getChildFile (a.vocalFile), 0.0, nullptr, audio, rate).wasOk());
        expectEquals (audio.getNumChannels(), 1);
        expectEquals (audio.getNumSamples(), 150 * block);
        bool same = true;
        for (int i = 0; i < audio.getNumSamples(); i += 97)
            same = same && juce::exactlyEqual (audio.getSample (0, i), marker (48000 + i, 0));
        expect (same, "vocal samples differ from input");

        expect (loadAudioFile (folder.getChildFile (b.sidechainFile), 0.0, nullptr, audio, rate).wasOk());
        expectEquals (audio.getNumChannels(), 2);
        expectEquals (audio.getSample (1, 10), marker (240010, 1) * 0.5f);

        // Format check: 32-bit float WAV.
        juce::WavAudioFormat wav;
        std::unique_ptr<juce::AudioFormatReader> reader (wav.createReaderFor (folder.getChildFile (a.vocalFile).createInputStream().release(), true));
        expect (reader != nullptr && reader->bitsPerSample == 32 && reader->usesFloatingPointData);
    }
};

//==============================================================================
class PlaybackTests final : public juce::UnitTest
{
public:
    PlaybackTests() : juce::UnitTest ("Playback", "VocalEngine") {}

    static std::shared_ptr<RenderedAudio> constantAudio (const juce::String& id, int len, float value)
    {
        auto r = std::make_shared<RenderedAudio>();
        r->regionId = id;
        r->sampleRate = 48000.0;
        r->audio.setSize (1, len);
        for (int i = 0; i < len; ++i) r->audio.setSample (0, i, value + (float) i * 1.0e-6f);
        return r;
    }

    void runTest() override
    {
        const double sr = 48000.0;

        PlaybackSnapshot snap;
        RegionDescriptor a; a.id = "a"; a.order = 1; a.startSample = 1000; a.captureSampleRate = sr;
        RegionDescriptor b; b.id = "b"; b.order = 2; b.startSample = 1500; b.captureSampleRate = sr;  // punch-in over a
        snap.add (a, constantAudio ("a", 1000, 0.25f), sr);
        snap.add (b, constantAudio ("b", 200, 0.75f), sr);
        snap.finalise();

        beginTest ("Rendered playback is sample accurate with punch-in precedence");
        {
            juce::AudioBuffer<float> out (2, 1000);
            out.clear();
            for (int i = 0; i < 1000; ++i) out.setSample (0, i, -1.0f); // marks "input"
            expect (PlaybackEngine::renderSnapshot (snap, out, playingAt (900, sr)));

            expectEquals (out.getSample (0, 99), -1.0f);                         // 999: before region a, input kept
            expectWithinAbsoluteError (out.getSample (0, 100), 0.25f, 1e-7f);    // 1000: a[0]
            expectWithinAbsoluteError (out.getSample (0, 599), 0.25f + 499e-6f, 1e-7f); // 1499: a[499]
            expectWithinAbsoluteError (out.getSample (0, 600), 0.75f, 1e-7f);    // 1500: b[0] wins
            expectWithinAbsoluteError (out.getSample (0, 800), 0.25f + 700e-6f, 1e-7f); // 1700: back to a[700]
            expectWithinAbsoluteError (out.getSample (1, 100), 0.25f, 1e-7f);    // mono -> stereo
        }

        beginTest ("Loop wrap inside a block jumps to the loop start");
        {
            auto h = playingAt (1900, sr);  // loop end at 1950 -> wraps to 1000
            h.isLooping = true;
            h.hasLoopPoints = true;
            const double spq = sr * 60.0 / 120.0;
            h.loopStartPpq = 1000.0 / spq;
            h.loopEndPpq = 1950.0 / spq;
            juce::AudioBuffer<float> out (1, 100);
            out.clear();
            PlaybackEngine::renderSnapshot (snap, out, h);
            expectWithinAbsoluteError (out.getSample (0, 49), 0.25f + 949e-6f, 1e-7f); // 1949
            expectWithinAbsoluteError (out.getSample (0, 50), 0.25f, 1e-7f);           // wrapped to 1000
        }

        beginTest ("Stopped transport leaves input untouched");
        {
            auto h = playingAt (1200, sr);
            h.isPlaying = false;
            juce::AudioBuffer<float> out (1, 64);
            out.clear();
            expect (! PlaybackEngine::renderSnapshot (snap, out, h));
            expectEquals (out.getMagnitude (0, 64), 0.0f);
        }

        beginTest ("PlaybackEngine hands snapshots over without freeing on the audio side");
        {
            PlaybackEngine pe;
            for (int i = 0; i < 200; ++i)
            {
                auto s = std::make_unique<PlaybackSnapshot>();
                s->add (a, constantAudio ("a", 1000, 0.5f), sr);
                pe.publish (std::move (s));
                juce::AudioBuffer<float> out (1, 128);
                out.clear();
                pe.render (out, playingAt (1000, sr));
                expectWithinAbsoluteError (out.getSample (0, 0), 0.5f, 1e-7f);
            }
            pe.collectGarbage();
        }

        beginTest ("RenderCache keys by region + plan hash");
        {
            RenderCache cache;
            auto r1 = constantAudio ("x", 10, 0.1f); r1->planHash = 1;
            auto r2 = constantAudio ("x", 10, 0.2f); r2->planHash = 2;
            cache.store (r1); cache.store (r2);
            expect (cache.find ("x", 1) == r1 && cache.find ("x", 2) == r2);
            cache.removeStale ("x", 2);
            expect (cache.find ("x", 1) == nullptr && cache.find ("x", 2) == r2);
            expect (RenderPlan::identity().hash() == RenderPlan::identity().hash());
        }
    }
};

//==============================================================================
class AnalysisTests final : public juce::UnitTest
{
public:
    AnalysisTests() : juce::UnitTest ("Analysis", "VocalEngine") {}

    static void addNote (juce::AudioBuffer<float>& b, int start, int len, int midi, double sr)
    {
        const double f0 = 440.0 * std::pow (2.0, (midi - 69) / 12.0);
        for (int h = 1; h <= 4; ++h)
        {
            const double amp = 0.08 / h;
            for (int i = 0; i < len; ++i)
            {
                const float env = (float) juce::jmin (1.0, juce::jmin (i, len - i) / 200.0);
                const float v = (float) (amp * std::sin (2.0 * juce::MathConstants<double>::pi * f0 * h * i / sr)) * env;
                b.addSample (0, start + i, v);
                b.addSample (1, start + i, v);
            }
        }
    }

    void runTest() override
    {
        beginTest ("Key and chords from a synthetic I-vi-IV-V7 progression in C");

        const double sr = 44100.0, bpm = 120.0;
        const int beat = (int) (sr * 60.0 / bpm);
        // Bars of 4 beats: C, Am, F, G7, C
        const std::vector<std::vector<int>> chords {
            { 48, 60, 64, 67 }, { 45, 57, 60, 64 }, { 41, 57, 60, 65 }, { 43, 55, 59, 62, 65 }, { 48, 60, 64, 67 } };
        juce::AudioBuffer<float> audio (2, beat * 4 * (int) chords.size());
        audio.clear();
        for (size_t bar = 0; bar < chords.size(); ++bar)
            for (int bt = 0; bt < 4; ++bt)
                for (int n : chords[bar])
                    addNote (audio, (int) ((bar * 4 + (size_t) bt) * (size_t) beat), beat, n, sr);

        TempoMap map (sr);
        map.addPoint ({ 0, 8.0, bpm, 4, 4 }); // region starts at bar 3

        SidechainAnalysisResult res;
        expect (Analysis::analyseSidechain (audio, sr, map, nullptr, res).wasOk());
        expectEquals ((int) res.beatChroma.size(), 20);
        expect (res.key.tonic == 0 && ! res.key.minor, "key was " + Analysis::keyName (res.key.tonic, res.key.minor));

        juce::StringArray names;
        for (auto& c : res.chords) names.add (Analysis::chordName (c.root, c.quality));
        logMessage ("Chords: " + names.joinIntoString (" | "));
        expect (names.joinIntoString (" ") == "C Am F G7 C", "chords were " + names.joinIntoString (" "));
        if (res.chords.size() == 5)
        {
            expectWithinAbsoluteError (res.chords[1].startPpq, 12.0, 1e-6);
            expectWithinAbsoluteError (res.chords[4].endPpq, 28.0, 1e-6);
        }

        beginTest ("Minor key profile");
        {
            Chroma aMinor {};
            for (int pc : { 9, 11, 0, 2, 4, 5, 7 }) aMinor[(size_t) pc] = 1.0f;
            aMinor[9] = 3.0f; aMinor[4] = 2.0f; aMinor[0] = 2.0f;
            const auto k = Analysis::estimateKey (aMinor);
            expect (k.tonic == 9 && k.minor, "key was " + Analysis::keyName (k.tonic, k.minor));
        }

        beginTest ("Silence gives no chord");
        {
            juce::AudioBuffer<float> silent (2, beat * 8);
            silent.clear();
            SidechainAnalysisResult r;
            expect (Analysis::analyseSidechain (silent, sr, map, nullptr, r).wasOk());
            expect (r.chords.size() == 1 && r.chords[0].root < 0);
            expect (! r.key.isValid());
        }
    }
};

//==============================================================================
class SessionTests final : public juce::UnitTest
{
public:
    SessionTests() : juce::UnitTest ("Session", "VocalEngine") {}

    void runTest() override
    {
        beginTest ("Session round-trips through XML and flags missing files");

        const auto folder = tempFolder ("session");
        SessionModel s;
        s.setDataFolder (folder);
        s.setMonitoringMode (MonitoringMode::rendered);
        s.setKeyOverride (14); // D minor

        CapturedRegion r;
        r.id = "take-1"; r.order = 1700000000123; r.startSample = 9876543210; r.startPpq = 12.5;
        r.lengthSamples = 4800; r.sampleRate = 48000.0; r.numChannels = 2; r.hasSidechain = true;
        r.vocalFile = "Regions/take-1_vocal.wav"; r.sidechainFile = "Regions/take-1_sidechain.wav";
        r.tempoMap = TempoMap (48000.0);
        r.tempoMap.addPoint ({ 0, 12.5, 128.0, 4, 4 });
        r.tempoMap.addPoint ({ 2400, 13.5, 100.0, 6, 8 });
        s.addRegion (r);

        folder.getChildFile ("Regions").createDirectory();
        folder.getChildFile (r.vocalFile).replaceWithText ("x");

        SidechainAnalysisResult an;
        an.key = { 7, false, 0.8f, 0.1f };
        an.chromaSum[7] = 1.0f; an.chromaSum[11] = 0.5f; an.chromaSum[2] = 0.7f;
        an.chords.push_back ({ 12.5, 14.0, 7, ChordQuality::major, 0.9f });
        an.chords.push_back ({ 14.0, 16.0, 2, ChordQuality::dominant7, 0.7f });
        s.setAnalysis (r.id, an);

        const auto xml = s.toXml();
        SessionModel restored;
        expect (restored.restoreFromXml (*xml).wasOk());
        expectEquals (restored.getSessionId(), s.getSessionId());
        expect (restored.getMonitoringMode() == MonitoringMode::rendered);
        expectEquals (restored.getKeyOverride(), 14);
        expectEquals (restored.getDataFolder().getFullPathName(), folder.getFullPathName());

        auto d = restored.getRegionDescriptors();
        expectEquals ((int) d.size(), 1);
        expectEquals ((juce::int64) d[0].startSample, (juce::int64) 9876543210);
        expectEquals ((juce::int64) d[0].order, (juce::int64) 1700000000123);
        expectEquals ((int) d[0].tempoMap.getPoints().size(), 2);
        expectEquals (d[0].tempoMap.getPoints()[1].denominator, 8);
        expect (! d[0].missingFiles);
        expect (d[0].planHash == RenderPlan::identity().hash());

        auto chords = restored.getChords (r.id);
        expectEquals ((int) chords.size(), 2);
        expect (chords[1].quality == ChordQuality::dominant7 && chords[1].root == 2);

        bool ov = false;
        auto k = restored.getEffectiveKey (&ov);
        expect (ov && k.tonic == 2 && k.minor);

        folder.getChildFile (r.vocalFile).deleteFile();
        expect (restored.getRegionDescriptors()[0].missingFiles);

        beginTest ("Foreign or newer state is rejected without losing the session");
        {
            juce::XmlElement other ("SomethingElse");
            expect (restored.restoreFromXml (other).failed());
            juce::XmlElement newer ("Session");
            newer.setAttribute ("version", 99);
            expect (restored.restoreFromXml (newer).failed());
            expectEquals (restored.getNumRegions(), 1);
        }
    }
};

//==============================================================================
class ImportTests final : public juce::UnitTest
{
public:
    ImportTests() : juce::UnitTest ("Import", "VocalEngine") {}

    void runTest() override
    {
        beginTest ("Whole song import resamples, pads the beat and starts at bar 1");
        {
            const auto folder = tempFolder ("import");
            juce::AudioBuffer<float> vocal (1, 44100 * 3), beat (2, 44100 * 2);
            for (int i = 0; i < vocal.getNumSamples(); ++i) vocal.setSample (0, i, 0.1f * std::sin (i * 0.05f));
            for (int i = 0; i < beat.getNumSamples(); ++i) { beat.setSample (0, i, 0.2f); beat.setSample (1, i, -0.2f); }
            const auto vf = folder.getChildFile ("My Hook Vocal.wav"), bf = folder.getChildFile ("my hook beat.wav");
            expect (TakeImporter::writeWav (vf, vocal, 44100.0).wasOk());
            expect (TakeImporter::writeWav (bf, beat, 44100.0).wasOk());

            juce::File v, b;
            TakeImporter::classifyFiles ({ bf, vf }, v, b);
            expect (v == vf && b == bf, "file classification");

            CapturedRegion r;
            TempoPoint tempo; tempo.bpm = 140.0; tempo.numerator = 3; tempo.denominator = 4;
            expect (TakeImporter::importTake (vf, bf, folder, 48000.0, tempo, nullptr, r).wasOk());
            expectEquals ((juce::int64) r.startSample, (juce::int64) 0);
            expectEquals ((juce::int64) r.lengthSamples, (juce::int64) 144000);
            expect (r.hasSidechain && r.numChannels == 1);
            expectWithinAbsoluteError (r.tempoMap.getPoints()[0].bpm, 140.0, 1e-9);

            juce::AudioBuffer<float> sc;
            double rate = 0;
            expect (loadAudioFile (folder.getChildFile (r.sidechainFile), 0.0, nullptr, sc, rate).wasOk());
            expectEquals (sc.getNumSamples(), 144000);
            expectWithinAbsoluteError (sc.getSample (1, 1000), -0.2f, 1e-3f);
            expectEquals (sc.getSample (0, 143000), 0.0f); // padded after the beat ends
            expectEquals (rate, 48000.0);
        }

        beginTest ("Offline (export) capture never drops audio");
        {
            const auto folder = tempFolder ("offline");
            CaptureEngine cap;
            cap.setDataFolder (folder);
            cap.prepare (48000.0, 4096);
            juce::AudioBuffer<float> main (2, 4096), sc (2, 4096);
            main.clear(); sc.clear();
            const int blocks = 48000 * 30 / 4096; // 30 s, three times the FIFO size, as fast as possible
            for (int b = 0; b < blocks; ++b)
                cap.process (playingAt ((std::int64_t) b * 4096, 48000.0), main, &sc, true, true);
            auto stop = playingAt ((std::int64_t) blocks * 4096, 48000.0);
            stop.isPlaying = false;
            cap.process (stop, main, &sc, true, true);
            juce::Thread::sleep (100);
            cap.release();
            auto regions = cap.popFinishedRegions();
            expectEquals ((int) regions.size(), 1);
            if (! regions.empty())
            {
                expectEquals (regions[0].droppedSamples, (std::int64_t) 0);
                expectEquals ((juce::int64) regions[0].lengthSamples, (juce::int64) blocks * 4096);
            }
        }
    }
};

//==============================================================================
class NoteTests final : public juce::UnitTest
{
public:
    NoteTests() : juce::UnitTest ("Notes", "VocalEngine") {}

    // Sung-ish tone: harmonics, slight vibrato, optional detune in cents.
    static void addVoiceNote (juce::AudioBuffer<float>& b, double sr, double startSec, double lenSec, float midi, float cents)
    {
        const double f0 = 440.0 * std::pow (2.0, (midi + cents / 100.0f - 69.0f) / 12.0);
        const int s0 = (int) (startSec * sr), n = (int) (lenSec * sr);
        double phase = 0.0;
        for (int i = 0; i < n; ++i)
        {
            const double t = i / sr;
            const double f = f0 * (1.0 + 0.004 * std::sin (2.0 * juce::MathConstants<double>::pi * 5.5 * t));
            phase += 2.0 * juce::MathConstants<double>::pi * f / sr;
            const float env = (float) juce::jmin (1.0, juce::jmin (i, n - i) / (0.01 * sr));
            float v = 0.0f;
            for (int h = 1; h <= 6; ++h) v += (float) (std::sin (phase * h) / (h * h));
            b.addSample (0, s0 + i, 0.3f * env * v);
        }
    }

    void runTest() override
    {
        const double sr = 48000.0;

        beginTest ("Pitch tracker finds f0 of a harmonic tone and marks silence unvoiced");
        {
            juce::AudioBuffer<float> b (1, (int) (sr * 1.5));
            b.clear();
            addVoiceNote (b, sr, 0.25, 1.0, 57.0f, 0.0f); // A3 = 220 Hz
            PitchTrack t;
            expect (PitchTracker::track (b, sr, {}, nullptr, t).wasOk());
            const int mid = (int) (0.75 * sr / t.hopSize);
            expect (t.isVoiced (mid));
            expectWithinAbsoluteError (t.frames[(size_t) mid].f0, 220.0f, 2.5f);
            expect (! t.isVoiced (2), "silence should be unvoiced");

            PitchTrack back;
            expect (PitchTrack::deserialise (t.serialise(), back));
            expectEquals ((int) back.frames.size(), (int) t.frames.size());
            expectEquals (back.frames[(size_t) mid].f0, t.frames[(size_t) mid].f0);
        }

        beginTest ("Segmentation splits a melody into notes and snaps targets to the key");
        {
            // C4 (flat 30 cents), E4, G4 (sharp 25 cents), then a gap, then F#4 -> should snap to G or F in C major.
            juce::AudioBuffer<float> b (1, (int) (sr * 3.0));
            b.clear();
            addVoiceNote (b, sr, 0.10, 0.45, 60.0f, -30.0f);
            addVoiceNote (b, sr, 0.55, 0.45, 64.0f, 0.0f);
            addVoiceNote (b, sr, 1.00, 0.45, 67.0f, 25.0f);
            addVoiceNote (b, sr, 1.80, 0.50, 66.0f, 0.0f);
            PitchTrack t;
            expect (PitchTracker::track (b, sr, {}, nullptr, t).wasOk());

            KeyEstimate cMajor; cMajor.tonic = 0; cMajor.minor = false;
            auto notes = NoteSegmenter::segment (t, cMajor);
            juce::StringArray desc;
            for (auto& n : notes) desc.add (juce::String (n.detectedMidi, 2) + "->" + juce::String (n.targetMidi, 0));
            logMessage ("Notes: " + desc.joinIntoString (", "));
            expectEquals ((int) notes.size(), 4);
            if (notes.size() == 4)
            {
                expectWithinAbsoluteError (notes[0].detectedMidi, 59.7f, 0.1f);
                expectEquals (notes[0].targetMidi, 60.0f);
                expectWithinAbsoluteError (notes[2].detectedMidi, 67.25f, 0.1f);
                expectEquals (notes[2].targetMidi, 67.0f);
                expect (juce::exactlyEqual (notes[3].targetMidi, 65.0f) || juce::exactlyEqual (notes[3].targetMidi, 67.0f), "F# should snap into C major");
                expectWithinAbsoluteError ((double) notes[1].start / sr, 0.55, 0.03);
                expectWithinAbsoluteError ((double) notes[3].end / sr, 2.30, 0.03);
            }

            NotePlan plan { notes };
            expect (plan.isIdentity(), "no correction yet");
            const auto h0 = plan.hash();
            plan.notes[0].correction = 1.0f;
            expect (! plan.isIdentity() && plan.hash() != h0);
        }

        beginTest ("Snap to key");
        {
            KeyEstimate aMinor; aMinor.tonic = 9; aMinor.minor = true;
            expectEquals (NoteSegmenter::snapToKey (60.8f, aMinor), 60.0f); // C# (flat) -> C in A minor
            expectEquals (NoteSegmenter::snapToKey (61.2f, aMinor), 62.0f); // C# (sharp) -> D
            expectEquals (NoteSegmenter::snapToKey (61.2f, KeyEstimate{}), 61.0f);
        }
    }
};

static NoteTests noteTests;
static ImportTests importTests;
static CoreTests coreTests;
static CaptureTests captureTests;
static PlaybackTests playbackTests;
static AnalysisTests analysisTests;
static SessionTests sessionTests;

int main()
{
    juce::UnitTestRunner runner;
    runner.setAssertOnFailure (false);
    runner.runTestsInCategory ("VocalEngine");

    int failures = 0;
    for (int i = 0; i < runner.getNumResults(); ++i)
        failures += runner.getResult (i)->failures;
    std::cout << (failures == 0 ? "ALL TESTS PASSED" : "TESTS FAILED: " + std::to_string (failures)) << std::endl;
    return failures == 0 ? 0 : 1;
}
