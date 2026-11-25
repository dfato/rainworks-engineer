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
        // Names containing beat/inst/music/track/mix count as the beat.
        void classifyFiles (const juce::Array<juce::File>&, juce::File& vocal, juce::File& beat);

        bool isSupportedAudioFile (const juce::File&);
    }
}
