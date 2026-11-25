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
#include "Engine/PitchCorrectRenderer.h"
#include "Engine/TimeWarp.h"
#include "Engine/Intent/NoteIntent.h"
#include "Engine/Voice/VoiceConverter.h"
#include "Engine/Voice/VoiceRenderer.h"
#include "Engine/Style/ExpressionLayer.h"
#include "Engine/Style/StyleProfile.h"
#include "Engine/Style/StyleLearner.h"
#include "Engine/Style/ToneShaper.h"
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

            // Style scales: in C major, D (62) is kept by the key's scale, moved off it by
            // pentatonic only if not in the set (it is), F (65) is not in major pentatonic.
            {
                KeyEstimate k = cMajor;
                k.scale = ScaleMode::pentatonic;
                expectEquals (NoteSegmenter::snapToKey (65.1f, k), 64.0f);     // F -> E
                expectEquals (NoteSegmenter::snapToKey (62.0f, k), 62.0f);     // D stays
                k.scale = ScaleMode::blues;
                expectEquals (NoteSegmenter::snapToKey (63.1f, k), 63.0f);     // blue third kept
                KeyEstimate aMinor; aMinor.tonic = 9; aMinor.minor = true; aMinor.scale = ScaleMode::harmonic;
                expectEquals (NoteSegmenter::snapToKey (67.8f, aMinor), 68.0f); // G -> G# (raised 7th)
                aMinor.scale = ScaleMode::chromatic;
                expectEquals (NoteSegmenter::snapToKey (66.2f, aMinor), 66.0f);
                expect (scaleModeFromString ("Blues") == ScaleMode::blues && toString (ScaleMode::modal) == "modal");
            }

            NotePlan plan { notes };
            expect (plan.isIdentity(), "no correction yet");
            const auto h0 = plan.hash();
            plan.notes[0].correction = 1.0f;
            expect (! plan.isIdentity() && plan.hash() != h0);
        }

        beginTest ("PSOLA pitch correction moves a note to its target and leaves the rest bit-exact");
        {
            juce::AudioBuffer<float> b (1, (int) (sr * 2.0));
            b.clear();
            addVoiceNote (b, sr, 0.10, 0.60, 57.0f, -40.0f); // A3, 40 cents flat
            addVoiceNote (b, sr, 1.00, 0.60, 60.0f, 0.0f);   // C4 untouched
            PitchTrack t;
            expect (PitchTracker::track (b, sr, {}, nullptr, t).wasOk());
            KeyEstimate cMajor; cMajor.tonic = 0;
            auto notes = NoteSegmenter::segment (t, cMajor);
            expectEquals ((int) notes.size(), 2);
            if (notes.size() != 2) return;
            notes[0].correction = 1.0f; // pull A3 up to 57

            const int n = b.getNumSamples();
            std::vector<float> f0 ((size_t) n);
            for (int i = 0; i < n; ++i) f0[(size_t) i] = t.frames[(size_t) juce::jmin ((int) t.frames.size() - 1, i / t.hopSize)].f0;
            NotePlan plan { notes };
            const auto shift = PitchCorrectRenderer::buildShiftCurve (plan, n, sr);
            juce::AudioBuffer<float> out;
            PitchCorrectRenderer::psolaShift (b, out, sr, f0, shift);
            expectEquals (out.getNumSamples(), n);

            PitchTrack t2;
            expect (PitchTracker::track (out, sr, {}, nullptr, t2).wasOk());
            const int mid = (int) (0.40 * sr / t2.hopSize);
            const float cents = 1200.0f * std::log2 (t2.frames[(size_t) mid].f0 / 220.0f);
            logMessage ("Corrected note is " + juce::String (cents, 1) + " cents from A3 (was -40)");
            expectWithinAbsoluteError (cents, 0.0f, 8.0f);

            bool exact = true;
            for (int i = (int) (1.2 * sr); i < (int) (1.5 * sr); ++i)
                exact = exact && juce::exactlyEqual (out.getSample (0, i), b.getSample (0, i));
            expect (exact, "uncorrected note must be untouched");

            // Level stays sensible (no OLA gain blow-up).
            const float inRms = b.getRMSLevel (0, (int) (0.2 * sr), (int) (0.4 * sr));
            const float outRms = out.getRMSLevel (0, (int) (0.2 * sr), (int) (0.4 * sr));
            expectWithinAbsoluteError (outRms / inRms, 1.0f, 0.25f);
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

//==============================================================================
class StyleLearnerTests final : public juce::UnitTest
{
public:
    StyleLearnerTests() : juce::UnitTest ("Style learning", "VocalEngine") {}

    // A synthetic "singer": long notes with a scoop and vibrato on the beat, short flat
    // notes on swung off-16ths, the whole take 10 cents sharp. Built as a pitch track so
    // every measured value is known exactly.
    static void buildTake (PitchTrack& t, std::vector<Note>& notes, double bpm, double swing)
    {
        const double sr = 48000.0;
        t.sampleRate = sr;
        t.hopSize = 240;
        const double beat = 60.0 / bpm, step = beat / 4.0;
        const double total = 18.0 * beat + 1.0;
        t.frames.assign ((size_t) (total * sr / t.hopSize), {});
        auto frameTime = [&] (size_t i) { return (double) i * t.hopSize / sr; };

        auto addNote = [&] (double start, double len, float midi, bool expressive)
        {
            for (size_t i = 0; i < t.frames.size(); ++i)
            {
                const double x = frameTime (i) - start;
                if (x < 0.0 || x >= len) continue;
                double cents = midi * 100.0 + 10.0;
                if (expressive)
                {
                    if (x < 0.06) cents -= 80.0 * (1.0 - x / 0.06);                              // scoop
                    if (x > 0.15) cents += 40.0 * std::sin (juce::MathConstants<double>::twoPi * 5.5 * (x - 0.15)); // vibrato
                }
                else
                    cents += 3.0 * std::sin (juce::MathConstants<double>::twoPi * 1.3 * x);    // small drift
                t.frames[i].f0 = PitchTrack::midiToHz ((float) (cents / 100.0));
                t.frames[i].confidence = 0.9f;
                t.frames[i].rms = 0.1f;
            }
            Note n;
            n.start = (std::int64_t) (start * sr);
            n.end = (std::int64_t) ((start + len) * sr);
            n.detectedMidi = NoteEdit::medianMidi (t, n.start, n.end, midi);
            n.targetMidi = midi;
            notes.push_back (n);
        };

        const float melody[] = { 60, 64, 67, 65, 64, 62, 60, 62 };
        for (int m = 0; m < 16; ++m)
        {
            const double bar = 0.3 + m * beat;           // one long + one short note per beat
            addNote (bar, 2.4 * step, melody[m % 8], true);
            addNote (bar + 2.0 * step + step * (1.0 + swing / 3.0), 0.6 * step, melody[(m + 3) % 8], false);
        }
    }

    void runTest() override
    {
        beginTest ("Style learner measures scoops, vibrato, tuning and swing from a pitch track");
        {
            PitchTrack t; std::vector<Note> notes;
            buildTake (t, notes, 60.0, 0.5);   // slow tempo so the long notes can show vibrato
            const auto s = StyleLearner::analyse (t, notes, 60.0);
            logMessage (juce::JSON::toString (s.toVar(), true));

            expectWithinAbsoluteError (s.tuningOffsetCents, 10.0f, 2.0f);
            expectLessThan (s.inTuneCents, 3.0f);
            expectEquals (s.longNotes, 16);
            expectGreaterOrEqual (s.vibratoNotes, 14);
            expectWithinAbsoluteError (s.vibratoRateHz, 5.5f, 0.3f);
            expectWithinAbsoluteError (s.vibratoDepthCents, 40.0f, 8.0f);
            // Long notes that follow a lower note within 80 ms are approached by a slide, not a scoop.
            expectGreaterOrEqual (s.scoopedNotes, 8);
            expectEquals (s.scoopedNotes, s.scoopCandidates);
            expectWithinAbsoluteError (s.scoopCents, 70.0f, 15.0f);
            expectWithinAbsoluteError (s.scoopMs, 52.0f, 12.0f);
            expectWithinAbsoluteError (s.vibratoOnsetMs, 150.0f, 50.0f);
            expect (s.timingValid, "onsets sit on the grid");
            expectWithinAbsoluteError (s.swing, 0.5f, 0.11f);
            expectLessThan (s.onsetSpreadMs, 8.0f);
        }

        beginTest ("Straight timing reads as no swing");
        {
            PitchTrack t; std::vector<Note> notes;
            buildTake (t, notes, 60.0, 0.0);
            const auto s = StyleLearner::analyse (t, notes, 60.0);
            expect (s.timingValid);
            expectLessThan (s.swing, 0.11f);
        }

        beginTest ("Learning moves a style toward the measurements, by evidence and strength");
        {
            PitchTrack t; std::vector<Note> notes;
            buildTake (t, notes, 60.0, 0.5);
            const auto s = StyleLearner::analyse (t, notes, 60.0);
            std::vector<StyleLearner::TakeStats> takes { s, s, s, s };
            const auto combined = StyleLearner::combine (takes);
            expectEquals (combined.longNotes, 4 * s.longNotes);

            StyleLibrary lib;
            const auto base = lib.get ("natural");
            const auto same = StyleLearner::learn (base, combined, 4, 0.0f);
            expectEquals (same.scoopCents, base.scoopCents);
            expectEquals (same.swing, base.swing);

            const auto learned = StyleLearner::learn (base, combined, 4, 1.0f);
            expectGreaterThan (learned.scoopCents, base.scoopCents + 20.0f);
            expectGreaterThan (learned.vibratoDepthCents, base.vibratoDepthCents + 5.0f);
            expectGreaterThan (learned.swing, 0.1f);
            expectGreaterThan (learned.correction, base.correction);   // it sings dead in tune

            auto named = learned;
            named.id = "my-style"; named.name = "My Style";
            const auto json = StyleLearner::toJson (named, "natural", combined, 4);
            expect (lib.addJson (json).isEmpty(), json);
            const auto back = lib.get ("my-style");
            expectEquals (back.name, juce::String ("My Style"));
            expectWithinAbsoluteError (back.scoopCents, learned.scoopCents, 0.01f);
            expectWithinAbsoluteError (back.swing, learned.swing, 0.01f);

            const auto restored = StyleLearner::TakeStats::fromVar (juce::JSON::parse (juce::JSON::toString (s.toVar())));
            expectEquals (restored.vibratoNotes, s.vibratoNotes);
            expectWithinAbsoluteError (restored.swing, s.swing, 0.01f);
        }

        beginTest ("Style learner finds vibrato in real audio");
        {
            const double sr = 48000.0;
            juce::AudioBuffer<float> b (1, (int) (sr * 2.0));
            b.clear();
            double phase = 0.0;
            for (int i = (int) (0.2 * sr); i < (int) (1.8 * sr); ++i)
            {
                const double x = i / sr - 0.2;
                const double cents = 40.0 * std::sin (juce::MathConstants<double>::twoPi * 5.0 * x);
                phase += juce::MathConstants<double>::twoPi * 220.0 * std::pow (2.0, cents / 1200.0) / sr;
                float v = 0.0f;
                for (int h = 1; h <= 6; ++h) v += (float) (std::sin (phase * h) / (h * h));
                b.setSample (0, i, 0.3f * v * (float) juce::jmin (1.0, juce::jmin (x, 1.6 - x) / 0.01));
            }
            const auto s = StyleLearner::analyseAudio (b, sr, 0.0);
            logMessage (juce::JSON::toString (s.toVar(), true));
            expectEquals (s.vibratoNotes, 1);
            expectWithinAbsoluteError (s.vibratoRateHz, 5.0f, 0.3f);
            expectWithinAbsoluteError (s.vibratoDepthCents, 40.0f, 10.0f);
        }
    }
};

static NoteTests noteTests;
static StyleLearnerTests styleLearnerTests;

//==============================================================================
class StyleTests final : public juce::UnitTest
{
public:
    StyleTests() : juce::UnitTest ("Styles", "VocalEngine") {}

    void runTest() override
    {
        beginTest ("Built-in library loads 30+ genres with inheritance resolved");
        {
            StyleLibrary lib;
            int genres = 0;
            for (auto& e : lib.list()) genres += e.category != "Basic" ? 1 : 0;
            expectGreaterOrEqual (genres, 30);

            const auto trap = lib.get ("trap");
            expectEquals (trap.name, juce::String ("Trap"));
            expectEquals (trap.category, juce::String ("Hip-hop"));
            expectWithinAbsoluteError (trap.retuneMs, 4.0f, 1e-6f);
            expectWithinAbsoluteError (trap.slideMs, 20.0f, 1e-6f);       // own value
            expectWithinAbsoluteError (trap.vibratoRateHz, 5.5f, 1e-6f);  // from natural, via hip-hop

            const auto emo = lib.get ("emo-rap"); // emo-rap -> melodic-rap -> trap -> hip-hop -> natural
            expectWithinAbsoluteError (emo.scoopCents, 50.0f, 1e-6f);
            expectWithinAbsoluteError (emo.vibratoDepthCents, 15.0f, 1e-6f);
            expectWithinAbsoluteError (emo.correction, 1.0f, 1e-6f);

            expectEquals (lib.get ("does-not-exist").id, juce::String ("natural"));
            expect (lib.get ("trap").hash() != lib.get ("pop").hash());
        }

        beginTest ("User JSON styles override and inherit; bad JSON is reported");
        {
            StyleLibrary lib;
            expect (lib.addJson (R"({"id":"my-trap","name":"My Trap","inherits":"trap","vibrato":{"addDepthCents":40}})").isEmpty());
            const auto mine = lib.get ("my-trap");
            expectWithinAbsoluteError (mine.vibratoDepthCents, 40.0f, 1e-6f);
            expectWithinAbsoluteError (mine.retuneMs, 4.0f, 1e-6f);
            expect (lib.addJson ("{ not json").isNotEmpty());
            expect (lib.addJson (R"({"name":"no id"})").isNotEmpty());
            expect (lib.addJson (R"({"id":"loop-a","inherits":"loop-b"})").isEmpty());
            expect (lib.addJson (R"({"id":"loop-b","inherits":"loop-a"})").isEmpty());
            lib.get ("loop-a"); // must not hang
        }

        beginTest ("Expression layer: hard tune flattens vibrato, natural keeps it, slides glide");
        {
            const double sr = 48000.0;
            const int n = (int) sr;
            std::vector<float> f0 ((size_t) n);
            for (int i = 0; i < n; ++i) // 57.3 MIDI with +-30 cent vibrato
                f0[(size_t) i] = PitchTrack::midiToHz (57.3f + 0.3f * std::sin (juce::MathConstants<float>::twoPi * 5.0f * (float) i / (float) sr));

            Note note; note.start = 0; note.end = n; note.detectedMidi = 57.3f; note.targetMidi = 57.0f; note.correction = 1.0f;
            NotePlan plan { { note } };
            StyleLibrary lib;

            auto spread = [&] (const StyleProfile& st)
            {
                const auto sh = ExpressionLayer::buildShiftCurve (plan, st, f0, sr);
                float lo = 1e9f, hi = -1e9f;
                for (int i = n / 4; i < n * 3 / 4; ++i)
                {
                    const float out = PitchTrack::hzToMidi (f0[(size_t) i]) + sh[(size_t) i];
                    lo = juce::jmin (lo, out); hi = juce::jmax (hi, out);
                }
                return std::make_pair (lo, hi);
            };

            const auto hard = spread (lib.get ("hard-tune"));
            expectWithinAbsoluteError (hard.first, 57.0f, 0.01f);
            expectWithinAbsoluteError (hard.second, 57.0f, 0.01f);

            const auto nat = spread (lib.get ("natural"));
            expectWithinAbsoluteError (nat.second - nat.first, 0.6f, 0.05f);           // vibrato kept
            expectWithinAbsoluteError ((nat.second + nat.first) * 0.5f, 57.0f, 0.05f); // centred on target

            // A tracker octave error inside a note must not become a jump in a flat style's curve.
            {
                auto glitchy = f0;
                for (int i = n / 2; i < n / 2 + 400; ++i) glitchy[(size_t) i] *= 2.0f;
                const auto sh = ExpressionLayer::buildShiftCurve (plan, lib.get ("trap"), glitchy, sr);
                float maxStep = 0.0f, maxAbs = 0.0f;
                for (int i = 1; i < n; ++i)
                {
                    maxStep = juce::jmax (maxStep, std::abs (sh[(size_t) i] - sh[(size_t) i - 1]));
                    maxAbs = juce::jmax (maxAbs, std::abs (sh[(size_t) i]));
                }
                expectLessThan (maxAbs, 1.0f);
                expectLessThan (maxStep, 0.02f);
            }

            // A loose style keeps its full added vibrato even at half correction.
            {
                auto loose = lib.get ("natural");
                loose.vibratoDepthCents = 40.0f; loose.vibratoRateHz = 5.0f; loose.vibratoOnsetMs = 0.0f;
                NotePlan half { { note } };
                half.notes[0].correction = 0.5f;
                const auto sh = ExpressionLayer::buildShiftCurve (half, loose, f0, sr);
                const auto flat = ExpressionLayer::buildShiftCurve (half, lib.get ("natural"), f0, sr);
                float depth = 0.0f;
                for (int i = n / 4; i < n * 3 / 4; ++i) depth = juce::jmax (depth, std::abs (sh[(size_t) i] - flat[(size_t) i]));
                expectWithinAbsoluteError (depth, 0.40f, 0.03f);
            }

            // Two corrected notes with a short gap: the slide moves the pitch before the second note starts.
            Note a = note; a.end = n / 2 - 960;  a.detectedMidi = 57.3f; a.targetMidi = 57.0f;
            Note b = note; b.start = n / 2;      b.detectedMidi = 57.3f; b.targetMidi = 60.0f;
            NotePlan two { { a, b } };
            auto st = lib.get ("hard-tune");
            st.slideMs = 80.0f;
            const auto sh = ExpressionLayer::buildShiftCurve (two, st, f0, sr);
            const int justBefore = (int) a.end - 10;
            const float outBefore = PitchTrack::hzToMidi (f0[(size_t) justBefore]) + sh[(size_t) justBefore];
            expect (outBefore > 57.2f && outBefore < 60.0f, "slide should be under way at the end of note A: " + juce::String (outBefore));

            // Style changes the render plan hash once notes are corrected.
            const auto p1 = RenderPlan::forNotes (plan, lib.get ("trap"));
            const auto p2 = RenderPlan::forNotes (plan, lib.get ("pop"));
            expect (p1.hash() != p2.hash());
            NotePlan none { { note } }; none.notes[0].correction = 0.0f;
            expect (RenderPlan::forNotes (none, lib.get ("trap")).hash() == RenderPlan::identity().hash());
        }

        beginTest ("Ornaments: runs walk the song's scale, phrase ends fall, style amount scales them");
        {
            const double sr = 48000.0;
            // C major phrase: seven short notes, then a long held E that ends the phrase.
            std::vector<Note> notes;
            const int degrees[] = { 60, 62, 64, 65, 67, 69, 71 };
            std::int64_t t = 0;
            for (int m : degrees)
            {
                Note x; x.start = t; x.end = t + (std::int64_t) (0.3 * sr); x.detectedMidi = x.targetMidi = (float) m; x.correction = 1.0f;
                notes.push_back (x); t = x.end + (std::int64_t) (0.02 * sr);
            }
            Note last; last.start = t; last.end = t + (std::int64_t) (1.2 * sr); last.detectedMidi = last.targetMidi = 64.0f; last.correction = 1.0f;
            notes.push_back (last);
            const int n = (int) (last.end + (std::int64_t) (0.2 * sr));
            std::vector<float> f0 ((size_t) n, 0.0f);
            for (auto& x : notes)
                for (auto i = x.start; i < x.end; ++i) f0[(size_t) i] = PitchTrack::midiToHz (x.detectedMidi);
            NotePlan plan { notes };

            StyleProfile st; st.retuneMs = 0.0f; st.keepVariation = 0.0f;
            st.runs = 1.0f; st.runNotes = 4; st.runNoteMs = 90.0f;
            const auto sh = ExpressionLayer::buildShiftCurve (plan, st, f0, sr);
            float hi = -1e9f;
            for (auto i = last.start; i < last.end; ++i) hi = juce::jmax (hi, sh[(size_t) i]);
            // E -> F (a half step in C major, not a whole tone) ... -> lands on C, a scale note.
            expectWithinAbsoluteError (hi, 1.0f, 0.05f);
            expectWithinAbsoluteError (sh[(size_t) (last.end - (std::int64_t) (0.04 * sr))], -4.0f, 0.05f);
            // The held start of the note is untouched.
            expectWithinAbsoluteError (sh[(size_t) (last.start + (std::int64_t) (0.3 * sr))], 0.0f, 0.01f);

            StyleProfile fall; fall.retuneMs = 0.0f; fall.keepVariation = 0.0f; fall.fallCents = 200.0f; fall.fallMs = 200.0f;
            const auto fs = ExpressionLayer::buildShiftCurve (plan, fall, f0, sr);
            expectLessThan (fs[(size_t) (last.end - (std::int64_t) (0.02 * sr))], -1.5f);
            expectWithinAbsoluteError (fs[(size_t) (notes[2].end - (std::int64_t) (0.02 * sr))], 0.0f, 0.01f); // mid-phrase: no fall

            // Amount 0 keeps only the tuning; amount 2 doubles the expression.
            const auto none = ExpressionLayer::buildShiftCurve (plan, st.withAmount (0.0f), f0, sr);
            float most = 0.0f;
            for (auto v : none) most = juce::jmax (most, std::abs (v));
            expectLessThan (most, 0.01f);
            expectWithinAbsoluteError (fall.withAmount (2.0f).fallCents, 400.0f, 0.01f);
            expect (st.hash() != st.withAmount (0.5f).hash());
        }

        beginTest ("Tone shaping changes the sound, keeps the loudness, and is off by default");
        {
            const double sr = 48000.0;
            const int n = (int) sr;
            juce::AudioBuffer<float> voice (1, n);
            juce::Random rng (7);
            for (int i = 0; i < n; ++i)
            {
                const float env = 0.5f + 0.5f * std::sin (juce::MathConstants<float>::twoPi * 2.0f * (float) i / (float) sr);
                voice.setSample (0, i, env * (0.4f * std::sin (juce::MathConstants<float>::twoPi * 220.0f * (float) i / (float) sr)
                                              + 0.05f * (rng.nextFloat() * 2.0f - 1.0f)));
            }
            auto highEnergy = [] (const juce::AudioBuffer<float>& b)
            {
                double e = 0.0;
                for (int i = 1; i < b.getNumSamples(); ++i) { const double d = b.getSample (0, i) - b.getSample (0, i - 1); e += d * d; }
                return e;
            };
            const double rmsIn = voice.getRMSLevel (0, 0, n);

            auto plain = voice;
            ToneShaper::process (plain, sr, StyleProfile {});
            float maxDiff = 0.0f;
            for (int i = 0; i < n; ++i) maxDiff = juce::jmax (maxDiff, std::abs (plain.getSample (0, i) - voice.getSample (0, i)));
            expectEquals (maxDiff, 0.0f);

            StyleProfile bright; bright.brightness = 1.0f;
            StyleProfile dark; dark.brightness = -1.0f;
            auto b = voice, d = voice;
            ToneShaper::process (b, sr, bright);
            ToneShaper::process (d, sr, dark);
            expectGreaterThan (highEnergy (b), highEnergy (voice) * 1.3);
            expectLessThan (highEnergy (d), highEnergy (voice) * 0.8);

            StyleProfile rough; rough.grit = 0.8f; rough.breath = 0.5f; rough.compress = 0.8f;
            auto g = voice;
            ToneShaper::process (g, sr, rough);
            expectWithinAbsoluteError ((double) g.getRMSLevel (0, 0, n), rmsIn, rmsIn * 0.02);
            expectLessThan (g.getMagnitude (0, 0, n), 0.99f);
            expectGreaterThan (highEnergy (g), highEnergy (voice) * 1.2);

            StyleLibrary lib;
            expect (lib.get ("gospel").runs > lib.get ("pop").runs);
            expect (lib.get ("rock").grit > 0.3f && lib.get ("lo-fi").brightness < 0.0f);
            expect (lib.get ("natural").hasTone() == false);
        }
    }
};

class TimingTests final : public juce::UnitTest
{
public:
    TimingTests() : juce::UnitTest ("Timing", "VocalEngine") {}

    static int firstLoud (const juce::AudioBuffer<float>& b, int from, int to, float thresh)
    {
        for (int i = from; i < to; ++i)
            if (std::abs (b.getSample (0, i)) > thresh) return i;
        return -1;
    }

    void runTest() override
    {
        const double sr = 48000.0;
        beginTest ("Grid snap with swing");
        {
            expectWithinAbsoluteError (TimeWarp::nearestGridPpq (0.26, 16, 0.0f), 0.25, 1.0e-9);
            expectWithinAbsoluteError (TimeWarp::nearestGridPpq (0.49, 16, 0.0f), 0.5, 1.0e-9);
            expectWithinAbsoluteError (TimeWarp::nearestGridPpq (0.30, 16, 1.0f), 0.25 * 4.0 / 3.0, 1.0e-9);
            expectWithinAbsoluteError (TimeWarp::nearestGridPpq (-0.01, 16, 0.0f), 0.0, 1.0e-9);
            expectWithinAbsoluteError (TimeWarp::nearestGridPpq (3.9, 4, 0.0f), 4.0, 1.0e-9);
        }

        beginTest ("Warp quantizes onsets, keeps note lengths, stays monotonic");
        {
            TempoMap tm (sr);
            tm.addPoint ({ 0, 0.0, 120.0, 4, 4 });   // one beat = 0.5 s
            Note a; a.start = (std::int64_t) (0.56 * sr); a.end = (std::int64_t) (0.90 * sr); a.correction = 1.0f;
            Note b; b.start = (std::int64_t) (1.47 * sr); b.end = (std::int64_t) (1.80 * sr); b.correction = 0.0f;
            NotePlan plan { { a, b } };
            StyleProfile st; st.quantize = 1.0f; st.grid = 4;
            const auto len = (std::int64_t) (3.0 * sr);

            expect (TimeWarp::movesTime (plan, st));
            const auto w = TimeWarp::build (plan, st, tm, len);
            expect (! w.isIdentity());
            expectWithinAbsoluteError (w.outputAt ((double) a.start), 0.5 * sr, 1.0);
            expectWithinAbsoluteError (w.outputAt ((double) a.end) - w.outputAt ((double) a.start), (double) (a.end - a.start), 1.0);
            expectWithinAbsoluteError (w.outputAt ((double) b.start), (double) b.start, 1.0); // uncorrected note stays put
            expectWithinAbsoluteError (w.inputAt (w.outputAt (12345.0)), 12345.0, 1.0e-6);
            expectWithinAbsoluteError (w.outputAt ((double) len), (double) len, 1.0e-6);
            for (size_t i = 1; i < w.anchors.size(); ++i)
                expect (w.anchors[i].out > w.anchors[i - 1].out && w.anchors[i].in > w.anchors[i - 1].in);

            // Half quantize goes halfway; no quantize and no nudges = identity.
            st.quantize = 0.5f;
            expectWithinAbsoluteError (TimeWarp::build (plan, st, tm, len).outputAt ((double) a.start), 0.53 * sr, 1.0);
            st.quantize = 0.0f;
            expect (! TimeWarp::movesTime (plan, st));
            expect (TimeWarp::build (plan, st, tm, len).isIdentity());

            // A huge nudge is limited by the gap stretch limits instead of folding time.
            plan.notes[0].timeShift = (std::int64_t) (5.0 * sr);
            const auto w2 = TimeWarp::build (plan, st, tm, len);
            for (size_t i = 1; i < w2.anchors.size(); ++i)
                expect (w2.anchors[i].out > w2.anchors[i - 1].out);
            expectWithinAbsoluteError (w2.anchors.back().out, (double) len, 1.0e-6);
        }

        beginTest ("Nudged note is re-timed by PSOLA with pitch kept; untouched audio is bit-exact");
        {
            juce::AudioBuffer<float> b (1, (int) (sr * 2.5));
            b.clear();
            NoteTests::addVoiceNote (b, sr, 0.10, 0.50, 57.0f, 0.0f);
            NoteTests::addVoiceNote (b, sr, 1.00, 0.60, 60.0f, 0.0f);
            PitchTrack t;
            expect (PitchTracker::track (b, sr, {}, nullptr, t).wasOk());
            auto notes = NoteSegmenter::segment (t, {});
            expectEquals ((int) notes.size(), 2);
            if (notes.size() != 2) return;
            notes[1].timeShift = (std::int64_t) (0.10 * sr); // 100 ms late

            NotePlan plan { notes };
            StyleProfile st;
            expect (RenderPlan::forNotes (plan, st).rendererId == RenderPlan::pitchCorrectId);
            NotePlan plain { notes }; plain.notes[1].timeShift = 0;
            expect (RenderPlan::forNotes (plan, st).hash() != RenderPlan::forNotes (plain, st).hash());

            const int n = b.getNumSamples();
            std::vector<float> f0 ((size_t) n);
            for (int i = 0; i < n; ++i) f0[(size_t) i] = t.frames[(size_t) juce::jmin ((int) t.frames.size() - 1, i / t.hopSize)].f0;
            const std::vector<float> shift ((size_t) n, 0.0f);
            const auto warp = TimeWarp::build (plan, st, TempoMap (sr), n);
            juce::AudioBuffer<float> out;
            PitchCorrectRenderer::psolaShift (b, out, sr, f0, shift, warp);
            expectEquals (out.getNumSamples(), n);

            const int inOnset = firstLoud (b, (int) (0.8 * sr), n, 0.05f);
            const int outOnset = firstLoud (out, (int) (0.8 * sr), n, 0.05f);
            const double movedMs = 1000.0 * (outOnset - inOnset) / sr;
            logMessage ("Second note onset moved by " + juce::String (movedMs, 1) + " ms (asked 100)");
            expectWithinAbsoluteError (movedMs, 100.0, 12.0);

            PitchTrack t2;
            expect (PitchTracker::track (out, sr, {}, nullptr, t2).wasOk());
            const float f = t2.frames[(size_t) (1.45 * sr / t2.hopSize)].f0;
            const float cents = 1200.0f * std::log2 (f / 261.6256f);
            logMessage ("Moved note is " + juce::String (cents, 1) + " cents from C4");
            expectWithinAbsoluteError (cents, 0.0f, 10.0f);

            bool exact = true;
            for (int i = 0; i < (int) (0.5 * sr); ++i)
                exact = exact && juce::exactlyEqual (out.getSample (0, i), b.getSample (0, i));
            expect (exact, "audio before the moved stretch must be untouched");

            const float inRms = b.getRMSLevel (0, (int) (1.1 * sr), (int) (0.4 * sr));
            const float outRms = out.getRMSLevel (0, (int) (1.2 * sr), (int) (0.4 * sr));
            expectWithinAbsoluteError (outRms / inRms, 1.0f, 0.25f);
        }

        beginTest ("Split and merge note blobs");
        {
            PitchTrack t; t.sampleRate = sr; t.hopSize = 240;
            for (int i = 0; i < 400; ++i)   // 2 s: first second A3, second second C4
                t.frames.push_back ({ i < 200 ? 220.0f : 261.6256f, 1.0f, 0.1f });
            Note n; n.start = 0; n.end = (std::int64_t) (2.0 * sr); n.detectedMidi = 58.0f; n.targetMidi = 57.0f;
            n.correction = 0.7f; n.timeShift = 100;
            std::vector<Note> notes { n };
            const auto minLen = (std::int64_t) (0.03 * sr);

            expect (! NoteEdit::split (notes, 0, 10, &t, minLen), "too close to the edge");
            expect (! NoteEdit::split (notes, 3, 1000, &t, minLen), "bad index");
            expect (NoteEdit::split (notes, 0, (std::int64_t) sr, &t, minLen));
            expectEquals ((int) notes.size(), 2);
            expect (notes[0].end == notes[1].start && notes[1].end == n.end);
            expectWithinAbsoluteError (notes[0].detectedMidi, 57.0f, 0.01f);
            expectWithinAbsoluteError (notes[1].detectedMidi, 60.0f, 0.01f);
            expect (notes[1].timeShift == 100 && notes[1].userEdited && notes[1].correction > 0.69f);

            expect (! NoteEdit::mergeWithNext (notes, 1, &t), "no next note");
            expect (NoteEdit::mergeWithNext (notes, 0, &t));
            expectEquals ((int) notes.size(), 1);
            expect (notes[0].start == 0 && notes[0].end == n.end);
            expectWithinAbsoluteError (notes[0].targetMidi, 57.0f, 1.0e-6f);
        }

        beginTest ("Style timing parses swing and is part of the style hash");
        {
            StyleLibrary lib;
            const auto lofi = lib.get ("lo-fi");
            expect (lofi.swing > 0.0f && lofi.quantize > 0.0f);
            expect (lib.get ("trap").quantize > 0.0f);
            expect (lib.get ("natural").quantize <= 0.0f);
            auto a = lofi; a.swing = 0.0f;
            expect (a.hash() != lofi.hash());
        }
    }
};

static TimingTests timingTests;
class VoiceTests final : public juce::UnitTest
{
public:
    VoiceTests() : juce::UnitTest ("Voice", "VocalEngine") {}

    static constexpr int dim = 8;

    // Stand-in content encoder: [1,1,N] audio -> [1,F,dim] features (frame energy in feature 0).
    struct FakeEncoder final : ml::InferenceBackend
    {
        juce::String getName() const override { return "fake encoder"; }
        juce::Result loadModel (const juce::File&) override { return juce::Result::ok(); }
        juce::Result loadModel (const void*, size_t) override { return juce::Result::ok(); }
        std::vector<ml::TensorInfo> getInputs() const override { return { { "source", 3 } }; }
        std::vector<ml::TensorInfo> getOutputs() const override { return { { "embed", 3 } }; }
        juce::Result run (const std::vector<ml::Tensor>& in, std::vector<ml::Tensor>& out) override
        {
            if (in.size() != 1 || in[0].shape.size() != 3) return juce::Result::fail ("bad encoder input");
            const int N = (int) in[0].shape[2];
            const int F = juce::jmax (0, (N - 400) / 320 + 1);
            auto t = ml::Tensor::makeFloat ("embed", { 1, F, dim }, std::vector<float> ((size_t) F * dim, 0.0f));
            for (int f = 0; f < F; ++f)
            {
                double e = 0.0;
                for (int i = 0; i < 400; ++i) { const double x = in[0].data[(size_t) (f * 320 + i)]; e += x * x; }
                t.data[(size_t) f * dim] = (float) std::sqrt (e / 400.0);
                t.data[(size_t) f * dim + 1] = 1.0f;
            }
            out = { t };
            ++calls;
            return juce::Result::ok();
        }
        int calls = 0;
    };

    // Stand-in synthesiser: sings a sine at pitchf, `hop` samples per frame.
    struct FakeSynth final : ml::InferenceBackend
    {
        explicit FakeSynth (int h, bool withPitch = true) : hop (h), pitchInput (withPitch) {}
        juce::String getName() const override { return "fake synth"; }
        juce::Result loadModel (const juce::File&) override { return juce::Result::ok(); }
        juce::Result loadModel (const void*, size_t) override { return juce::Result::ok(); }
        std::vector<ml::TensorInfo> getInputs() const override
        {
            std::vector<ml::TensorInfo> v { { "phone", 3 }, { "phone_lengths", 1 }, { "pitchf", 2 }, { "ds", 1 }, { "rnd", 3 } };
            if (pitchInput) v.push_back ({ "pitch", 2 });
            return v;
        }
        std::vector<ml::TensorInfo> getOutputs() const override { return { { "audio", 3 } }; }
        juce::Result run (const std::vector<ml::Tensor>& in, std::vector<ml::Tensor>& out) override
        {
            const ml::Tensor *phone = nullptr, *pitch = nullptr, *pitchf = nullptr, *rnd = nullptr, *len = nullptr;
            for (auto& t : in)
            {
                if (t.name == "phone") phone = &t;
                if (t.name == "pitch") pitch = &t;
                if (t.name == "pitchf") pitchf = &t;
                if (t.name == "rnd") rnd = &t;
                if (t.name == "phone_lengths") len = &t;
            }
            if (! phone || ! pitch || ! pitchf || ! rnd || ! len) return juce::Result::fail ("missing input");
            const int T = (int) pitchf->shape[1];
            shapesOk = shapesOk && phone->shape == std::vector<std::int64_t> { 1, T, dim } && rnd->shape == std::vector<std::int64_t> { 1, 192, T }
                       && len->ints == std::vector<std::int64_t> { T } && pitch->type == ml::Tensor::Type::int64;
            for (auto p : pitch->ints) { minCoarse = std::min (minCoarse, p); maxCoarse = std::max (maxCoarse, p); }

            auto t = ml::Tensor::makeFloat ("audio", { 1, 1, (std::int64_t) T * hop }, std::vector<float> ((size_t) T * (size_t) hop, 0.0f));
            double phase = 0.0;
            for (int i = 0; i < T * hop; ++i)
            {
                const float f = pitchf->data[(size_t) (i / hop)];
                phase += 2.0 * juce::MathConstants<double>::pi * f / (hop * 100.0);
                t.data[(size_t) i] = f > 0.0f ? 0.3f * (float) std::sin (phase) : 0.0f;
            }
            out = { t };
            ++calls;
            return juce::Result::ok();
        }
        int hop, calls = 0;
        bool pitchInput, shapesOk = true;
        std::int64_t minCoarse = 1000, maxCoarse = -1;
    };

    static VoiceModelInfo testModel()
    {
        VoiceModelInfo m;
        m.id = "test"; m.name = "Test"; m.sampleRate = 40000.0; m.featureDim = dim; m.version = "v1";
        return m;
    }

    static float centsAt (const PitchTrack& t, double sec, float hz)
    {
        const auto f = t.frames[(size_t) juce::jmin ((int) t.frames.size() - 1, (int) (sec * t.sampleRate / t.hopSize))].f0;
        return f > 0.0f ? 1200.0f * std::log2 (f / hz) : 9999.0f;
    }

    void runTest() override
    {
        beginTest ("RVC pitch bins");
        {
            expect (VoiceConverter::coarsePitch (0.0f) == 1);
            expect (VoiceConverter::coarsePitch (50.0f) == 1);
            expect (VoiceConverter::coarsePitch (1100.0f) == 255);
            expect (VoiceConverter::coarsePitch (5000.0f) == 255);
            bool mono = true;
            for (float hz = 60.0f; hz < 1000.0f; hz *= 1.05f)
                mono = mono && VoiceConverter::coarsePitch (hz * 1.05f) >= VoiceConverter::coarsePitch (hz);
            expect (mono);
        }

        beginTest ("Conversion follows the planned pitch across chunk joins, at the model's rate");
        {
            const double sr = 48000.0;
            const int n = (int) (3.2 * sr);
            std::vector<float> in ((size_t) n);
            for (int i = 0; i < n; ++i) in[(size_t) i] = 0.3f * (float) std::sin (2.0 * juce::MathConstants<double>::pi * 220.0 * i / sr);
            const float target = 246.9417f; // B3
            std::vector<float> f0 ((size_t) n, target);
            for (int i = (int) (1.4 * sr); i < (int) (1.6 * sr); ++i) f0[(size_t) i] = 0.0f; // a breath

            FakeEncoder enc;
            FakeSynth syn (400);
            const auto model = testModel();
            VoiceConverter vc (enc, syn, model);
            expect (vc.validate().isEmpty(), vc.validate());
            VoiceConverter::Settings st;
            st.chunkSeconds = 1.0;
            std::vector<float> out;
            const auto r = vc.convert (in.data(), n, sr, f0, st, nullptr, out);
            expect (r.wasOk(), r.getErrorMessage());
            expectEquals ((int) out.size(), (int) std::llround (n * 40000.0 / sr));
            expect (syn.calls >= 4, "should run in chunks");
            expect (syn.shapesOk, "synth inputs shaped like an RVC export");
            expect (syn.minCoarse >= 1 && syn.maxCoarse <= 255 && syn.maxCoarse > 1);

            juce::AudioBuffer<float> ob (1, (int) out.size());
            ob.copyFrom (0, 0, out.data(), (int) out.size());
            PitchTrack t;
            expect (PitchTracker::track (ob, 40000.0, {}, nullptr, t).wasOk());
            for (double sec : { 0.3, 0.88, 0.95, 1.0, 1.85, 2.6, 2.7 })  // includes chunk overlaps
                expectWithinAbsoluteError (centsAt (t, sec, target), 0.0f, 10.0f);
            const float breath = ob.getRMSLevel (0, (int) (1.45 * 40000), (int) (0.1 * 40000));
            expect (breath < 0.02f, "unvoiced planned pitch stays quiet");
        }

        beginTest ("Models without a pitch input are refused");
        {
            FakeEncoder enc;
            FakeSynth syn (400, false);
            VoiceConverter vc (enc, syn, testModel());
            expect (vc.validate().contains ("pitch"));
            std::vector<float> in (48000, 0.1f), f0 (48000, 200.0f), out;
            expect (vc.convert (in.data(), 48000, 48000.0, f0, {}, nullptr, out).failed());
        }

        beginTest ("Feature bank round trip and nearest-neighbour retrieval");
        {
            const std::vector<float> v { 0, 0,  10, 0,  0, 10 };
            const auto mb = FeatureBank::serialise (v, 2);
            FeatureBank bank;
            expect (bank.loadFromMemory (mb.getData(), mb.getSize()));
            expectEquals (bank.size(), 3);
            const float q[2] = { 9.9f, 0.1f };
            float o[2];
            bank.retrieve (q, o, 1);
            expectWithinAbsoluteError (o[0], 10.0f, 1.0e-4f);
            bank.retrieve (q, o, 3);
            expect (o[0] > 9.0f, "closest vector dominates the blend");
            expect (! bank.loadFromMemory ("nope", 4));
        }

        beginTest ("voice.json loads, and broken voices are reported");
        {
            auto root = tempFolder ("voices");
            auto good = root.getChildFile ("dominic");
            good.createDirectory();
            good.getChildFile ("synth.onnx").replaceWithText ("x");
            good.getChildFile ("enc.onnx").replaceWithText ("x");
            good.getChildFile ("voice.json").replaceWithText (R"({"name": "Dominic", "format": "rvc-v2", "sampleRate": 48000, "contentEncoder": "enc.onnx"})");
            auto bad = root.getChildFile ("broken");
            bad.createDirectory();
            bad.getChildFile ("voice.json").replaceWithText (R"({"name": "Broken"})");

            VoiceLibrary lib;
            const auto problems = lib.scan (root);
            expectEquals ((int) lib.list().size(), 1);
            expectEquals (problems.size(), 1);
            VoiceModelInfo m;
            expect (lib.get ("dominic", m));
            expectEquals (m.name, juce::String ("Dominic"));
            expectEquals (m.hopSize(), 480);
            expect (m.version.isNotEmpty());

            // Plan hash changes with the voice and its version.
            NotePlan none;
            const auto a = RenderPlan::forNotes (none, {}, "dominic", "v1");
            const auto b = RenderPlan::forNotes (none, {}, "dominic", "v2");
            expect (a.rendererId == RenderPlan::voiceConvertId);
            expect (a.hash() != b.hash() && a.hash() != RenderPlan::identity().hash());
        }

        beginTest ("Voice renderer: plan -> pitch/timing -> model -> engine rate");
        {
            const double sr = 44100.0;
            auto folder = tempFolder ("voice-render");
            juce::AudioBuffer<float> take (1, (int) (2.0 * sr));
            take.clear();
            NoteTests::addVoiceNote (take, sr, 0.2, 1.2, 57.0f, -30.0f);
            expect (TakeImporter::writeWav (folder.getChildFile ("take.wav"), take, sr).wasOk());
            PitchTrack track;
            expect (PitchTracker::track (take, sr, {}, nullptr, track).wasOk());
            expect (folder.getChildFile ("take_pitch.bin").replaceWithData (track.serialise().getData(), track.serialise().getSize()));
            auto notes = NoteSegmenter::segment (track, {});
            expectEquals ((int) notes.size(), 1);
            for (auto& nn : notes) nn.correction = 1.0f;

            RegionDescriptor d;
            d.id = "r1";
            d.vocalFile = folder.getChildFile ("take.wav");
            d.pitchFile = folder.getChildFile ("take_pitch.bin");
            d.captureSampleRate = sr;
            d.lengthSamples = take.getNumSamples();
            d.plan = RenderPlan::forNotes ({ notes }, {}, "test", "v1");
            d.planHash = d.plan.hash();

            int made = 0;
            VoiceRenderer vr ([&made]() -> std::unique_ptr<ml::InferenceBackend>
                              {
                                  ++made;
                                  if (made % 2 == 1) return std::make_unique<FakeEncoder>();
                                  return std::make_unique<FakeSynth> (400);
                              },
                              [] (const juce::String& id, VoiceModelInfo& m) { if (id != "test") return false; m = testModel(); return true; });
            JobQueue q (1, std::make_unique<ImmediateDispatcher>());
            RenderedAudio out;
            juce::Result res = juce::Result::ok();
            q.submit ("render", {}, JobPriority::normal, [&] (JobContext& ctx) { res = vr.render (d, d.plan, 48000.0, ctx, out); return res; }, {});
            q.waitUntilIdle (60000);
            expect (res.wasOk(), res.getErrorMessage());
            expectEquals (out.sampleRate, 48000.0);
            expectEquals (out.audio.getNumSamples(), (int) std::llround (take.getNumSamples() * 48000.0 / sr));

            PitchTrack t2;
            expect (PitchTracker::track (out.audio, 48000.0, {}, nullptr, t2).wasOk());
            const float cents = centsAt (t2, 0.8, 220.0f);
            logMessage ("Voice-rendered note is " + juce::String (cents, 1) + " cents from A3 (sung -30)");
            expectWithinAbsoluteError (cents, 0.0f, 10.0f);

            // A second render reuses the loaded models.
            RenderedAudio again;
            q.submit ("render", {}, JobPriority::normal, [&] (JobContext& ctx) { return vr.render (d, d.plan, 48000.0, ctx, again); }, {});
            q.waitUntilIdle (60000);
            expectEquals (made, 2);

            RenderPlan missing = d.plan; missing.voiceId = "nobody";
            RenderedAudio none;
            q.submit ("render", {}, JobPriority::normal, [&] (JobContext& ctx) { res = vr.render (d, missing, 48000.0, ctx, none); return res; }, {});
            q.waitUntilIdle (60000);
            expect (res.failed() && res.getErrorMessage().contains ("not installed"));
        }
    }
};

static VoiceTests voiceTests;
class IntentTests final : public juce::UnitTest
{
public:
    IntentTests() : juce::UnitTest ("Intent", "VocalEngine") {}

    static constexpr double sr = 48000.0;
    static constexpr int hop = 240; // 5 ms

    // Appends frames for a note whose pitch (MIDI) at time t (seconds into the note) is pitchAt(t).
    template <typename F>
    static Note addNote (PitchTrack& t, double startSec, double lenSec, F pitchAt, float conf = 0.95f, float rms = 0.2f)
    {
        const int f0 = (int) (startSec * sr / hop), f1 = (int) ((startSec + lenSec) * sr / hop);
        if ((int) t.frames.size() < f1) t.frames.resize ((size_t) f1);
        std::vector<float> m;
        for (int f = f0; f < f1; ++f)
        {
            const float midi = pitchAt ((f - f0) * hop / sr);
            t.frames[(size_t) f] = { PitchTrack::midiToHz (midi), conf, rms };
            m.push_back (midi);
        }
        Note n;
        n.start = (std::int64_t) f0 * hop;
        n.end = (std::int64_t) f1 * hop;
        std::nth_element (m.begin(), m.begin() + (std::ptrdiff_t) (m.size() / 2), m.end());
        n.detectedMidi = m[m.size() / 2];
        n.targetMidi = std::round (n.detectedMidi);
        return n;
    }

    static PitchTrack emptyTrack() { PitchTrack t; t.sampleRate = sr; t.hopSize = hop; return t; }

    void runTest() override
    {
        KeyEstimate cMajor; cMajor.tonic = 0; cMajor.minor = false;
        const double pi2 = 2.0 * juce::MathConstants<double>::pi;

        beginTest ("Steady note: no intent, full correction");
        {
            auto t = emptyTrack();
            std::vector<Note> notes { addNote (t, 0.1, 0.6, [] (double) { return 63.7f; }) };
            const auto a = NoteIntent::analyse (t, notes, 0, cMajor, 0.2f);
            expectEquals ((int) a.flags, 0);
            expectWithinAbsoluteError (a.scale, 1.0f, 1.0e-6f);
            expectWithinAbsoluteError (a.target, 64.0f, 1.0e-6f); // E, in key
        }

        beginTest ("Vibrato, slide, scoop");
        {
            auto t = emptyTrack();
            std::vector<Note> notes {
                addNote (t, 0.1, 0.9, [pi2] (double x) { return 64.0f + 0.4f * (float) std::sin (pi2 * 5.8 * x); }),
                addNote (t, 1.2, 0.3, [] (double x) { return 57.0f + 3.0f * (float) (x / 0.3); }),
                addNote (t, 1.8, 0.6, [] (double x) { return x < 0.08 ? 65.8f : 67.0f; }),
            };
            const auto vib = NoteIntent::analyse (t, notes, 0, cMajor, 0.2f);
            logMessage ("Vibrato read as " + juce::String (vib.vibratoHz, 1) + " Hz, " + juce::String (vib.vibratoCents, 0) + " cents");
            expect ((vib.flags & NoteIntent::vibrato) != 0, "vibrato");
            expect ((vib.flags & NoteIntent::spoken) == 0, "vibrato isn't speech");
            expectWithinAbsoluteError (vib.scale, 1.0f, 1.0e-6f);

            const auto sl = NoteIntent::analyse (t, notes, 1, cMajor, 0.2f);
            expect ((sl.flags & NoteIntent::slide) != 0, "slide");
            expect (sl.scale <= 0.4f + 1.0e-6f);

            const auto sc = NoteIntent::analyse (t, notes, 2, cMajor, 0.2f);
            expect ((sc.flags & NoteIntent::scoop) != 0, "scoop");
            expect ((sc.flags & NoteIntent::slide) == 0);
        }

        beginTest ("Chosen accidental keeps its semitone; an out-of-tune note still snaps to the key");
        {
            auto t = emptyTrack();
            std::vector<Note> notes {
                addNote (t, 0.1, 0.5, [] (double) { return 66.05f; }),   // F#4 in C major, held and steady
                addNote (t, 0.8, 0.5, [] (double) { return 65.55f; }),   // between F and F#: off-pitch
                addNote (t, 1.5, 0.15, [] (double) { return 66.0f; }),   // F# but too short to count as chosen
            };
            const auto acc = NoteIntent::analyse (t, notes, 0, cMajor, 0.2f);
            expect ((acc.flags & NoteIntent::accidental) != 0);
            expectWithinAbsoluteError (acc.target, 66.0f, 1.0e-6f);
            const auto off = NoteIntent::analyse (t, notes, 1, cMajor, 0.2f);
            expect ((off.flags & NoteIntent::accidental) == 0);
            expect (std::abs (off.target - 66.0f) > 0.5f, "off-pitch note goes to an in-key note");
            const auto brief = NoteIntent::analyse (t, notes, 2, cMajor, 0.2f);
            expect ((brief.flags & NoteIntent::accidental) == 0);
            expect ((NoteIntent::analyse (t, notes, 0, {}, 0.2f).flags & NoteIntent::accidental) == 0, "no key, no accidentals");
        }

        beginTest ("Passing, spoken and quiet notes get less correction");
        {
            auto t = emptyTrack();
            juce::Random rng (7);
            std::vector<Note> notes {
                addNote (t, 0.10, 0.30, [] (double) { return 60.0f; }),
                addNote (t, 0.42, 0.08, [] (double) { return 62.0f; }),  // passing, 20 ms gaps
                addNote (t, 0.52, 0.30, [] (double) { return 64.0f; }),
                addNote (t, 1.00, 0.40, [&rng] (double) { return 55.0f + 2.0f * rng.nextFloat(); }, 0.5f), // spoken
                addNote (t, 1.60, 0.40, [] (double) { return 67.0f; }, 0.95f, 0.01f),                    // ad-lib
            };
            NoteIntent::apply (t, notes, cMajor);
            expect ((notes[1].intent & NoteIntent::passing) != 0, "passing");
            expectWithinAbsoluteError (notes[1].intentScale, 0.5f, 1.0e-6f);
            expect ((notes[3].intent & NoteIntent::spoken) != 0, "spoken");
            expectWithinAbsoluteError (notes[3].intentScale, 0.0f, 1.0e-6f);
            expect ((notes[4].intent & NoteIntent::quiet) != 0, "quiet");
            expect ((notes[0].intent & (NoteIntent::passing | NoteIntent::spoken | NoteIntent::quiet)) == 0);
            expectWithinAbsoluteError (NoteIntent::correctionFor (notes[1], 0.8f, true), 0.4f, 1.0e-6f);
            expectWithinAbsoluteError (NoteIntent::correctionFor (notes[1], 0.8f, false), 0.8f, 1.0e-6f);
            expect (NoteIntent::describe (notes[1].intent).startsWith ("Passing"));
        }

        beginTest ("User-edited targets survive; clear resets intent");
        {
            auto t = emptyTrack();
            std::vector<Note> notes { addNote (t, 0.1, 0.5, [] (double) { return 66.0f; }) };
            notes[0].userEdited = true;
            notes[0].targetMidi = 67.0f;
            NoteIntent::apply (t, notes, cMajor);
            expectWithinAbsoluteError (notes[0].targetMidi, 67.0f, 1.0e-6f);
            expect (notes[0].intent != 0);
            notes[0].userEdited = false;
            NoteIntent::clear (notes, cMajor);
            expect (notes[0].intent == 0 && notes[0].intentScale >= 1.0f);
            expect (std::abs (notes[0].targetMidi - 66.0f) > 0.5f, "back to the key");
        }

        beginTest ("Intended vibrato survives a flattening style, but not hard tune");
        {
            auto t = emptyTrack();
            std::vector<Note> notes { addNote (t, 0.1, 0.9, [pi2] (double x) { return 64.0f + 0.4f * (float) std::sin (pi2 * 5.8 * x); }) };
            notes[0].correction = 1.0f;
            const int n = (int) (1.2 * sr);
            std::vector<float> f0 ((size_t) n, 0.0f);
            for (int i = 0; i < n; ++i) f0[(size_t) i] = t.frames[(size_t) juce::jmin ((int) t.frames.size() - 1, i / hop)].f0;

            StyleProfile flatish; flatish.keepVariation = 0.1f; flatish.retuneMs = 20.0f;
            auto wobble = [&] (const NotePlan& plan, const StyleProfile& st)
            {
                const auto shift = ExpressionLayer::buildShiftCurve (plan, st, f0, sr);
                float lo = 1000.0f, hi = -1000.0f;
                for (int i = (int) (0.4 * sr); i < (int) (0.8 * sr); ++i)
                {
                    const float out = PitchTrack::hzToMidi (f0[(size_t) i]) + shift[(size_t) i];
                    lo = std::min (lo, out); hi = std::max (hi, out);
                }
                return (hi - lo) * 100.0f;
            };
            NotePlan without { notes };
            NotePlan with { notes };
            NoteIntent::apply (t, with.notes, cMajor);
            with.notes[0].correction = 1.0f;
            const float a = wobble (without, flatish), b = wobble (with, flatish);
            logMessage ("Vibrato width after a flattening style: " + juce::String (a, 0) + " cents without intent, " + juce::String (b, 0) + " with");
            expect (b > 2.0f * a, "intent keeps the vibrato");
            StyleProfile hard; hard.keepVariation = 0.0f; hard.retuneMs = 2.0f;
            expect (wobble (with, hard) < 15.0f, "hard tune still flattens");
            expect (with.hash() != without.hash());
        }
    }
};

static IntentTests intentTests;
static StyleTests styleTests;
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
