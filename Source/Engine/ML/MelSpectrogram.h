#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <vector>

namespace ve::ml
{
    // Log-mel front end shared by the neural pitch model (FCPE) and the technique
    // classifier. It matches torchfcpe's Wav2Mel exactly in layout:
    //   mono audio -> 16 kHz (windowed-sinc) -> STFT (1024 Hann, hop 160, reflect-padded,
    //   not centred) -> magnitude -> 128 Slaney mel bands 0-8 kHz -> log(max(x, 1e-5)).
    // Frame i is centred on 16 kHz sample i * 160 (10 ms), and there are n / 160 + 1 frames.
    struct MelSpectrogram
    {
        static constexpr int sampleRate = 16000;
        static constexpr int fftSize = 1024;
        static constexpr int hopSize = 160;
        static constexpr int numMels = 128;

        int numFrames = 0;
        std::vector<float> data;   // numFrames x numMels, frame-major

        const float* frame (int i) const noexcept { return data.data() + (size_t) i * numMels; }

        // Mono 16 kHz audio -> mel.
        static MelSpectrogram compute (const std::vector<float>& audio16k);
    };

    // Mono mix of a buffer, resampled to `toRate` with a windowed-sinc low-pass (no aliasing
    // from content above the new Nyquist, which a plain interpolator would fold down).
    std::vector<float> resampleToMono (const juce::AudioBuffer<float>&, double fromRate, double toRate);
    std::vector<float> resample (const float* in, int n, double fromRate, double toRate);

    // librosa.filters.mel (Slaney scale and area normalisation), numMels x (fftSize/2 + 1).
    std::vector<float> slaneyMelBasis (double sampleRate, int fftSize, int numMels, double fMin, double fMax);
}
