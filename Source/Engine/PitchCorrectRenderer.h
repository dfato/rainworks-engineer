#pragma once

#include "RenderCache.h"

namespace ve
{
    // Stage 2 renderer: moves each corrected note toward its target pitch with
    // TD-PSOLA, keeping timing, vibrato and formants. Audio outside corrected
    // notes is passed through bit-exact.
    class PitchCorrectRenderer final : public RegionRenderer
    {
    public:
        juce::Result render (const RegionDescriptor&, const RenderPlan&, double targetSampleRate,
                             JobContext&, RenderedAudio& out) override;

        // Core DSP, exposed for tests. `shiftSemitones` is per sample (0 = untouched).
        static void psolaShift (const juce::AudioBuffer<float>& in, juce::AudioBuffer<float>& out,
                                double sampleRate, const std::vector<float>& f0PerSample,
                                const std::vector<float>& shiftSemitones);

        // Per-sample shift curve for a note plan (ramps at note edges).
        static std::vector<float> buildShiftCurve (const NotePlan&, int numSamples, double sampleRate);
    };
}
