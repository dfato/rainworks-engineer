#pragma once

#include "CaptureEngine.h"
#include "JobQueue.h"

namespace ve
{
    // Builds a take from audio files instead of real-time capture, so a whole
    // song can be brought in without playing it through.
    //
    // The vocal (and optional beat/instrumental) are resampled to the engine
    // rate and written into the data folder as 32-bit float WAVs, exactly like
    // captured takes. The take starts at the beginning of the song (sample 0,
    // PPQ 0), which matches stems exported from FL Studio from bar 1.
    namespace TakeImporter
    {
        juce::Result importTake (const juce::File& vocalFile,
                                 const juce::File& beatFile,      // juce::File() for none
                                 const juce::File& dataFolder,
                                 double engineSampleRate,
                                 const TempoPoint& tempo,         // tempo/time signature to assume
                                 JobContext* ctx,
                                 CapturedRegion& out);

        // Writes a 32-bit float WAV. Overwrites.
        juce::Result writeWav (const juce::File&, const juce::AudioBuffer<float>&, double sampleRate);

        // Given dropped/selected files, picks which is the vocal and which the beat.
        // Names with vocal/vox/acapella/... count as the vocal; names with
        // beat/inst/music/track/mix/backing/karaoke/808 count as the beat.
        // A single file whose name says beat is returned as the beat only
        // (vocal left empty), so dropping just the beat never makes a vocal take.
        void classifyFiles (const juce::Array<juce::File>&, juce::File& vocal, juce::File& beat);
        bool nameLooksLikeBeat (const juce::File&);
        bool nameLooksLikeVocal (const juce::File&);

        // Writes the part of a beat file that lies under one take as that take's
        // sidechain WAV (stereo, take rate, take length; silence where the beat
        // has no audio). beatStartSeconds is where the beat file starts on the
        // song timeline (0 = bar 1). relativeOut is the path inside dataFolder.
        juce::Result writeBeatForTake (const juce::File& beatFile, double beatStartSeconds,
                                       const juce::File& dataFolder, const juce::String& takeId,
                                       std::int64_t takeStartSample, std::int64_t takeLengthSamples,
                                       double takeSampleRate, JobContext* ctx, juce::String& relativeOut);

        bool isSupportedAudioFile (const juce::File&);
    }
}
