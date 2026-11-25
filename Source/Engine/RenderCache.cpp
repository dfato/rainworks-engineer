#include "RenderCache.h"
#include <juce_audio_formats/juce_audio_formats.h>

namespace ve
{
    std::uint64_t RenderPlan::hash() const noexcept
    {
        // FNV-1a over the plan's fields. Stable across runs and platforms.
        std::uint64_t h = 1469598103934665603ull;
        auto mix = [&h] (const void* data, size_t n)
        {
            auto* p = static_cast<const unsigned char*> (data);
            for (size_t i = 0; i < n; ++i) { h ^= p[i]; h *= 1099511628211ull; }
        };
        const auto id = rendererId.toStdString();
        mix (id.data(), id.size());
        mix (&rendererVersion, sizeof (rendererVersion));
        if (rendererId != capturedId)
        {
            const auto n = notes.hash();
            mix (&n, sizeof (n));
        }
        return h;
    }

    RenderPlan RenderPlan::forNotes (NotePlan n)
    {
        RenderPlan p;
        p.rendererId = n.isIdentity() ? capturedId : pitchCorrectId;
        p.notes = std::move (n);
        return p;
    }

    std::shared_ptr<const WaveformPeaks> WaveformPeaks::compute (const juce::AudioBuffer<float>& b, double sr, int spp)
    {
        auto p = std::make_shared<WaveformPeaks>();
        p->samplesPerPeak = juce::jmax (1, spp);
        p->sampleRate = sr;
        const int n = b.getNumSamples();
        const int count = (n + p->samplesPerPeak - 1) / p->samplesPerPeak;
        p->mins.assign ((size_t) count, 0.0f);
        p->maxs.assign ((size_t) count, 0.0f);

        for (int i = 0; i < count; ++i)
        {
            const int start = i * p->samplesPerPeak;
            const int len = juce::jmin (p->samplesPerPeak, n - start);
            float lo = 0.0f, hi = 0.0f;
            for (int ch = 0; ch < b.getNumChannels(); ++ch)
            {
                const auto r = juce::FloatVectorOperations::findMinAndMax (b.getReadPointer (ch, start), len);
                lo = juce::jmin (lo, r.getStart());
                hi = juce::jmax (hi, r.getEnd());
            }
            p->mins[(size_t) i] = lo;
            p->maxs[(size_t) i] = hi;
        }
        return p;
    }

    juce::Result loadAudioFile (const juce::File& file, double targetRate, JobContext* ctx,
                                juce::AudioBuffer<float>& out, double& outRate)
    {
        if (! file.existsAsFile())
            return juce::Result::fail ("Missing file: " + file.getFullPathName());

        juce::AudioFormatManager formats;
        formats.registerBasicFormats(); // WAV, AIFF, FLAC, Ogg (+ MP3/WMA where the platform provides them)
        std::unique_ptr<juce::AudioFormatReader> reader (formats.createReaderFor (file));
        if (reader == nullptr)
            return juce::Result::fail ("Unreadable audio file: " + file.getFullPathName());

        const auto len = reader->lengthInSamples;
        if (len > std::numeric_limits<int>::max() / 2)
            return juce::Result::fail ("Region too long: " + file.getFileName());

        const int numCh = (int) reader->numChannels;
        juce::AudioBuffer<float> raw (numCh, (int) len);

        constexpr int chunk = 1 << 16;
        for (juce::int64 pos = 0; pos < len; pos += chunk)
        {
            if (ctx != nullptr && ctx->shouldCancel())
                return juce::Result::fail ("cancelled");
            const int n = (int) juce::jmin<juce::int64> (chunk, len - pos);
            reader->read (&raw, (int) pos, n, pos, true, true);
            if (ctx != nullptr)
                ctx->setProgress (0.8f * (float) (pos + n) / (float) juce::jmax<juce::int64> (1, len));
        }

        const double srcRate = reader->sampleRate;
        if (targetRate <= 0.0 || std::abs (targetRate - srcRate) < 0.5)
        {
            out = std::move (raw);
            outRate = srcRate;
            return juce::Result::ok();
        }

        // Offline resample (Lagrange is fine for the preview path; final renders
        // will be produced at the session rate by later stages).
        const double ratio = srcRate / targetRate;
        const int outLen = (int) std::ceil ((double) len / ratio);
        out.setSize (numCh, outLen);
        for (int ch = 0; ch < numCh; ++ch)
        {
            juce::LagrangeInterpolator interp;
            interp.process (ratio, raw.getReadPointer (ch), out.getWritePointer (ch), outLen, (int) len, 0);
        }
        outRate = targetRate;
        return juce::Result::ok();
    }

    juce::Result CapturedAudioRenderer::render (const RegionDescriptor& region, const RenderPlan&, double targetRate,
                                                JobContext& ctx, RenderedAudio& out)
    {
        double rate = 0.0;
        if (auto r = loadAudioFile (region.vocalFile, targetRate, &ctx, out.audio, rate); r.failed())
            return r;
        out.sampleRate = rate;
        out.regionId = region.id;
        out.planHash = region.planHash;
        out.peaks = WaveformPeaks::compute (out.audio, rate);
        ctx.setProgress (1.0f);
        return juce::Result::ok();
    }

    //==========================================================================
    std::shared_ptr<const RenderedAudio> RenderCache::find (const juce::String& id, std::uint64_t hash) const
    {
        std::lock_guard<std::mutex> lock (mutex);
        auto it = entries.find ({ id, hash });
        return it != entries.end() ? it->second : nullptr;
    }

    std::shared_ptr<const RenderedAudio> RenderCache::findAny (const juce::String& id) const
    {
        std::lock_guard<std::mutex> lock (mutex);
        for (auto& [k, v] : entries)
            if (k.first == id) return v;
        return nullptr;
    }

    void RenderCache::store (std::shared_ptr<const RenderedAudio> r)
    {
        if (r == nullptr) return;
        std::lock_guard<std::mutex> lock (mutex);
        entries[{ r->regionId, r->planHash }] = std::move (r);
    }

    void RenderCache::removeStale (const juce::String& id, std::uint64_t keep)
    {
        std::lock_guard<std::mutex> lock (mutex);
        for (auto it = entries.begin(); it != entries.end();)
            it = (it->first.first == id && it->first.second != keep) ? entries.erase (it) : std::next (it);
    }

    void RenderCache::removeRegion (const juce::String& id)
    {
        std::lock_guard<std::mutex> lock (mutex);
        for (auto it = entries.begin(); it != entries.end();)
            it = it->first.first == id ? entries.erase (it) : std::next (it);
    }

    void RenderCache::clear()
    {
        std::lock_guard<std::mutex> lock (mutex);
        entries.clear();
    }

    std::size_t RenderCache::size() const
    {
        std::lock_guard<std::mutex> lock (mutex);
        return entries.size();
    }

    std::size_t RenderCache::memoryBytes() const
    {
        std::lock_guard<std::mutex> lock (mutex);
        std::size_t total = 0;
        for (auto& [k, v] : entries)
            total += (size_t) v->audio.getNumChannels() * (size_t) v->audio.getNumSamples() * sizeof (float);
        return total;
    }
}
