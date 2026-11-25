#include "../Style/ToneShaper.h"
#include "VoiceRenderer.h"
#include "../PitchCorrectRenderer.h"

namespace ve
{
    VoiceRenderer::VoiceRenderer()
        : VoiceRenderer ([this]
                         {
                             ml::BackendOptions o;
                             o.threads = juce::jmax (1, juce::SystemStats::getNumCpus() / 2);
                             o.preferGpu = useGpu.load();
                             return ml::createOnnxBackend (o);
                         },
                         [] (const juce::String& id, VoiceModelInfo& m) { return sharedVoiceLibrary().get (id, m); })
    {
    }

    VoiceRenderer::VoiceRenderer (BackendFactory f, VoiceLookup l) : makeBackend (std::move (f)), lookup (std::move (l)) {}

    // Never blocks: a render may be holding `lock` for a while. The models are reloaded
    // on the new device at the start of the next render.
    void VoiceRenderer::setUseGpu (bool on) { useGpu.store (on); }

    juce::String VoiceRenderer::getLastDevice() const
    {
        std::lock_guard<std::mutex> g (deviceLock);
        return lastDevice;
    }

    juce::Result VoiceRenderer::ensureLoaded (const VoiceModelInfo& m)
    {
        if (const bool gpu = useGpu.load(); gpu != loadedOnGpu)
        {
            encoder.reset();
            synth.reset();
            encoderPath = loadedVoice = loadedVersion = {};
            loadedOnGpu = gpu;
        }
        if (encoder == nullptr || encoderPath != m.contentEncoderFile.getFullPathName())
        {
            encoder = makeBackend();
            if (encoder == nullptr)
                return juce::Result::fail ("Voice rendering needs ONNX Runtime: " + ml::onnxUnavailableReason());
            if (auto r = encoder->loadModel (m.contentEncoderFile); r.failed())
            {
                encoder.reset();
                return juce::Result::fail ("Content encoder: " + r.getErrorMessage());
            }
            encoderPath = m.contentEncoderFile.getFullPathName();
        }
        if (synth == nullptr || loadedVoice != m.id || loadedVersion != m.version)
        {
            synth = makeBackend();
            if (synth == nullptr)
                return juce::Result::fail ("Voice rendering needs ONNX Runtime: " + ml::onnxUnavailableReason());
            if (auto r = synth->loadModel (m.synthFile); r.failed())
            {
                synth.reset();
                return juce::Result::fail ("Voice model " + m.name + ": " + r.getErrorMessage());
            }
            bank = {};
            if (m.featuresFile.existsAsFile() && ! bank.load (m.featuresFile))
                bank = {}; // a bad bank only costs some likeness; carry on without it
            loadedVoice = m.id;
            loadedVersion = m.version;
        }
        {
            std::lock_guard<std::mutex> g (deviceLock);
            lastDevice = synth->getDevice();
        }
        return juce::Result::ok();
    }

    juce::Result VoiceRenderer::render (const RegionDescriptor& region, const RenderPlan& plan, double targetRate,
                                        JobContext& ctx, RenderedAudio& out)
    {
        VoiceModelInfo model;
        if (! lookup (plan.voiceId, model))
            return juce::Result::fail ("Voice \"" + plan.voiceId + "\" is not installed");

        // Pitch + timing first, at the take's own rate; keep the planned pitch for the model.
        juce::AudioBuffer<float> shaped;
        double rate = 0.0;
        std::vector<float> plannedF0;
        if (auto r = PitchCorrectRenderer::renderAtSourceRate (region, plan, ctx, shaped, rate, &plannedF0); r.failed())
            return r;
        ctx.setProgress (0.2f);

        const int n = shaped.getNumSamples();
        const int chs = juce::jmax (1, shaped.getNumChannels());
        std::vector<float> mono ((size_t) n, 0.0f);
        for (int ch = 0; ch < shaped.getNumChannels(); ++ch)
            for (int i = 0; i < n; ++i)
                mono[(size_t) i] += shaped.getSample (ch, i) / (float) chs;

        std::vector<float> converted;
        {
            std::lock_guard<std::mutex> g (lock);
            if (auto r = ensureLoaded (model); r.failed())
                return r;
            VoiceConverter vc (*encoder, *synth, model, bank.size() > 0 ? &bank : nullptr);
            if (auto r = vc.convert (mono.data(), n, rate, plannedF0, settings, &ctx, converted); r.failed())
                return r;
        }
        if (ctx.shouldCancel()) return juce::Result::fail ("cancelled");

        // Back to the take's channel count, then to the engine rate, same length as the take.
        juce::AudioBuffer<float> audio (chs, (int) converted.size());
        for (int ch = 0; ch < chs; ++ch)
            audio.copyFrom (ch, 0, converted.data(), (int) converted.size());
        ToneShaper::process (audio, model.sampleRate, plan.style);  // the style's tone, on the new voice
        PitchCorrectRenderer::resample (audio, model.sampleRate, targetRate > 0.0 ? targetRate : rate);
        const double outRate = targetRate > 0.0 ? targetRate : rate;
        const int wanted = (int) std::llround ((double) n * outRate / rate);
        if (audio.getNumSamples() != wanted)
        {
            juce::AudioBuffer<float> fixed (chs, wanted);
            fixed.clear();
            for (int ch = 0; ch < chs; ++ch)
                fixed.copyFrom (ch, 0, audio, ch, 0, juce::jmin (wanted, audio.getNumSamples()));
            audio = std::move (fixed);
        }

        out.audio = std::move (audio);
        out.sampleRate = outRate;
        out.regionId = region.id;
        out.planHash = region.planHash;
        out.peaks = WaveformPeaks::compute (out.audio, outRate);
        ctx.setProgress (1.0f);
        return juce::Result::ok();
    }
}
