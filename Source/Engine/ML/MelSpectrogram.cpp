#include "MelSpectrogram.h"
#include <juce_dsp/juce_dsp.h>
#include <cmath>
#include <numeric>

namespace ve::ml
{
    namespace
    {
        constexpr double pi = 3.14159265358979323846;

        // Windowed-sinc tap at offset t (input samples) for a low-pass at `fc` (fraction of
        // the input rate, <= 0.5 * 2) with `zeros` zero crossings either side.
        inline double sincTap (double t, double fc, double halfWidth)
        {
            if (std::abs (t) >= halfWidth) return 0.0;
            const double x = t * fc;
            const double s = std::abs (x) < 1.0e-9 ? 1.0 : std::sin (pi * x) / (pi * x);
            const double w = std::cos (0.5 * pi * t / halfWidth);
            return fc * s * w * w;
        }

        double hzToMelSlaney (double hz)
        {
            const double fSp = 200.0 / 3.0, minLogHz = 1000.0, minLogMel = minLogHz / fSp, logStep = std::log (6.4) / 27.0;
            return hz >= minLogHz ? minLogMel + std::log (hz / minLogHz) / logStep : hz / fSp;
        }

        double melToHzSlaney (double mel)
        {
            const double fSp = 200.0 / 3.0, minLogHz = 1000.0, minLogMel = minLogHz / fSp, logStep = std::log (6.4) / 27.0;
            return mel >= minLogMel ? minLogHz * std::exp (logStep * (mel - minLogMel)) : fSp * mel;
        }
    }

    std::vector<float> resample (const float* in, int n, double fromRate, double toRate)
    {
        if (n <= 0 || fromRate <= 0.0 || toRate <= 0.0) return {};
        const auto from = (std::int64_t) std::llround (fromRate), to = (std::int64_t) std::llround (toRate);
        if (from == to) return { in, in + n };

        const auto g = std::gcd (from, to);
        const std::int64_t L = to / g, M = from / g;   // output n sits at input n * M / L
        const double fc = 0.99 * std::min (1.0, (double) to / (double) from);
        const int zeros = 32;
        const double halfWidth = zeros / fc;
        const int K = (int) std::ceil (halfWidth);
        const int taps = 2 * K + 1;

        const std::int64_t outLen = ((std::int64_t) n * L + M - 1) / M;
        std::vector<float> out ((size_t) outLen, 0.0f);

        // Precomputed polyphase table when the number of phases is reasonable.
        const bool table = L <= 2048;
        std::vector<float> h;
        if (table)
        {
            h.resize ((size_t) (L * taps));
            for (std::int64_t p = 0; p < L; ++p)
            {
                const double frac = (double) p / (double) L;
                for (int k = -K; k <= K; ++k)
                    h[(size_t) (p * taps + (k + K))] = (float) sincTap ((double) k - frac, fc, halfWidth);
            }
        }

        std::vector<float> tmp ((size_t) taps);
        for (std::int64_t j = 0; j < outLen; ++j)
        {
            const std::int64_t pos = j * M;
            const std::int64_t base = pos / L, phase = pos % L;
            const float* taps_ = nullptr;
            if (table)
                taps_ = h.data() + phase * taps;
            else
            {
                const double frac = (double) phase / (double) L;
                for (int k = -K; k <= K; ++k) tmp[(size_t) (k + K)] = (float) sincTap ((double) k - frac, fc, halfWidth);
                taps_ = tmp.data();
            }
            double acc = 0.0;
            const std::int64_t lo = std::max<std::int64_t> (0, base - K), hi = std::min<std::int64_t> (n - 1, base + K);
            for (std::int64_t i = lo; i <= hi; ++i)
                acc += (double) in[i] * taps_[i - base + K];
            out[(size_t) j] = (float) acc;
        }
        return out;
    }

    std::vector<float> resampleToMono (const juce::AudioBuffer<float>& audio, double fromRate, double toRate)
    {
        const int n = audio.getNumSamples();
        if (n <= 0 || audio.getNumChannels() <= 0) return {};
        std::vector<float> mono ((size_t) n, 0.0f);
        const float g = 1.0f / (float) audio.getNumChannels();
        for (int ch = 0; ch < audio.getNumChannels(); ++ch)
            juce::FloatVectorOperations::addWithMultiply (mono.data(), audio.getReadPointer (ch), g, n);
        return resample (mono.data(), n, fromRate, toRate);
    }

