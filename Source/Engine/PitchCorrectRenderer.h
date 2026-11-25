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

        // Loads the take and applies pitch + timing at the take's own rate. `plannedF0`, if
        // given, receives the pitch (Hz, 0 = unvoiced) each output sample should have.
        static juce::Result renderAtSourceRate (const RegionDescriptor&, const RenderPlan&, JobContext&,
                                                juce::AudioBuffer<float>& out, double& rate, std::vector<float>* plannedF0);

        // In-place sample-rate conversion (no-op when the rates match).
        static void resample (juce::AudioBuffer<float>&, double fromRate, double toRate);

        // Per-sample shift curve for a note plan (ramps at note edges).
        static std::vector<float> buildShiftCurve (const NotePlan&, int numSamples, double sampleRate);
    };
}
