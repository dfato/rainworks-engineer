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
//   --base-json <file> start from this style JSON instead of a library style (needs "id")
//   --pool <stats>     contrast mode: learn how the inputs differ from this pooled .stats.json
//   --gain <g>         contrast mode: how much to exaggerate the difference (default 2)
//   --stats-dir <dir>  also write <input>.stats.json per audio input
//   --out <file>       write the profile JSON here (default: stdout)
//   --stats-only       print the combined statistics and stop
//   --render <style> <out.wav>
//                      instead of learning: correct the first input with <style> (pitch and
//                      expression, ornaments and tone, no timing moves) and write the result,
//                      to audition a style
//   --amount <0..2>    with --render: style amount (1 = as designed)
//   --correct <0..1>   with --render: the Correct amount (default: the style's)
//   --no-intent        with --render: correct every note alike (the plugin's Intent toggle off)
//   --key <key>        with --render: the song's key (e.g. Gm, F#), instead of guessing it from the vocal
//   --engine <e>       pitch tracker: auto (neural when its model is installed, default), dsp or neural
//   --referee <e>      with --render: tracker that measures the result (default: dsp, so trackers compare fairly)
//   --voicing <0..1>   neural tracker: salience below which a frame is unvoiced
//   --refine <cents>   neural tracker: keep YIN's estimate where it agrees within this many cents (default 50, 0 = off)
//   --rescue <0..1>    neural tracker: keep frames down to this salience voiced where a confident YIN agrees
//   --pitch-eval <csv> instead of learning: compare the tracker on the first input with a reference
//                      f0 file ("time,hz" per line, 0 Hz = unvoiced, as vocadito/MIR datasets ship)
//                      and print accuracy as one JSON line
//   --note-eval <csv>  with --pitch-eval: also score the note segmentation against reference notes
//                      ("onset,hz,duration" per line, as vocadito ships): a note counts when its onset
//                      is within 50 ms and its pitch within 50 cents of an unmatched reference note

