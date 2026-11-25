// EngineerStyleLearn: learn a style profile from reference vocals.
//
//   EngineerStyleLearn [options] <input>...
//
// Each input is a dry, monophonic vocal (wav/aiff/flac/ogg/mp3), optionally with
// "@<bpm>" appended for timing analysis (song.wav@92), or a .stats.json file
// written by an earlier run with --stats-dir.
//
// Options:
//   --base <id>        style to start from (default "natural"); also the "inherits" link
//   --id <id>          id of the learned style (default: "<base>-learned")
//   --name <name>      display name (default: "<Base name> (learned)")
//   --category <cat>   category (default: base's)
//   --strength <0..1>  how far to move from the base toward the measurements (default 1)
//   --stats-dir <dir>  also write <input>.stats.json per audio input
//   --out <file>       write the profile JSON here (default: stdout)
//   --stats-only       print the combined statistics and stop
//   --render <style> <out.wav>
//                      instead of learning: correct the first input with <style> (pitch and
//                      expression, no timing moves) and write the result, to audition a style

#include <juce_audio_formats/juce_audio_formats.h>
#include "Engine/Style/StyleLearner.h"
#include "Engine/Style/StyleProfile.h"
#include "Engine/Style/ExpressionLayer.h"
#include "Engine/PitchCorrectRenderer.h"
#include "Engine/SidechainAnalysis.h"
#include <iostream>

using namespace ve;

namespace
{
    int fail (const juce::String& msg) { std::cerr << msg << std::endl; return 1; }

    bool readMono (juce::AudioFormatManager& fm, const juce::File& f, juce::AudioBuffer<float>& out, double& sr)
    {
        std::unique_ptr<juce::AudioFormatReader> r (fm.createReaderFor (f));
        if (r == nullptr || r->lengthInSamples <= 0) return false;
        const int len = (int) juce::jmin<juce::int64> (r->lengthInSamples, (juce::int64) r->sampleRate * 60 * 20);
        juce::AudioBuffer<float> tmp ((int) r->numChannels, len);
        r->read (&tmp, 0, len, 0, true, true);
        out.setSize (1, len);
        out.clear();
        for (int c = 0; c < tmp.getNumChannels(); ++c)
            out.addFrom (0, 0, tmp, c, 0, len, 1.0f / (float) tmp.getNumChannels());
        sr = r->sampleRate;
        return true;
    }
}

