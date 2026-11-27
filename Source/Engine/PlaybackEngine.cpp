#include "PlaybackEngine.h"
#include <algorithm>

namespace ve
{
    void PlaybackSnapshot::add (const RegionDescriptor& d, std::shared_ptr<const RenderedAudio> audio, double engineRate)
    {
        if (audio == nullptr || audio->audio.getNumSamples() == 0)
            return;

        const double scale = d.captureSampleRate > 0.0 ? engineRate / d.captureSampleRate : 1.0;
        PlaybackRegion r;
        r.start = (std::int64_t) std::llround ((double) d.startSample * scale);
        r.length = audio->audio.getNumSamples();
        r.order = d.order;
        r.fade = (int) juce::jmin<std::int64_t> ((std::int64_t) std::lround (edgeFadeSeconds * engineRate), r.length / 2);
        r.audio = &audio->audio;
        regions.push_back (r);
        keepAlive.push_back (std::move (audio));
    }

    void PlaybackSnapshot::finalise()
    {
        std::stable_sort (regions.begin(), regions.end(),
                          [] (const PlaybackRegion& a, const PlaybackRegion& b) { return a.order < b.order; });
    }

    PlaybackEngine::PlaybackEngine() = default;

    PlaybackEngine::~PlaybackEngine()
    {
        collectGarbage();
        delete pending.exchange (nullptr);
        delete active;
    }

    void PlaybackEngine::publish (std::unique_ptr<PlaybackSnapshot> snap)
    {
        if (snap != nullptr)
            snap->finalise();
        // If the audio thread never picked up the previous pending snapshot we own it again.
        delete pending.exchange (snap.release(), std::memory_order_acq_rel);
        collectGarbage();
    }

    void PlaybackEngine::collectGarbage()
    {
        const int n = retireFifo.getNumReady();
        if (n <= 0) return;
        int s1, z1, s2, z2;
        retireFifo.prepareToRead (n, s1, z1, s2, z2);
        for (int i = 0; i < z1; ++i) delete retired[(size_t) (s1 + i)];
        for (int i = 0; i < z2; ++i) delete retired[(size_t) (s2 + i)];
        retireFifo.finishedRead (z1 + z2);
    }

    void PlaybackEngine::acquireLatest() noexcept
    {
        if (pending.load (std::memory_order_relaxed) == nullptr)
            return;
        if (active != nullptr && retireFifo.getFreeSpace() < 1)
            return; // message thread is behind; keep playing the current snapshot

        auto* next = pending.exchange (nullptr, std::memory_order_acq_rel);
        if (next == nullptr)
            return;

        if (active != nullptr)
        {
            int s1, z1, s2, z2;
            retireFifo.prepareToWrite (1, s1, z1, s2, z2);
            retired[(size_t) s1] = active;
            retireFifo.finishedWrite (1);
        }
        active = next;
    }

    bool PlaybackEngine::render (juce::AudioBuffer<float>& out, const HostSnapshot& host) noexcept
    {
        acquireLatest();
        return active != nullptr && renderSnapshot (*active, out, host);
    }

    namespace
    {
        // Raised-cosine fade gain for sample p of a region (1 inside, rising over its first `fade`
        // samples and falling over its last). Constant sum with the take underneath.
        inline float edgeGain (const PlaybackRegion& r, std::int64_t p) noexcept
        {
            if (r.fade <= 0) return 1.0f;
            const auto fromEdge = std::min (p, r.length - 1 - p);
            if (fromEdge >= r.fade) return 1.0f;
            const float s = std::sin (juce::MathConstants<float>::halfPi * ((float) fromEdge + 0.5f) / (float) r.fade);
            return s * s;
        }

        // Copies region audio for timeline range [t, t + len) into out at outOffset.
        // Later takes are laid over earlier ones (and over the input), fading at their edges.
        bool renderSegment (const PlaybackSnapshot& snap, juce::AudioBuffer<float>& out,
                            std::int64_t t, int outOffset, int len) noexcept
        {
            bool any = false;
            const int outCh = out.getNumChannels();

            for (const auto& r : snap.regions) // oldest first, so later takes overwrite
            {
                const auto from = std::max (t, r.start);
                const auto to = std::min (t + len, r.start + r.length);
                if (to <= from)
                    continue;

                any = true;
                const int n = (int) (to - from);
                const int dst = outOffset + (int) (from - t);
                const int src = (int) (from - r.start);
                const auto& a = *r.audio;
                const int inCh = a.getNumChannels();
                const bool downmix = outCh == 1 && inCh >= 2;

                // The part of [src, src + n) clear of both fades is a straight copy.
                const int bodyA = juce::jlimit (src, src + n, r.fade);
                const int bodyB = juce::jlimit (bodyA, src + n, (int) (r.length - r.fade));

                auto copyBody = [&] (int s0, int s1)
                {
                    if (s1 <= s0) return;
                    const int d0 = dst + (s0 - src);
                    if (downmix)
                    {
                        out.copyFrom (0, d0, a.getReadPointer (0, s0), s1 - s0, 0.5f);
                        out.addFrom (0, d0, a, 1, s0, s1 - s0, 0.5f);
                    }
                    else
                        for (int ch = 0; ch < outCh; ++ch)
                            out.copyFrom (ch, d0, a, std::min (ch, inCh - 1), s0, s1 - s0);
                };
                auto blendEdge = [&] (int s0, int s1)
                {
                    for (int s = s0; s < s1; ++s)
                    {
                        const float g = edgeGain (r, s);
                        const int d = dst + (s - src);
                        for (int ch = 0; ch < outCh; ++ch)
                        {
                            const float v = downmix ? 0.5f * (a.getSample (0, s) + a.getSample (1, s))
                                                    : a.getSample (std::min (ch, inCh - 1), s);
                            out.setSample (ch, d, out.getSample (ch, d) + g * (v - out.getSample (ch, d)));
                        }
                    }
                };
                blendEdge (src, bodyA);
                copyBody (bodyA, bodyB);
                blendEdge (bodyB, src + n);
            }
            return any;
        }
    }

    bool PlaybackEngine::renderSnapshot (const PlaybackSnapshot& snap, juce::AudioBuffer<float>& out, const HostSnapshot& host) noexcept
    {
        if (! host.isPlaying || ! host.hasTimelinePosition() || snap.regions.empty())
            return false;

        const int n = out.getNumSamples();
        std::int64_t t = host.timeInSamples;

        // Hosts usually split blocks at the loop end, but not all do.
        // If the loop end falls inside this block, wrap to the loop start.
        bool wrap = false;
        std::int64_t loopStart = 0, loopEnd = 0;
        if (host.isLooping && host.hasLoopPoints && host.hasPpq && host.hasBpm)
        {
            loopStart = host.ppqToTimelineSample (host.loopStartPpq);
            loopEnd = host.ppqToTimelineSample (host.loopEndPpq);
            wrap = loopEnd > loopStart && t < loopEnd && t + n > loopEnd;
        }

        if (! wrap)
            return renderSegment (snap, out, t, 0, n);

        const int first = (int) (loopEnd - t);
        bool any = renderSegment (snap, out, t, 0, first);
        any = renderSegment (snap, out, loopStart, first, n - first) || any;
        return any;
    }
}