#include <juce_audio_formats/juce_audio_formats.h>
#include "Engine/Style/StyleLearner.h"
#include "Engine/Style/StyleProfile.h"
#include "Engine/Style/ExpressionLayer.h"
#include "Engine/Style/ToneShaper.h"
#include "Engine/PitchCorrectRenderer.h"
#include "Engine/SidechainAnalysis.h"
#include "Engine/Intent/NoteIntent.h"
#include "Engine/ML/NeuralPitch.h"
#include "Engine/ML/TechniqueDetector.h"
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

    PitchTracker::Engine parseEngine (const juce::String& e)
    {
        if (e == "dsp") return PitchTracker::Engine::dsp;
        if (e == "neural") return PitchTracker::Engine::neural;
        return PitchTracker::Engine::automatic;
    }

    // Raw pitch accuracy and friends (as mir_eval.melody), against a reference f0 file.
    int pitchEval (const juce::File& refFile, const PitchTrack& track, double sr, const juce::String& label,
                   const juce::File& notesFile)
    {
        juce::StringArray lines;
        lines.addLines (refFile.loadFileAsString());
        int refVoiced = 0, refUnvoiced = 0, hit50 = 0, chroma50 = 0, within25 = 0, voicedHit = 0, falseAlarm = 0, octave = 0;
        std::vector<float> errs;
        for (auto& l : lines)
        {
            const auto parts = juce::StringArray::fromTokens (l, ", \t", "");
            if (parts.size() < 2) continue;
            const double t = parts[0].getDoubleValue();
            const float ref = parts[1].getFloatValue();
            // Our frame nearest to t.
            const int i = (int) std::lround (t * sr / track.hopSize);
            if (i < 0 || i >= (int) track.frames.size()) continue;
            const float est = track.frames[(size_t) i].f0;
            if (ref <= 0.0f)
            {
                ++refUnvoiced;
                if (est > 0.0f) ++falseAlarm;
                continue;
            }
            ++refVoiced;
            if (est <= 0.0f) continue;
            ++voicedHit;
            const float c = 1200.0f * std::log2 (est / ref);
            const float ac = std::abs (c);
            if (ac <= 50.0f) ++hit50;
            if (ac <= 25.0f) ++within25;
            float cm = std::fmod (ac, 1200.0f);
            cm = juce::jmin (cm, 1200.0f - cm);
            if (cm <= 50.0f) ++chroma50;
            if (ac > 50.0f && cm <= 50.0f) ++octave;
            if (ac <= 50.0f) errs.push_back (ac);
        }
        if (refVoiced == 0) return fail ("no voiced reference frames in " + refFile.getFullPathName());
        std::sort (errs.begin(), errs.end());
        const float medErr = errs.empty() ? 0.0f : errs[errs.size() / 2];
        double meanErr = 0.0;
        for (auto e : errs) meanErr += e;
        meanErr = errs.empty() ? 0.0 : meanErr / (double) errs.size();
        juce::DynamicObject::Ptr o = new juce::DynamicObject();
        o->setProperty ("file", label);
        o->setProperty ("engine", track.engine == PitchEngine::neural ? "neural" : "dsp");
        o->setProperty ("voicedFrames", refVoiced);
        o->setProperty ("rawPitchAccuracy", (double) hit50 / refVoiced);          // est within 50 cents, of all voiced reference frames
        o->setProperty ("rawChromaAccuracy", (double) chroma50 / refVoiced);
        o->setProperty ("within25", (double) within25 / refVoiced);
        o->setProperty ("octaveErrors", (double) octave / refVoiced);
        o->setProperty ("voicingRecall", (double) voicedHit / refVoiced);
        o->setProperty ("voicingFalseAlarm", refUnvoiced > 0 ? (double) falseAlarm / refUnvoiced : 0.0);
        o->setProperty ("medianCents", medErr);                                    // over frames within 50 cents
        o->setProperty ("meanCents", meanErr);
        if (notesFile.existsAsFile())
        {
            struct RefNote { double onset; float midi; bool used = false; };
            std::vector<RefNote> refs;
            juce::StringArray nl;
            nl.addLines (notesFile.loadFileAsString());
            for (auto& l : nl)
            {
                const auto parts = juce::StringArray::fromTokens (l, ", \t", "");
                if (parts.size() >= 2 && parts[1].getFloatValue() > 0.0f)
                    refs.push_back ({ parts[0].getDoubleValue(), PitchTrack::hzToMidi (parts[1].getFloatValue()) });
            }
            const auto notes = NoteSegmenter::segment (track, {});
            int matched = 0;
            for (auto& n : notes)
            {
                const double onset = (double) n.start / sr;
                RefNote* best = nullptr;
                for (auto& r : refs)
                    if (! r.used && std::abs (r.onset - onset) <= 0.05 && std::abs (r.midi - n.detectedMidi) <= 0.5f
                        && (best == nullptr || std::abs (r.onset - onset) < std::abs (best->onset - onset)))
                        best = &r;
                if (best != nullptr) { best->used = true; ++matched; }
            }
            const double p = notes.empty() ? 0.0 : (double) matched / (double) notes.size();
            const double r = refs.empty() ? 0.0 : (double) matched / (double) refs.size();
            o->setProperty ("refNotes", (int) refs.size());
            o->setProperty ("notes", (int) notes.size());
            o->setProperty ("notePrecision", p);
            o->setProperty ("noteRecall", r);
            o->setProperty ("noteF", p + r > 0.0 ? 2.0 * p * r / (p + r) : 0.0);
        }
        std::cout << juce::JSON::toString (juce::var (o.get()), true) << std::endl;
        return 0;
    }
}

