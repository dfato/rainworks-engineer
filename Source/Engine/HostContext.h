#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <cstdint>

namespace ve
{
    // Everything we know about the host transport for one block.
    // Hosts may omit any field, so each has a "has" flag. Fields we could
    // derive from others are filled in and marked in `derivedFlags`.
    struct HostSnapshot
    {
        enum Derived : std::uint32_t
        {
            derivedNone        = 0,
            derivedPpq         = 1u << 0, // PPQ computed from samples + BPM
            derivedTimeSamples = 1u << 1, // samples computed from PPQ + BPM
        };

        double sampleRate = 0.0;
        int    blockSize = 0;

        bool   hasPlayHead = false;

        bool   hasBpm = false;
        double bpm = 120.0;

        bool   hasTimeSig = false;
        int    timeSigNumerator = 4;
        int    timeSigDenominator = 4;

        bool   hasPpq = false;
        double ppqPosition = 0.0;

        bool   hasBarStart = false;
        double ppqLastBarStart = 0.0;

        bool         hasTimeSamples = false;
        std::int64_t timeInSamples = 0;

        bool   isPlaying = false;
        bool   isRecording = false;
        bool   isLooping = false;

        bool   hasLoopPoints = false;
        double loopStartPpq = 0.0;
        double loopEndPpq = 0.0;

        std::uint32_t derivedFlags = derivedNone;
        std::uint64_t blockCounter = 0;

        // Quarter notes per beat for the current time signature (4/den).
        double ppqPerBeat() const noexcept { return 4.0 / (double) juce::jmax (1, timeSigDenominator); }
        double samplesPerPpq() const noexcept { return sampleRate * 60.0 / juce::jmax (1.0, bpm); }

        // True when we can place this block on the host timeline in samples.
        bool hasTimelinePosition() const noexcept { return hasTimeSamples; }

        // Converts a PPQ value to a host timeline sample using this block's tempo.
        // Only exact while tempo is constant between here and `ppq`.
        std::int64_t ppqToTimelineSample (double ppq) const noexcept
        {
            return timeInSamples + (std::int64_t) std::llround ((ppq - ppqPosition) * samplesPerPpq());
        }
    };

    namespace HostContext
    {
        // Reads the play head. Real-time safe: no allocation, no locks.
        HostSnapshot read (juce::AudioPlayHead* playHead, double sampleRate, int blockSize, std::uint64_t blockCounter) noexcept;
    }
}