int main (int argc, char* argv[])
{
    juce::String baseId = "natural", id, name, category, outPath, statsDir;
    float strength = 1.0f;
    bool statsOnly = false;
    juce::String renderStyle, renderOut;
    juce::StringArray inputs;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] { return i + 1 < argc ? juce::String (argv[++i]) : juce::String(); };
        if      (a == "--base")       baseId = next();
        else if (a == "--id")         id = next();
        else if (a == "--name")       name = next();
        else if (a == "--category")   category = next();
        else if (a == "--strength")   strength = next().getFloatValue();
        else if (a == "--out")        outPath = next();
        else if (a == "--stats-dir")  statsDir = next();
        else if (a == "--stats-only") statsOnly = true;
        else if (a == "--render")     { renderStyle = next(); renderOut = next(); }
        else if (a == "--help" || a == "-h") { std::cout << "See the comment at the top of Source/Tools/StyleLearnMain.cpp" << std::endl; return 0; }
        else inputs.add (a);
    }
    if (inputs.isEmpty()) return fail ("usage: EngineerStyleLearn [--base id] [--id id] [--name n] [--strength s] [--stats-dir d] [--out f] vocal.wav[@bpm] ...");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    if (renderStyle.isNotEmpty())
    {
        auto& lib = sharedStyleLibrary();
        if (! lib.contains (renderStyle)) return fail ("unknown style " + renderStyle);
        const auto style = lib.get (renderStyle);
        const auto path = inputs[0].containsChar ('@') ? inputs[0].upToLastOccurrenceOf ("@", false, false) : inputs[0];
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (path);
        juce::AudioBuffer<float> audio;
        double sr = 0.0;
        if (! readMono (formats, file, audio, sr)) return fail ("could not read audio " + file.getFullPathName());

        PitchTrack track;
        if (PitchTracker::track (audio, sr, {}, nullptr, track).failed()) return fail ("pitch tracking failed");
        auto notes = NoteSegmenter::segment (track, {});
        // Key from the sung notes, weighted by length.
        Chroma chroma {};
        for (auto& n : notes) chroma[(size_t) (((int) std::lround (n.detectedMidi) % 12 + 12) % 12)] += (float) (n.end - n.start);
        const auto key = Analysis::estimateKey (chroma);
        NoteSegmenter::resnap (notes, key);
        for (auto& n : notes) n.correction = style.correction;

        const int len = audio.getNumSamples();
        std::vector<float> f0 ((size_t) len, 0.0f);
        for (int i = 0; i < len; ++i)
        {
            const double fi = (double) i / track.hopSize;
            const int a = juce::jlimit (0, (int) track.frames.size() - 1, (int) fi);
            const int b = juce::jmin (a + 1, (int) track.frames.size() - 1);
            const float fa = track.frames[(size_t) a].f0, fb = track.frames[(size_t) b].f0;
            f0[(size_t) i] = (fa > 0.0f && fb > 0.0f) ? fa + (fb - fa) * (float) (fi - a) : (fa > 0.0f ? fa : fb);
        }
        NotePlan plan { notes };
        const auto shift = ExpressionLayer::buildShiftCurve (plan, style, f0, sr);
        juce::AudioBuffer<float> out;
        PitchCorrectRenderer::psolaShift (audio, out, sr, f0, shift);

        auto outFile = juce::File::getCurrentWorkingDirectory().getChildFile (renderOut);
        outFile.deleteFile();
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (outFile);
        juce::WavAudioFormat wav;
        auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (sr).withNumChannels (1).withBitsPerSample (16));
        if (w == nullptr) return fail ("cannot write " + renderOut);
        w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
        std::cerr << "rendered " << style.name << " (" << (int) notes.size() << " notes, key " << key.tonic << (key.minor ? "m" : "") << ")" << std::endl;
        return 0;
    }

    std::vector<StyleLearner::TakeStats> takes;
    for (auto& in : inputs)
    {
        if (in.endsWithIgnoreCase (".stats.json"))
        {
            const auto v = juce::JSON::parse (juce::File (juce::File::getCurrentWorkingDirectory().getChildFile (in)));
            if (! v.isObject()) return fail ("could not read " + in);
            takes.push_back (StyleLearner::TakeStats::fromVar (v));
            continue;
        }

        double bpm = 0.0;
        auto path = in;
        if (in.containsChar ('@'))
        {
            bpm = in.fromLastOccurrenceOf ("@", false, false).getDoubleValue();
            path = in.upToLastOccurrenceOf ("@", false, false);
        }
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (path);
        juce::AudioBuffer<float> audio;
        double sr = 0.0;
        if (! readMono (formats, file, audio, sr)) return fail ("could not read audio " + path);

        auto st = StyleLearner::analyseAudio (audio, sr, bpm);
        std::cerr << file.getFileName() << ": " << st.notes << " notes, " << juce::String (st.voicedSeconds, 1) << " s voiced" << std::endl;
        if (statsDir.isNotEmpty())
        {
            auto dir = juce::File::getCurrentWorkingDirectory().getChildFile (statsDir);
            dir.createDirectory();
            dir.getChildFile (file.getFileNameWithoutExtension() + ".stats.json").replaceWithText (juce::JSON::toString (st.toVar()));
        }
        takes.push_back (st);
    }

    const auto combined = StyleLearner::combine (takes);
    if (statsOnly) { std::cout << juce::JSON::toString (combined.toVar()) << std::endl; return 0; }

    auto& lib = sharedStyleLibrary();
    if (! lib.contains (baseId)) return fail ("unknown base style " + baseId);
    const auto base = lib.get (baseId);

    auto learned = StyleLearner::learn (base, combined, (int) takes.size(), strength);
    learned.id = id.isNotEmpty() ? id : baseId + "-learned";
    learned.name = name.isNotEmpty() ? name : base.name + " (learned)";
    if (category.isNotEmpty()) learned.category = category;
    learned.description = "Learned from " + juce::String ((int) takes.size()) + " reference vocal" + (takes.size() == 1 ? "" : "s") + ".";

    const auto json = StyleLearner::toJson (learned, baseId, combined, (int) takes.size());
    if (outPath.isNotEmpty())
        juce::File::getCurrentWorkingDirectory().getChildFile (outPath).replaceWithText (json);
    else
        std::cout << json << std::endl;
    return 0;
}
