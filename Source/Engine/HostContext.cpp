#include "HostContext.h"

namespace ve::HostContext
{
    HostSnapshot read (juce::AudioPlayHead* playHead, double sampleRate, int blockSize, std::uint64_t blockCounter) noexcept
    {
        HostSnapshot s;
        s.sampleRate = sampleRate;
        s.blockSize = blockSize;
        s.blockCounter = blockCounter;

        if (playHead == nullptr)
            return s;

        const auto pos = playHead->getPosition();
        if (! pos.hasValue())
            return s;

        s.hasPlayHead = true;

        if (const auto bpm = pos->getBpm(); bpm.hasValue() && *bpm > 0.0)
        {
            s.hasBpm = true;
            s.bpm = *bpm;
        }

        if (const auto sig = pos->getTimeSignature(); sig.hasValue() && sig->numerator > 0 && sig->denominator > 0)
        {
            s.hasTimeSig = true;
            s.timeSigNumerator = sig->numerator;
            s.timeSigDenominator = sig->denominator;
        }

        if (const auto ppq = pos->getPpqPosition(); ppq.hasValue())
        {
            s.hasPpq = true;
            s.ppqPosition = *ppq;
        }

        if (const auto bar = pos->getPpqPositionOfLastBarStart(); bar.hasValue())
        {
            s.hasBarStart = true;
            s.ppqLastBarStart = *bar;
        }

        if (const auto t = pos->getTimeInSamples(); t.hasValue())
        {
            s.hasTimeSamples = true;
            s.timeInSamples = *t;
        }
        else if (const auto secs = pos->getTimeInSeconds(); secs.hasValue() && sampleRate > 0.0)
        {
            s.hasTimeSamples = true;
            s.timeInSamples = (std::int64_t) std::llround (*secs * sampleRate);
            s.derivedFlags |= HostSnapshot::derivedTimeSamples;
        }

        s.isPlaying   = pos->getIsPlaying();
        s.isRecording = pos->getIsRecording();
        s.isLooping   = pos->getIsLooping();

        if (const auto loop = pos->getLoopPoints(); loop.hasValue() && loop->ppqEnd > loop->ppqStart)
        {
            s.hasLoopPoints = true;
            s.loopStartPpq = loop->ppqStart;
            s.loopEndPpq = loop->ppqEnd;
        }

        // Fill gaps from what we do have.
        if (sampleRate > 0.0 && s.hasBpm)
        {
            if (! s.hasPpq && s.hasTimeSamples)
            {
                s.ppqPosition = (double) s.timeInSamples / s.samplesPerPpq();
                s.hasPpq = true;
                s.derivedFlags |= HostSnapshot::derivedPpq;
            }
            else if (s.hasPpq && ! s.hasTimeSamples)
            {
                s.timeInSamples = (std::int64_t) std::llround (s.ppqPosition * s.samplesPerPpq());
                s.hasTimeSamples = true;
                s.derivedFlags |= HostSnapshot::derivedTimeSamples;
            }
        }

        return s;
    }
}
