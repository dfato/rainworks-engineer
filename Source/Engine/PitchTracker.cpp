#include "PitchTracker.h"
#include <juce_dsp/juce_dsp.h>
#include <complex>

namespace ve
{
    juce::MemoryBlock PitchTrack::serialise() const
    {
        juce::MemoryOutputStream out;
        out.writeInt (0x56455054); // "VEPT"
        out.writeInt (1);          // version
        out.writeDouble (sampleRate);
        out.writeInt (hopSize);
        out.writeInt ((int) frames.size());
        for (auto& f : frames)
        {
            out.writeFloat (f.f0);
            out.writeFloat (f.confidence);
            out.writeFloat (f.rms);
        }
        return out.getMemoryBlock();
    }

    bool PitchTrack::deserialise (const juce::MemoryBlock& mb, PitchTrack& out)
    {
        juce::MemoryInputStream in (mb, false);
        if (in.readInt() != 0x56455054 || in.readInt() != 1)
            return false;
        out.sampleRate = in.readDouble();
        out.hopSize = in.readInt();
        const int n = in.readInt();
        if (n < 0 || (juce::int64) n * 12 > in.getNumBytesRemaining() || out.hopSize <= 0)
            return false;
        out.frames.resize ((size_t) n);
        for (auto& f : out.frames)
        {
            f.f0 = in.readFloat();
            f.confidence = in.readFloat();
            f.rms = in.readFloat();
        }
        return true;
    }

    namespace PitchTracker
    {
        juce::Result track (const juce::AudioBuffer<float>& audio, double sr, const Settings& s,
                            JobContext* ctx, PitchTrack& out)
        {
            out = {};
            out.sampleRate = sr;
            const int total = audio.getNumSamples();
            if (total <= 0 || sr <= 0.0)
                return juce::Result::fail ("No audio to track");

            // Mono mix.
            std::vector<float> x ((size_t) total, 0.0f);
            const float g = 1.0f / (float) juce::jmax (1, audio.getNumChannels());
            for (int ch = 0; ch < audio.getNumChannels(); ++ch)
                juce::FloatVectorOperations::addWithMultiply (x.data(), audio.getReadPointer (ch), g, total);

            const int hop = s.hopSize > 0 ? s.hopSize : juce::nextPowerOfTwo ((int) (sr * 0.005)) ;
            const int tauMax = juce::jmin ((int) std::ceil (sr / s.minHz), 4096);
            const int tauMin = juce::jmax (2, (int) std::floor (sr / s.maxHz));
            const int W = juce::nextPowerOfTwo (tauMax);   // integration window
            const int fftOrder = (int) std::log2 ((double) W) + 2; // size 4W >= 3W for linear correlation
            const int N = 1 << fftOrder;
            out.hopSize = hop;

            juce::dsp::FFT fft (fftOrder);
            std::vector<std::complex<float>> a ((size_t) N), b ((size_t) N), A ((size_t) N), B ((size_t) N), c ((size_t) N);
            std::vector<float> seg ((size_t) (2 * W), 0.0f), prefix ((size_t) (2 * W + 1), 0.0f), d ((size_t) (tauMax + 1));

            const float silence = juce::Decibels::decibelsToGain (s.silenceDb);
            const int numFrames = total / hop + 1;
            out.frames.resize ((size_t) numFrames);

            for (int i = 0; i < numFrames; ++i)
            {
                if (ctx != nullptr && (i & 255) == 0)
                {
                    if (ctx->shouldCancel()) return juce::Result::fail ("cancelled");
                    ctx->setProgress ((float) i / (float) numFrames);
                }

                // Window of 2W samples starting W/2 before the frame centre.
                const int start = i * hop - W / 2;
                float energy = 0.0f;
                for (int j = 0; j < 2 * W; ++j)
                {
                    const int k = start + j;
                    seg[(size_t) j] = (k >= 0 && k < total) ? x[(size_t) k] : 0.0f;
                }
                for (int j = 0; j < W; ++j) energy += seg[(size_t) j] * seg[(size_t) j];
                const float rms = std::sqrt (energy / (float) W);

                auto& frame = out.frames[(size_t) i];
                frame.rms = rms;
                if (rms < silence)
                    continue;

                // Cross-correlation c(tau) = sum_{j<W} seg[j] * seg[j + tau] via FFT.
                for (int j = 0; j < N; ++j)
                {
                    a[(size_t) j] = j < W ? seg[(size_t) j] : 0.0f;
                    b[(size_t) j] = j < 2 * W ? seg[(size_t) j] : 0.0f;
                }
                fft.perform (a.data(), A.data(), false);
                fft.perform (b.data(), B.data(), false);
                for (int j = 0; j < N; ++j) A[(size_t) j] = std::conj (A[(size_t) j]) * B[(size_t) j];
                fft.perform (A.data(), c.data(), true); // JUCE's inverse is scaled by 1/N

                prefix[0] = 0.0f;
                for (int j = 0; j < 2 * W; ++j) prefix[(size_t) j + 1] = prefix[(size_t) j] + seg[(size_t) j] * seg[(size_t) j];

                // Difference function and cumulative mean normalisation.
                d[0] = 1.0f;
                float running = 0.0f;
                for (int tau = 1; tau <= tauMax; ++tau)
                {
                    const float eTau = prefix[(size_t) (tau + W)] - prefix[(size_t) tau];
                    const float diff = juce::jmax (0.0f, energy + eTau - 2.0f * c[(size_t) tau].real());
                    running += diff;
                    d[(size_t) tau] = running > 0.0f ? diff * (float) tau / running : 1.0f;
                }

                // First dip below threshold, then its local minimum.
                int best = -1;
                for (int tau = tauMin; tau < tauMax; ++tau)
                {
                    if (d[(size_t) tau] < s.threshold)
                    {
                        while (tau + 1 < tauMax && d[(size_t) tau + 1] < d[(size_t) tau]) ++tau;
                        best = tau;
                        break;
                    }
                }
                if (best < 0)
                {
                    // No dip below threshold: take the global minimum, judge by confidence.
                    best = tauMin;
                    for (int tau = tauMin; tau < tauMax; ++tau)
                        if (d[(size_t) tau] < d[(size_t) best]) best = tau;
                }

                // Parabolic interpolation.
                float tauF = (float) best;
                if (best > 1 && best < tauMax - 1)
                {
                    const float y0 = d[(size_t) best - 1], y1 = d[(size_t) best], y2 = d[(size_t) best + 1];
                    const float den = y0 - 2.0f * y1 + y2;
                    if (std::abs (den) > 1.0e-9f)
                        tauF += 0.5f * (y0 - y2) / den;
                }

                const float conf = juce::jlimit (0.0f, 1.0f, 1.0f - d[(size_t) best]);
                frame.confidence = conf;
                if (conf >= s.minConfidence && tauF > 0.0f)
                    frame.f0 = (float) sr / tauF;
            }

            if (ctx != nullptr) ctx->setProgress (1.0f);
            return juce::Result::ok();
        }
    }
}