    std::vector<float> slaneyMelBasis (double sr, int nFft, int nMels, double fMin, double fMax)
    {
        const int bins = nFft / 2 + 1;
        std::vector<double> fftFreqs ((size_t) bins);
        for (int i = 0; i < bins; ++i) fftFreqs[(size_t) i] = sr * i / nFft;

        const double mMin = hzToMelSlaney (fMin), mMax = hzToMelSlaney (fMax);
        std::vector<double> melF ((size_t) nMels + 2);
        for (int i = 0; i < nMels + 2; ++i)
            melF[(size_t) i] = melToHzSlaney (mMin + (mMax - mMin) * i / (nMels + 1));

        std::vector<float> w ((size_t) (nMels * bins), 0.0f);
        for (int m = 0; m < nMels; ++m)
        {
            const double lo = melF[(size_t) m], c = melF[(size_t) m + 1], hi = melF[(size_t) m + 2];
            const double enorm = 2.0 / (hi - lo);
            for (int b = 0; b < bins; ++b)
            {
                const double lower = (fftFreqs[(size_t) b] - lo) / (c - lo);
                const double upper = (hi - fftFreqs[(size_t) b]) / (hi - c);
                w[(size_t) (m * bins + b)] = (float) (std::max (0.0, std::min (lower, upper)) * enorm);
            }
        }
        return w;
    }

    MelSpectrogram MelSpectrogram::compute (const std::vector<float>& y)
    {
        MelSpectrogram out;
        const int n = (int) y.size();
        const int targetFrames = n / hopSize + 1;
        out.numFrames = targetFrames;
        out.data.assign ((size_t) targetFrames * numMels, std::log (1.0e-5f));
        if (n == 0) return out;

        // Pad as torchfcpe does: reflect when the signal is long enough, zeros otherwise.
        const int padL = (fftSize - hopSize) / 2;
        const int padR = std::max ((fftSize - hopSize + 1) / 2, fftSize - n - padL);
        const bool reflect = padR < n;
        std::vector<float> x ((size_t) (n + padL + padR), 0.0f);
        for (int i = 0; i < (int) x.size(); ++i)
        {
            int k = i - padL;
            if (k < 0 || k >= n)
            {
                if (! reflect) continue;
                if (k < 0) k = -k;
                if (k >= n) k = 2 * (n - 1) - k;
                if (k < 0 || k >= n) continue;
            }
            x[(size_t) i] = y[(size_t) k];
        }

        static const std::vector<float> basis = slaneyMelBasis (sampleRate, fftSize, numMels, 0.0, 8000.0);
        static const std::vector<float> window = []
        {
            std::vector<float> w ((size_t) fftSize);
            for (int i = 0; i < fftSize; ++i) w[(size_t) i] = (float) (0.5 - 0.5 * std::cos (2.0 * pi * i / fftSize)); // periodic
            return w;
        }();

        const int bins = fftSize / 2 + 1;
        const int stftFrames = 1 + ((int) x.size() - fftSize) / hopSize;
        juce::dsp::FFT fft (10);
        std::vector<float> buf ((size_t) fftSize * 2), mag ((size_t) bins);
        const float logFloor = std::log (1.0e-5f);

        for (int f = 0; f < targetFrames; ++f)
        {
            const int sf = std::min (f, stftFrames - 1);   // past the end: repeat the last frame
            const float* src = x.data() + (size_t) sf * hopSize;
            for (int i = 0; i < fftSize; ++i) buf[(size_t) i] = src[i] * window[(size_t) i];
            std::fill (buf.begin() + fftSize, buf.end(), 0.0f);
            fft.performRealOnlyForwardTransform (buf.data(), true);
            for (int b = 0; b < bins; ++b)
            {
                const float re = buf[(size_t) (2 * b)], im = buf[(size_t) (2 * b + 1)];
                mag[(size_t) b] = std::sqrt (re * re + im * im + 1.0e-9f);
            }
            float* dst = out.data.data() + (size_t) f * numMels;
            for (int m = 0; m < numMels; ++m)
            {
                const float* w = basis.data() + (size_t) m * bins;
                float acc = 0.0f;
                for (int b = 0; b < bins; ++b) acc += w[b] * mag[(size_t) b];
                dst[m] = acc > 1.0e-5f ? std::log (acc) : logFloor;
            }
        }
        return out;
    }
}
