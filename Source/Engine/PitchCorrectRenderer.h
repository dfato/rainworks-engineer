#pragma once

#include "RenderCache.h"
#include "TimeWarp.h"

namespace ve
{
    // Moves each corrected note toward its target pitch with TD-PSOLA, keeping
    // vibrato and formants, and (when the plan moves timing) re-times the take
    // with the same grains. Audio outside touched stretches is passed through bit-exact.
    class PitchCorrectRenderer final : public RegionRenderer
    {
    public:
        juce::Result render (const RegionDescriptor&, const RenderPlan&, double targetSampleRate,
                             JobContext&, RenderedAudio& out) override;

        // Core DSP, exposed for tests. `shiftSemitones` and `f0PerSample` are indexed by
        // input (sung) time; 0 shift = untouched. With a warp, output sample t is built
        // from input time warp.inputAt (t). Output length = input length.
        static void psolaShift (const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& out,
                                double sampleRate, const std::vector<float>& f0PerSample,
                                const std::vector<float>& shiftSemitones, const TimeWarp& warp = {});

        // Per-sample shift curve for a note plan (ramps at note edges).
        static std::vector<float> buildShiftCurve (const NotePlan&, int numSamples, double sampleRate);
    };
}