int main (int argc, char* argv[])
{
    juce::String baseId = "natural", id, name, category, outPath, statsDir;
    float strength = 1.0f, gain = 2.0f;
    juce::String baseJson, poolPath;
    bool statsOnly = false;
    float amount = 1.0f;
    juce::String keyName;
    float correctOverride = -1.0f;
    bool useIntent = true;
    juce::String renderStyle, renderOut;
    juce::StringArray inputs;
    auto engine = PitchTracker::Engine::automatic, referee = PitchTracker::Engine::dsp;
    PitchTracker::NeuralSettings neural;
    juce::String pitchEvalRef, noteEvalRef;

    for (int i = 1; i < argc; ++i)
    {
        const juce::String a (argv[i]);
        auto next = [&] { return i + 1 < argc ? juce::String (argv[++i]) : juce::String(); };
        if      (a == "--base")       baseId = next();
        else if (a == "--id")         id = next();
        else if (a == "--name")       name = next();
        else if (a == "--category")   category = next();
        else if (a == "--strength")   strength = next().getFloatValue();
        else if (a == "--base-json")  baseJson = next();
        else if (a == "--pool")       poolPath = next();
        else if (a == "--gain")       gain = next().getFloatValue();
        else if (a == "--out")        outPath = next();
        else if (a == "--stats-dir")  statsDir = next();
        else if (a == "--stats-only") statsOnly = true;
        else if (a == "--amount")     amount = next().getFloatValue();
        else if (a == "--key")        keyName = next();
        else if (a == "--correct")    correctOverride = next().getFloatValue();
        else if (a == "--no-intent")  useIntent = false;
        else if (a == "--render")     { renderStyle = next(); renderOut = next(); }
        else if (a == "--engine")     engine = parseEngine (next());
        else if (a == "--referee")    referee = parseEngine (next());
        else if (a == "--voicing")    neural.voicingThreshold = next().getFloatValue();
        else if (a == "--refine")     neural.refineCents = next().getFloatValue();
        else if (a == "--rescue")     neural.rescueThreshold = next().getFloatValue();
        else if (a == "--pitch-eval") pitchEvalRef = next();
        else if (a == "--note-eval")  noteEvalRef = next();
        else if (a == "--help" || a == "-h") { std::cout << "See the comment at the top of Source/Tools/StyleLearnMain.cpp" << std::endl; return 0; }
        else inputs.add (a);
    }
    if (inputs.isEmpty()) return fail ("usage: EngineerStyleLearn [--base id] [--id id] [--name n] [--strength s] [--stats-dir d] [--out f] vocal.wav[@bpm] ...");

    juce::AudioFormatManager formats;
    formats.registerBasicFormats();

    if (pitchEvalRef.isNotEmpty())
    {
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (inputs[0]);
        juce::AudioBuffer<float> audio;
        double sr = 0.0;
        if (! readMono (formats, file, audio, sr)) return fail ("could not read audio " + file.getFullPathName());
        PitchTrack track;
        juce::String status;
        if (auto r = PitchTracker::trackBest (audio, sr, {}, nullptr, track, engine, neural, &status); r.failed())
            return fail ("pitch tracking failed: " + r.getErrorMessage());
        std::cerr << status << std::endl;
        return pitchEval (juce::File::getCurrentWorkingDirectory().getChildFile (pitchEvalRef), track, sr, file.getFileName(),
                          noteEvalRef.isNotEmpty() ? juce::File::getCurrentWorkingDirectory().getChildFile (noteEvalRef) : juce::File());
    }

    if (renderStyle.isNotEmpty())
    {
        auto& lib = sharedStyleLibrary();
        if (! lib.contains (renderStyle)) return fail ("unknown style " + renderStyle);
        const auto style = lib.get (renderStyle).withAmount (amount);
        const auto path = inputs[0].containsChar ('@') ? inputs[0].upToLastOccurrenceOf ("@", false, false) : inputs[0];
        const auto file = juce::File::getCurrentWorkingDirectory().getChildFile (path);
        juce::AudioBuffer<float> audio;
        double sr = 0.0;
        if (! readMono (formats, file, audio, sr)) return fail ("could not read audio " + file.getFullPathName());

        PitchTrack track;
        juce::String status;
        if (auto r = PitchTracker::trackBest (audio, sr, {}, nullptr, track, engine, neural, &status); r.failed())
            return fail ("pitch tracking failed: " + r.getErrorMessage());
        std::cerr << status << std::endl;
        auto notes = NoteSegmenter::segment (track, {});
        // Key from the sung notes, weighted by length.
        Chroma chroma {};
        for (auto& n : notes) chroma[(size_t) (((int) std::lround (n.detectedMidi) % 12 + 12) % 12)] += (float) (n.end - n.start);
        auto key = Analysis::estimateKey (chroma);
        if (keyName.isNotEmpty())
        {
            // "G", "F#", "Bb", "Gm", "C#m": as the song's key would come from the beat.
            const int letters[] = { 9, 11, 0, 2, 4, 5, 7 };  // A B C D E F G
            const auto c = juce::CharacterFunctions::toUpperCase (keyName[0]);
            if (c < 'A' || c > 'G') return fail ("bad --key " + keyName);
            int pc = letters[c - 'A'];
            auto rest = keyName.substring (1);
            if (rest.startsWith ("#")) { pc += 1; rest = rest.substring (1); }
            else if (rest.startsWith ("b")) { pc -= 1; rest = rest.substring (1); }
            key.tonic = (pc + 12) % 12;
            key.minor = rest.startsWith ("m");
        }
        key.scale = style.scale;
        NoteSegmenter::resnap (notes, key);
        // As the plugin does: intent per note (a slide or a blue note gets less correction),
        // then the Correct amount (the style's default unless --correct is given).
        if (useIntent) NoteIntent::apply (track, notes, key);
        const float amountCorrect = correctOverride >= 0.0f ? juce::jlimit (0.0f, 1.0f, correctOverride) : style.correction;
        for (auto& n : notes) n.correction = NoteIntent::correctionFor (n, amountCorrect, useIntent);

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
        if (auto at = juce::SystemStats::getEnvironmentVariable ("VE_SHIFT_AT", {}); at.isNotEmpty())
            for (double t = at.getDoubleValue(); t < at.getDoubleValue() + 0.4; t += 0.02)
            {
                const int i = juce::jlimit (0, len - 1, (int) (t * sr));
                std::cerr << "t " << juce::String (t, 2) << " f0 " << f0[(size_t) i] << " shift " << shift[(size_t) i] << std::endl;
            }
        if (juce::SystemStats::getEnvironmentVariable ("VE_SHIFT_STATS", {}).isNotEmpty())
        {
            int jumps = 0, big = 0; float maxStep = 0.0f;
            for (int i = 1; i < len; ++i)
            {
                const float step = std::abs (shift[(size_t) i] - shift[(size_t) i - 1]);
                maxStep = juce::jmax (maxStep, step);
                if (step > 0.02f) ++jumps;
                if (std::abs (shift[(size_t) i]) > 2.5f) ++big;
            }
            for (auto& n : notes)
                std::cerr << juce::String ((double) n.start / sr, 2) << "s " << juce::String ((double) (n.end - n.start) / sr, 2)
                          << "s " << juce::String (n.detectedMidi, 2) << " -> " << n.targetMidi << "\n";
            int longNotes = 0;
            for (auto& n : notes) if ((double) (n.end - n.start) >= 0.6 * sr) ++longNotes;
            std::cerr << longNotes << " notes of 0.6 s or more; ";
            std::cerr << "shift: " << jumps << " steps > 2 cents/sample, max step " << maxStep << " st, " << big << " samples beyond 2.5 st" << std::endl;
        }
        juce::AudioBuffer<float> out;
        PitchCorrectRenderer::psolaShift (audio, out, sr, f0, shift);
        ToneShaper::process (out, sr, style);

        auto outFile = juce::File::getCurrentWorkingDirectory().getChildFile (renderOut);
        outFile.deleteFile();
        std::unique_ptr<juce::OutputStream> os = std::make_unique<juce::FileOutputStream> (outFile);
        juce::WavAudioFormat wav;
        auto w = wav.createWriterFor (os, juce::AudioFormatWriterOptions{}.withSampleRate (sr).withNumChannels (1).withBitsPerSample (16));
        if (w == nullptr) return fail ("cannot write " + renderOut);
        w->writeFromAudioSampleBuffer (out, 0, out.getNumSamples());
        // How in tune the result is: re-track the output and compare each note's middle with its
        // target, before (the take as sung) and after.
        {
            PitchTrack outTrack;
            PitchTracker::NeuralSettings refereeSettings = neural;
            refereeSettings.techniques = false;
            if (PitchTracker::trackBest (out, sr, {}, nullptr, outTrack, referee, refereeSettings).wasOk())
            {
                auto report = [&] (const PitchTrack& tr, const char* label)
                {
                    std::vector<float> errs, wobble;
                    for (auto& n : notes)
                    {
                        if ((double) (n.end - n.start) < 0.15 * sr) continue;   // long enough to hear the pitch
                        const auto mid0 = n.start + (n.end - n.start) / 5, mid1 = n.end - (n.end - n.start) / 5;
                        std::vector<float> v;
                        for (auto& f : tr.frames)
                        {
                            const auto at = (std::int64_t) (&f - tr.frames.data()) * tr.hopSize;
                            if (at >= mid0 && at < mid1 && f.f0 > 0.0f) v.push_back (PitchTrack::hzToMidi (f.f0));
                        }
                        if (v.size() < 3) continue;
                        std::sort (v.begin(), v.end());
                        const float med = v[v.size() / 2];
                        errs.push_back (std::abs (med - n.targetMidi) * 100.0f);
                        if (std::abs (med - n.targetMidi) > 0.25f && juce::SystemStats::getEnvironmentVariable ("VE_OFF_NOTES", {}).isNotEmpty())
                            std::cerr << "  " << label << " off: " << juce::String ((double) n.start / sr, 2) << "s len "
                                      << juce::String ((double) (n.end - n.start) / sr, 2) << " sung " << juce::String (n.detectedMidi, 2)
                                      << " target " << n.targetMidi << " got " << juce::String (med, 2) << " corr "
                                      << juce::String (n.correction, 2) << " intent " << NoteIntent::describe (n.intent) << std::endl;
                        wobble.push_back ((v[v.size() * 9 / 10] - v[v.size() / 10]) * 100.0f);
                    }
                    if (errs.empty()) return;
                    auto median = [] (std::vector<float> x) { std::sort (x.begin(), x.end()); return x[x.size() / 2]; };
                    int within10 = 0, within25 = 0;
                    for (auto e : errs) { within10 += e <= 10.0f; within25 += e <= 25.0f; }
                    std::cerr << label << ": " << errs.size() << " held notes, median " << juce::String (median (errs), 1)
                              << " cents from target, " << (100 * within10 / (int) errs.size()) << "% within 10 cents, "
                              << (100 * within25 / (int) errs.size()) << "% within 25, pitch spread inside notes "
                              << juce::String (median (wobble), 1) << " cents" << std::endl;
                };
                PitchTrack inTrack;
                if (PitchTracker::trackBest (audio, sr, {}, nullptr, inTrack, referee, refereeSettings).failed()) inTrack = track;
                std::cerr << "measured with the " << (outTrack.engine == PitchEngine::neural ? "neural" : "DSP") << " tracker" << std::endl;
                report (inTrack, "before");
                report (outTrack, "after ");
            }
        }
        int moved = 0;
        for (auto& n : notes) if (std::abs (n.targetMidi - std::round (n.detectedMidi)) >= 1.0f) ++moved;
        std::cerr << "rendered " << style.name << ": " << (int) notes.size() << " notes, key " << key.tonic << (key.minor ? "m" : "")
                  << ", scale " << toString (style.scale) << ", " << moved << " moved to another note" << std::endl;
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

        auto st = StyleLearner::analyseAudio (audio, sr, bpm, nullptr, engine);
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
    if (baseJson.isNotEmpty())
    {
        const auto text = juce::File::getCurrentWorkingDirectory().getChildFile (baseJson).loadFileAsString();
        if (auto err = lib.addJson (text); err.isNotEmpty()) return fail ("bad --base-json: " + err);
        baseId = juce::JSON::parse (text).getProperty ("id", baseId).toString();
    }
    if (! lib.contains (baseId)) return fail ("unknown base style " + baseId);
    const auto base = lib.get (baseId);

    StyleProfile learned;
    if (poolPath.isNotEmpty())
    {
        const auto pv = juce::JSON::parse (juce::File::getCurrentWorkingDirectory().getChildFile (poolPath));
        if (! pv.isObject()) return fail ("could not read " + poolPath);
        const auto pool = StyleLearner::TakeStats::fromVar (pv);
        learned = StyleLearner::learnContrast (base, combined, (int) takes.size(), pool, (int) pv.getProperty ("takes", 50), gain);
    }
    else
        learned = StyleLearner::learn (base, combined, (int) takes.size(), strength);
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
