#include "VoiceModel.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace ve
{
    bool VoiceModelInfo::isUsable (juce::String* why) const
    {
        auto fail = [why] (const juce::String& m) { if (why != nullptr) *why = m; return false; };
        if (format != "rvc-v2" && format != "rvc-v1") return fail ("unsupported format \"" + format + "\"");
        if (! synthFile.existsAsFile()) return fail ("missing " + synthFile.getFileName());
        if (! contentEncoderFile.existsAsFile()) return fail ("missing content encoder " + contentEncoderFile.getFullPathName());
        if (sampleRate < 8000.0 || sampleRate > 96000.0) return fail ("bad sampleRate");
        return true;
    }

    juce::String VoiceModelInfo::load (const juce::File& folder, VoiceModelInfo& out)
    {
        const auto jsonFile = folder.getChildFile ("voice.json");
        if (! jsonFile.existsAsFile()) return "no voice.json";
        const auto v = juce::JSON::parse (jsonFile.loadFileAsString());
        if (! v.isObject()) return "voice.json is not valid JSON";

        VoiceModelInfo m;
        m.id = folder.getFileName();
        m.folder = folder;
        m.name = v.getProperty ("name", m.id).toString();
        m.format = v.getProperty ("format", m.format).toString();
        m.sampleRate = (double) v.getProperty ("sampleRate", m.sampleRate);
        m.speakerId = (int) v.getProperty ("speakerId", 0);
        m.featureDim = (int) v.getProperty ("featureDim", m.format == "rvc-v1" ? 256 : 768);
        m.synthFile = folder.getChildFile (v.getProperty ("synth", "synth.onnx").toString());
        const auto feats = v.getProperty ("features", "").toString();
        if (feats.isNotEmpty()) m.featuresFile = folder.getChildFile (feats);
        const auto enc = v.getProperty ("contentEncoder", "").toString();
        m.contentEncoderFile = enc.isNotEmpty() ? folder.getChildFile (enc) : VoiceLibrary::defaultContentEncoder();

        juce::String ver;
        for (auto& f : { m.synthFile, m.featuresFile, m.contentEncoderFile, jsonFile })
            if (f.existsAsFile())
                ver << f.getFileName() << ':' << f.getSize() << ':' << f.getLastModificationTime().toMilliseconds() << ';';
        m.version = juce::String::toHexString (ver.hashCode64());

        juce::String why;
        if (! m.isUsable (&why)) return why;
        out = std::move (m);
        return {};
    }

    //==========================================================================
    bool FeatureBank::load (const juce::File& f)
    {
        juce::MemoryBlock mb;
        return f.loadFileAsData (mb) && loadFromMemory (mb.getData(), mb.getSize());
    }

    bool FeatureBank::loadFromMemory (const void* p, size_t size)
    {
        auto* b = static_cast<const char*> (p);
        if (size < 16 || std::memcmp (b, "VEFB", 4) != 0) return false;
        std::uint32_t hdr[3];
        std::memcpy (hdr, b + 4, sizeof (hdr));
        if (hdr[0] != 1 || hdr[2] == 0 || hdr[2] > 4096) return false;
        const size_t floats = (size_t) hdr[1] * hdr[2];
        if (size < 16 + floats * sizeof (float)) return false;
        count = (int) hdr[1];
        dim = (int) hdr[2];
        data.resize (floats);
        std::memcpy (data.data(), b + 16, floats * sizeof (float));
        norms.resize ((size_t) count);
        for (int i = 0; i < count; ++i)
        {
            double s = 0.0;
            for (int d = 0; d < dim; ++d) { const double x = data[(size_t) i * (size_t) dim + (size_t) d]; s += x * x; }
            norms[(size_t) i] = (float) s;
        }
        return true;
    }

    juce::MemoryBlock FeatureBank::serialise (const std::vector<float>& vectors, int d)
    {
        juce::MemoryOutputStream out;
        out.write ("VEFB", 4);
        const std::uint32_t hdr[3] = { 1u, (std::uint32_t) (vectors.size() / (size_t) d), (std::uint32_t) d };
        out.write (hdr, sizeof (hdr));
        out.write (vectors.data(), vectors.size() / (size_t) d * (size_t) d * sizeof (float));
        return out.getMemoryBlock();
    }

    void FeatureBank::retrieve (const float* q, float* out, int k) const
    {
        if (count == 0) { std::memcpy (out, q, sizeof (float) * (size_t) dim); return; }
        k = juce::jlimit (1, count, k);

        // Squared distance = |x|^2 - 2 x.q + |q|^2; |q|^2 doesn't change the ranking.
        double qq = 0.0;
        for (int d = 0; d < dim; ++d) qq += (double) q[d] * q[d];
        std::vector<std::pair<float, int>> best; // (distance, index), kept sorted, size <= k
        best.reserve ((size_t) k + 1);
        for (int i = 0; i < count; ++i)
        {
            const float* x = data.data() + (size_t) i * (size_t) dim;
            float dot = 0.0f;
            for (int d = 0; d < dim; ++d) dot += x[d] * q[d];
            const float dist = juce::jmax (0.0f, norms[(size_t) i] - 2.0f * dot + (float) qq);
            if ((int) best.size() < k || dist < best.back().first)
            {
                best.insert (std::upper_bound (best.begin(), best.end(), std::make_pair (dist, i)), { dist, i });
                if ((int) best.size() > k) best.pop_back();
            }
        }

        std::fill (out, out + dim, 0.0f);
        double wsum = 0.0;
        for (auto& [dist, i] : best)
        {
            const double w = 1.0 / (juce::jmax (1.0e-6, (double) dist));
            wsum += w;
            const float* x = data.data() + (size_t) i * (size_t) dim;
            for (int d = 0; d < dim; ++d) out[d] += (float) (w * x[d]);
        }
        const float inv = (float) (1.0 / wsum);
        for (int d = 0; d < dim; ++d) out[d] *= inv;
    }

    //==========================================================================
    juce::File VoiceLibrary::voicesFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("RainWorks/Engineer/Voices");
    }

    juce::File VoiceLibrary::modelsFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory).getChildFile ("RainWorks/Engineer/Models");
    }

    juce::File VoiceLibrary::defaultContentEncoder() { return modelsFolder().getChildFile ("content-encoder.onnx"); }

    juce::StringArray VoiceLibrary::scan (const juce::File& folder)
    {
        juce::StringArray problems;
        std::vector<VoiceModelInfo> found;
        for (auto& dir : folder.findChildFiles (juce::File::findDirectories, false))
        {
            VoiceModelInfo m;
            const auto err = VoiceModelInfo::load (dir, m);
            if (err.isEmpty()) found.push_back (std::move (m));
            else problems.add (dir.getFileName() + ": " + err);
        }
        std::sort (found.begin(), found.end(), [] (auto& a, auto& b) { return a.name.compareNatural (b.name) < 0; });
        std::lock_guard<std::mutex> g (lock);
        voices = std::move (found);
        return problems;
    }

    std::vector<VoiceModelInfo> VoiceLibrary::list() const
    {
        std::lock_guard<std::mutex> g (lock);
        return voices;
    }

    bool VoiceLibrary::get (const juce::String& id, VoiceModelInfo& out) const
    {
        std::lock_guard<std::mutex> g (lock);
        for (auto& v : voices)
            if (v.id == id) { out = v; return true; }
        return false;
    }

    VoiceLibrary& sharedVoiceLibrary()
    {
        static VoiceLibrary lib;
        static std::once_flag once;
        std::call_once (once, [] { lib.scan(); });
        return lib;
    }
}
