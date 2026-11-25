#pragma once

#include <juce_core/juce_core.h>
#include <mutex>
#include <vector>

namespace ve
{
    // One singer's voice model, as a folder in Documents/RainWorks/Engineer/Voices:
    //
    //   <folder>/voice.json   { "name": "Dominic", "format": "rvc-v2", "sampleRate": 40000,
    //                           "synth": "synth.onnx", "features": "features.bin", "speakerId": 0,
    //                           "featureDim": 768, "contentEncoder": "" }
    //   <folder>/synth.onnx   the exported synthesiser (inputs phone, phone_lengths, pitch, pitchf, ds, rnd)
    //   <folder>/features.bin optional retrieval bank (see FeatureBank)
    //
    // The content encoder (ContentVec / HuBERT layer 12, 16 kHz in) is shared by all voices:
    // Documents/RainWorks/Engineer/Models/content-encoder.onnx, unless voice.json names another.
    struct VoiceModelInfo
    {
        juce::String id;           // folder name, stable across renames of "name"
        juce::String name;
        juce::String format = "rvc-v2";
        double sampleRate = 40000.0;
        int speakerId = 0;
        int featureDim = 768;
        juce::File folder, synthFile, featuresFile, contentEncoderFile;
        juce::String version;      // changes when the model files change (feeds the render plan hash)

        int hopSize() const noexcept { return juce::roundToInt (sampleRate / 100.0); } // 100 frames per second
        bool isUsable (juce::String* why = nullptr) const;

        // Parses voice.json in a folder. Returns an error message, or empty on success.
        static juce::String load (const juce::File& folder, VoiceModelInfo& out);
    };

    // Nearest-neighbour retrieval bank: feature vectors from the singer's training audio.
    // File: "VEFB" magic, uint32 version (1), uint32 count, uint32 dim, then count*dim float32.
    class FeatureBank
    {
    public:
        bool load (const juce::File&);
        bool loadFromMemory (const void* data, size_t size);
        static juce::MemoryBlock serialise (const std::vector<float>& vectors, int dim);

        int size() const noexcept { return count; }
        int getDim() const noexcept { return dim; }

        // Blend of the k nearest vectors (weights 1/d^2) to `query` (dim floats), written to `out`.
        void retrieve (const float* query, float* out, int k = 8) const;

    private:
        std::vector<float> data, norms;
        int count = 0, dim = 0;
    };

    class VoiceLibrary
    {
    public:
        static juce::File voicesFolder();
        static juce::File modelsFolder();
        static juce::File defaultContentEncoder();

        // Rescans the folder. Returns problems found (one line per bad voice).
        juce::StringArray scan (const juce::File& folder = voicesFolder());

        std::vector<VoiceModelInfo> list() const;
        bool get (const juce::String& id, VoiceModelInfo& out) const;

    private:
        mutable std::mutex lock;
        std::vector<VoiceModelInfo> voices;
    };

    VoiceLibrary& sharedVoiceLibrary();
}
