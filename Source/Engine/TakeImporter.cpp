#include "TakeImporter.h"
#include "RenderCache.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace ve::TakeImporter
{
    juce::Result writeWav (const juce::File& file, const juce::AudioBuffer<float>& audio, double sr)
    {
        file.deleteFile();
        if (! file.getParentDirectory().createDirectory())
            return juce::Result::fail ("Could not create " + file.getParentDirectory().getFullPathName());

        std::unique_ptr<juce::OutputStream> stream = std::make_unique<juce::FileOutputStream> (file);
        if (! static_cast<juce::FileOutputStream*> (stream.get())->openedOk())
            return juce::Result::fail ("Could not open " + file.getFullPathName());

        juce::WavAudioFormat wav;
        auto writer = wav.createWriterFor (stream, juce::AudioFormatWriterOptions{}
                                                       .withSampleRate (sr)
                                                       .withNumChannels (audio.getNumChannels())
                                                       .withBitsPerSample (32)
                                                       .withSampleFormat (juce::AudioFormatWriterOptions::SampleFormat::floatingPoint));
        if (writer == nullptr)
            return juce::Result::fail ("Could not create a WAV writer for " + file.getFileName());
        if (! writer->writeFromAudioSampleBuffer (audio, 0, audio.getNumSamples()))
            return juce::Result::fail ("Could not write " + file.getFileName() + " (disk full?)");
        return juce::Result::ok();
    }

    bool isSupportedAudioFile (const juce::File& f)
    {
        return f.existsAsFile() && f.hasFileExtension ("wav;wave;aif;aiff;flac;ogg;mp3");
    }

    void classifyFiles (const juce::Array<juce::File>& files, juce::File& vocal, juce::File& beat)
    {
        vocal = beat = juce::File();
        juce::Array<juce::File> audio;
        for (auto& f : files)
            if (isSupportedAudioFile (f))
                audio.add (f);

        auto looksLikeBeat = [] (const juce::File& f)
        {
            const auto n = f.getFileNameWithoutExtension().toLowerCase();
            for (auto* k : { "beat", "inst", "music", "track", "mix", "backing", "karaoke" })
                if (n.contains (k)) return true;
            return false;
        };

        for (auto& f : audio)
            if (beat == juce::File() && looksLikeBeat (f) && audio.size() > 1)
                beat = f;
        for (auto& f : audio)
            if (f != beat) { vocal = f; break; }
        if (beat == juce::File())
            for (auto& f : audio)
                if (f != vocal) { beat = f; break; }
    }

    juce::Result importTake (const juce::File& vocalFile, const juce::File& beatFile, const juce::File& dataFolder,
                             double engineRate, const TempoPoint& tempo, JobContext* ctx, CapturedRegion& out)
    {
        if (engineRate <= 0.0)
            engineRate = 48000.0;

        juce::AudioBuffer<float> vocal;
        double rate = 0.0;
        if (auto r = loadAudioFile (vocalFile, engineRate, nullptr, vocal, rate); r.failed())
            return r;
        if (vocal.getNumSamples() == 0)
            return juce::Result::fail (vocalFile.getFileName() + " is empty");
        if (ctx != nullptr) { if (ctx->shouldCancel()) return juce::Result::fail ("cancelled"); ctx->setProgress (0.3f); }

        // Vocal: keep mono or stereo; extra channels are dropped.
        if (vocal.getNumChannels() > 2)
            vocal.setSize (2, vocal.getNumSamples(), true);
        const int length = vocal.getNumSamples();

        juce::AudioBuffer<float> beat;
        const bool hasBeat = beatFile != juce::File();
        if (hasBeat)
        {
            juce::AudioBuffer<float> raw;
            if (auto r = loadAudioFile (beatFile, engineRate, nullptr, raw, rate); r.failed())
                return r;
            // Stereo, same length as the vocal (padded with silence or trimmed).
            beat.setSize (2, length);
            beat.clear();
            const int n = juce::jmin (length, raw.getNumSamples());
            for (int ch = 0; ch < 2 && raw.getNumChannels() > 0; ++ch)
                beat.copyFrom (ch, 0, raw, juce::jmin (ch, raw.getNumChannels() - 1), 0, n);
        }
        if (ctx != nullptr) { if (ctx->shouldCancel()) return juce::Result::fail ("cancelled"); ctx->setProgress (0.6f); }

        out = {};
        out.id = juce::Uuid().toDashedString();
        out.order = juce::Time::currentTimeMillis();
        out.startSample = 0;
        out.startPpq = 0.0;
        out.lengthSamples = length;
        out.sampleRate = engineRate;
        out.numChannels = vocal.getNumChannels();
        out.hasSidechain = hasBeat;
        out.tempoMap = TempoMap (engineRate);
        auto t = tempo;
        t.sample = 0;
        t.ppq = 0.0;
        out.tempoMap.addPoint (t);

        out.vocalFile = CaptureEngine::regionsSubfolder() + "/" + out.id + "_vocal.wav";
        if (auto r = writeWav (dataFolder.getChildFile (out.vocalFile), vocal, engineRate); r.failed())
            return r;

        if (hasBeat)
        {
            out.sidechainFile = CaptureEngine::regionsSubfolder() + "/" + out.id + "_sidechain.wav";
            if (auto r = writeWav (dataFolder.getChildFile (out.sidechainFile), beat, engineRate); r.failed())
                return r;
        }

        if (ctx != nullptr) ctx->setProgress (1.0f);
        return juce::Result::ok();
    }
}
