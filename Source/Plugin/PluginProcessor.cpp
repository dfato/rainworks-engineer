#include "PluginProcessor.h"
#include <juce_audio_formats/juce_audio_formats.h>
#include "PluginEditor.h"
#include "Engine/EngineConfig.h"
#include "Engine/SidechainAnalysis.h"
#include "Engine/TakeImporter.h"
#include "Engine/NotePlan.h"
#include "Engine/PitchTracker.h"
#include "Engine/Style/StyleLearner.h"
#include "Engine/PitchCorrectRenderer.h"
#include "Engine/Intent/NoteIntent.h"
#include "Engine/ML/InferenceBackend.h"
#include "Engine/Voice/VoiceRenderer.h"

VocalEngineProcessor::BusesProperties VocalEngineProcessor::makeBusesProperties()
{
    return BusesProperties()
        .withInput  ("Input",     juce::AudioChannelSet::stereo(), true)
        .withOutput ("Output",    juce::AudioChannelSet::stereo(), true)
        .withInput  ("Sidechain", juce::AudioChannelSet::stereo(), true);
}

VocalEngineProcessor::VocalEngineProcessor()
    : juce::AudioProcessor (makeBusesProperties()),
      renderer (std::make_shared<ve::CapturedAudioRenderer>()),
      pitchRenderer (std::make_shared<ve::PitchCorrectRenderer>()),
      voiceRenderer (std::make_shared<ve::VoiceRenderer>()),
      jobs (std::make_unique<ve::JobQueue> (2))
{
    capture.setDataFolder (session.getDataFolder());
    session.getTree().addListener (this);
    startTimerHz (20);
}

VocalEngineProcessor::~VocalEngineProcessor()
{
    alive->store (false);
    stopTimer();
    session.getTree().removeListener (this);
    jobs.reset();       // cancel + join workers first
    capture.release();  // flush the take in progress, if any
}

//==============================================================================
void VocalEngineProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    // Playback comes from cache and capture is offline, so nothing adds delay.
    setLatencySamples (0);
    sidechainPeak.store (0.0f, std::memory_order_relaxed);

    const auto previous = engineSampleRate.exchange (sampleRate);
    if (previous > 0.0 && std::abs (previous - sampleRate) > 0.5)
        sampleRateChanged = true; // cached renders are at the old rate

    capture.prepare (sampleRate, samplesPerBlock);
}

void VocalEngineProcessor::releaseResources()
{
    capture.release();
}

bool VocalEngineProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mainIn  = layouts.getMainInputChannelSet();
    const auto mainOut = layouts.getMainOutputChannelSet();

    // Main: mono or stereo vocal, output must match input.
    if (mainIn != juce::AudioChannelSet::mono() && mainIn != juce::AudioChannelSet::stereo())
        return false;
    if (mainOut != mainIn)
        return false;

    // Sidechain: stereo, or disabled so the plugin works without it.
    if (layouts.inputBuses.size() > ve::Bus::sidechain)
    {
        const auto sc = layouts.getChannelSet (true, ve::Bus::sidechain);
        if (! sc.isDisabled() && sc != juce::AudioChannelSet::stereo())
            return false;
    }

    return true;
}

void VocalEngineProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const int numSamples = buffer.getNumSamples();
    const auto host = ve::HostContext::read (getPlayHead(), getSampleRate(), numSamples, ++blockCounter);
    hostSnapshots.write (host);
    if (host.hasBpm) lastHostBpm.store (host.bpm, std::memory_order_relaxed);
    if (host.hasTimeSig)
    {
        lastHostNum.store (host.timeSigNumerator, std::memory_order_relaxed);
        lastHostDen.store (host.timeSigDenominator, std::memory_order_relaxed);
    }

    const auto mainIn = getBusBuffer (buffer, true, ve::Bus::mainInput);
    mainInputChannels.store (mainIn.getNumChannels(), std::memory_order_relaxed);

    // Sidechain (optional).
    auto* scBus = getBus (true, ve::Bus::sidechain);
    const bool scOn = scBus != nullptr && scBus->isEnabled() && scBus->getNumberOfChannels() > 0;
    sidechainEnabled.store (scOn, std::memory_order_relaxed);

    juce::AudioBuffer<float> scBuffer;
    float peak = 0.0f;
    if (scOn)
    {
        scBuffer = getBusBuffer (buffer, true, ve::Bus::sidechain);
        peak = scBuffer.getMagnitude (0, numSamples);
    }
    // Decay so the UI (polling at ~30 Hz) does not miss short signals.
    const float held = sidechainPeak.load (std::memory_order_relaxed) * 0.9f;
    sidechainPeak.store (juce::jmax (peak, held), std::memory_order_relaxed);

    const auto mode = (ve::MonitoringMode) monitoringMode.load (std::memory_order_relaxed);

    // Capture reads the input before playback may overwrite the shared channels.
    capture.process (host, mainIn, scOn ? &scBuffer : nullptr,
                     armed.load (std::memory_order_relaxed) && mode != ve::MonitoringMode::bypass,
                     isNonRealtime()); // e.g. FL export: wait for the disk instead of dropping

    // Main output shares channels with main input, so passthrough is implicit.
    auto mainOut = getBusBuffer (buffer, false, ve::Bus::mainOutput);
    for (int ch = mainIn.getNumChannels(); ch < mainOut.getNumChannels(); ++ch)
        mainOut.clear (ch, 0, numSamples);

    // While a take is being recorded you always hear the input.
    if (mode == ve::MonitoringMode::rendered && ! capture.isCapturing())
        playback.render (mainOut, host);
}

VocalEngineProcessor::Status VocalEngineProcessor::getStatus() const noexcept
{
    Status s;
    s.sidechainEnabled  = sidechainEnabled.load (std::memory_order_relaxed);
    s.sidechainPeak     = sidechainPeak.load (std::memory_order_relaxed);
    s.mainInputChannels = mainInputChannels.load (std::memory_order_relaxed);
    return s;
}

//==============================================================================
void VocalEngineProcessor::timerCallback()
{
    playback.collectGarbage();

    // Debounced re-render after note edits.
    if (! pendingRerender.empty() && juce::Time::getMillisecondCounter() - lastEditMs > 250)
    {
        for (auto& id : pendingRerender)
            if (auto r = session.findRegion (id); r.isValid())
                scheduleRender (session.getRegionDescriptor (r));
        pendingRerender.clear();
        rebuildPlayback(); // picks up cached renders right away (e.g. undoing a correction)
    }

    if (sampleRateChanged.exchange (false))
    {
        cache.clear();
        reloadAllRegions();
    }

    bool changed = false;
    for (auto& r : capture.popFinishedRegions())
    {
        if (! r.ok)
        {
            postMessage ("Capture failed: " + r.error);
            continue;
        }

        session.addRegion (r);
        if (r.droppedSamples > 0)
            postMessage ("Take captured with " + juce::String (r.droppedSamples) + " samples of dropout (disk too slow?)");

        const auto d = session.getRegionDescriptor (session.findRegion (r.id));
        scheduleRender (d);
        scheduleAnalysis (d);
        schedulePitchTracking (d);
        changed = true;
    }

    if (stateDirty)
        refreshCachedState();

    if (changed)
        sendChangeMessage();
}

void VocalEngineProcessor::postMessage (const juce::String& m)
{
    lastMessage = juce::Time::getCurrentTime().toString (false, true, false) + "  " + m;
    sendChangeMessage();
}

void VocalEngineProcessor::rebuildPlayback()
{
    auto snap = std::make_unique<ve::PlaybackSnapshot>();
    const double rate = engineSampleRate.load();
    snap->sampleRate = rate;
    for (auto& d : session.getRegionDescriptors())
        if (! d.missingFiles)
        {
            // While a new render is being built, keep playing the previous one.
            auto audio = cache.find (d.id, d.planHash);
            if (audio == nullptr) audio = cache.findAny (d.id);
            snap->add (d, audio, rate > 0.0 ? rate : d.captureSampleRate);
        }
    playback.publish (std::move (snap));
}

void VocalEngineProcessor::scheduleRender (const ve::RegionDescriptor& d)
{
    if (d.missingFiles || cache.find (d.id, d.planHash) != nullptr)
        return;

    const double rate = engineSampleRate.load() > 0.0 ? engineSampleRate.load() : d.captureSampleRate;
    auto result = std::make_shared<ve::RenderedAudio>();
    loadingRegions.insert (d.id);

    jobs->submit ("Load take", "render:" + d.id, ve::JobPriority::high,
        [r = (d.plan.rendererId == ve::RenderPlan::capturedId ? renderer
              : d.plan.rendererId == ve::RenderPlan::voiceConvertId ? std::static_pointer_cast<ve::RegionRenderer> (voiceRenderer) : pitchRenderer),
         d, rate, result] (ve::JobContext& ctx)
        {
            return r->render (d, d.plan, rate, ctx, *result);
        },
        [this, id = d.id, hash = d.planHash, result, isVoice = d.plan.rendererId == ve::RenderPlan::voiceConvertId] (const ve::JobResult& res)
        {
            loadingRegions.erase (id);
            if (res.success && isVoice)
                postMessage ("Voice render done on " + voiceRenderer->getLastDevice());
            if (res.success)
            {
                cache.removeStale (id, hash);
                cache.store (result);
                rebuildPlayback();
            }
            else if (! res.cancelled)
            {
                postMessage ("Could not load take: " + res.error);
            }
            sendChangeMessage();
        });
}

void VocalEngineProcessor::scheduleAnalysis (const ve::RegionDescriptor& d)
{
    if (d.sidechainFile == juce::File() || ! d.sidechainFile.existsAsFile())
        return;

    auto result = std::make_shared<ve::SidechainAnalysisResult>();
    jobs->submit ("Analyse sidechain", "analysis:" + d.id, ve::JobPriority::normal,
        [d, result] (ve::JobContext& ctx)
        {
            juce::AudioBuffer<float> audio;
            double rate = 0.0;
            if (auto r = ve::loadAudioFile (d.sidechainFile, 0.0, nullptr, audio, rate); r.failed())
                return r;
            auto map = d.tempoMap;
            map.setSampleRate (rate);
            return ve::Analysis::analyseSidechain (audio, rate, map, &ctx, *result);
        },
        [this, id = d.id, result] (const ve::JobResult& res)
        {
            if (res.success)
            {
                session.setAnalysis (id, *result);
                resnapNotes(); // key estimate may have changed
            }
            else if (! res.cancelled)
                postMessage ("Sidechain analysis failed: " + res.error);
            sendChangeMessage();
        });
}

void VocalEngineProcessor::schedulePitchTracking (const ve::RegionDescriptor& d)
{
    if (d.missingFiles)
        return;

    struct Output { ve::PitchTrack track; std::vector<ve::Note> notes; juce::String engine; };
    auto result = std::make_shared<Output>();
    const auto key = snappingKey();
    const auto pitchRel = ve::CaptureEngine::regionsSubfolder() + "/" + d.id + "_pitch.bin";
    const auto pitchFile = session.getDataFolder().getChildFile (pitchRel);

    const bool useIntent = session.getUseIntent();
    jobs->submit ("Track pitch", "pitch:" + d.id, ve::JobPriority::normal,
        [d, key, pitchFile, result, useIntent] (ve::JobContext& ctx)
        {
            juce::AudioBuffer<float> audio;
            double rate = 0.0;
            if (auto r = ve::loadAudioFile (d.vocalFile, 0.0, nullptr, audio, rate); r.failed())
                return r;
            // Neural pitch + technique detection when the model is installed, YIN otherwise.
            if (auto r = ve::PitchTracker::trackBest (audio, rate, {}, &ctx, result->track,
                                                      ve::PitchTracker::Engine::automatic, {}, &result->engine); r.failed())
                return r;
            result->notes = ve::NoteSegmenter::segment (result->track, key);
            if (useIntent)
                ve::NoteIntent::apply (result->track, result->notes, key);
            const auto mb = result->track.serialise();
            if (! pitchFile.replaceWithData (mb.getData(), mb.getSize()))
                return juce::Result::fail ("Could not write " + pitchFile.getFullPathName());
            return juce::Result::ok();
        },
        [this, id = d.id, pitchRel, result, useIntent] (const ve::JobResult& res)
        {
            if (res.success)
            {
                const float amount = getGlobalCorrection();
                for (auto& n : result->notes) n.correction = ve::NoteIntent::correctionFor (n, amount, useIntent);
                session.setNotes (id, result->notes, pitchRel);
                if (result->engine != lastPitchEngine)
                {
                    lastPitchEngine = result->engine;
                    postMessage ("Pitch analysis: " + result->engine);
                }
                if (useIntent)
                    postMessage (intentSummary (result->notes));
                if (amount > 0.0f)
                    pendingRerender.insert (id);
            }
            else if (! res.cancelled)
                postMessage ("Pitch tracking failed: " + res.error);
            sendChangeMessage();
        });
}

void VocalEngineProcessor::setGlobalCorrection (float amount)
{
    amount = juce::jlimit (0.0f, 1.0f, amount);
    session.getTree().setProperty ("globalCorrection", amount, nullptr);
    for (auto& d : session.getRegionDescriptors())
    {
        if (! session.hasNotes (d.id)) continue;
        auto plan = session.getNotePlan (d.id);
        for (auto& n : plan.notes)
            if (! n.userEdited) n.correction = ve::NoteIntent::correctionFor (n, amount, session.getUseIntent());
        session.setNotes (d.id, plan.notes);
        pendingRerender.insert (d.id);
    }
    lastEditMs = juce::Time::getMillisecondCounter();
    sendChangeMessage();
}

void VocalEngineProcessor::setVoice (const juce::String& id)
{
    session.setVoiceId (id);
    ve::VoiceModelInfo m;
    if (id.isEmpty())
        postMessage ("Voice: as recorded");
    else if (! ve::ml::isOnnxAvailable())
        postMessage ("Voice rendering is unavailable: " + ve::ml::onnxUnavailableReason());
    else if (! ve::sharedVoiceLibrary().get (id, m))
        postMessage ("Voice \"" + id + "\" is not installed");
    else
        postMessage ("Voice: " + m.name + " (rendering every take, this can take a minute)");
    for (auto& d : session.getRegionDescriptors())
        pendingRerender.insert (d.id);
    lastEditMs = juce::Time::getMillisecondCounter();
    sendChangeMessage();
}

void VocalEngineProcessor::setVoiceGpu (bool on)
{
    session.setVoiceGpu (on);
    voiceRenderer->setUseGpu (on);
    postMessage (on ? (ve::ml::isGpuSupported() ? "Voice models will render on the GPU (DirectML)"
                                                : "This build can't use the GPU; voice models render on the CPU")
                    : "Voice models will render on the CPU");
}

void VocalEngineProcessor::rescanVoices()
{
    const auto problems = ve::sharedVoiceLibrary().scan();
    const auto count = ve::sharedVoiceLibrary().list().size();
    postMessage (juce::String ((int) count) + " voice model(s) found"
                 + (problems.isEmpty() ? juce::String() : ". Skipped: " + problems.joinIntoString ("; ")));
    setVoice (session.getVoiceId());
}

void VocalEngineProcessor::setStyle (const juce::String& id)
{
    const auto style = ve::sharedStyleLibrary().get (id);
    const auto oldScale = ve::sharedStyleLibrary().get (session.getStyleId()).scale;
    session.setStyleId (style.id);
    if (style.scale != oldScale)
        resnapNotes();                      // the style's scale moves note targets
    setGlobalCorrection (style.correction); // marks every take for re-render
    postMessage ("Style: " + style.name + (style.description.isNotEmpty() ? " - " + style.description : juce::String()));
}

void VocalEngineProcessor::learnStyle (const juce::Array<juce::File>& refs, const juce::String& requestedName)
{
    if (refs.isEmpty())
        return;

    const auto base = ve::sharedStyleLibrary().get (session.getStyleId());
    const auto name = requestedName.trim().isNotEmpty() ? requestedName.trim() : base.name + " (learned)";
    auto stats = std::make_shared<std::vector<ve::StyleLearner::TakeStats>>();
    postMessage ("Learning a style from " + juce::String (refs.size()) + " vocal" + (refs.size() == 1 ? "" : "s") + "...");

    jobs->submit ("Learn style", {}, ve::JobPriority::normal,
        [refs, stats] (ve::JobContext& ctx)
        {
            juce::AudioFormatManager formats;
            formats.registerBasicFormats();
            for (auto& f : refs)
            {
                if (ctx.shouldCancel()) return juce::Result::fail ("cancelled");
                std::unique_ptr<juce::AudioFormatReader> r (formats.createReaderFor (f));
                if (r == nullptr) continue;
                const int len = (int) juce::jmin<juce::int64> (r->lengthInSamples, (juce::int64) (r->sampleRate * 60 * 15));
                juce::AudioBuffer<float> audio ((int) r->numChannels, len);
                r->read (&audio, 0, len, 0, true, true);
                // Tempo of a reference file is unknown, so timing (swing/quantize) is not learned here.
                auto st = ve::StyleLearner::analyseAudio (audio, r->sampleRate, 0.0, &ctx);
                if (st.notes > 0) stats->push_back (st);
            }
            return stats->empty() ? juce::Result::fail ("no singing found in those files") : juce::Result::ok();
        },
        [this, stats, base, name] (const ve::JobResult& res)
        {
            if (! res.success)
            {
                if (! res.cancelled) postMessage ("Learn style failed: " + res.error);
                return;
            }
            const auto combined = ve::StyleLearner::combine (*stats);
            auto learned = ve::StyleLearner::learn (base, combined, (int) stats->size());
            juce::String slug;
            for (auto c : name.toLowerCase()) slug << (juce::CharacterFunctions::isLetterOrDigit (c) ? juce::String::charToString (c) : juce::String ("-"));
            learned.id = "user-" + slug;
            learned.name = name;
            learned.category = "My styles";
            learned.description = "Learned from " + juce::String ((int) stats->size()) + " reference vocal"
                                  + (stats->size() == 1 ? "" : "s") + ", starting from " + base.name + ".";
            const auto json = ve::StyleLearner::toJson (learned, base.id, combined, (int) stats->size());

            auto folder = ve::StyleLibrary::userStylesFolder();
            folder.createDirectory();
            folder.getChildFile (learned.id + ".json").replaceWithText (json);
            if (auto err = ve::sharedStyleLibrary().addJson (json); err.isNotEmpty())
            {
                postMessage ("Learn style failed: " + err);
                return;
            }
            ++styleListVersion;
            setStyle (learned.id);
            postMessage ("Learned style \"" + name + "\" from " + juce::String ((int) stats->size()) + " vocal"
                         + (stats->size() == 1 ? "" : "s") + ". Saved to Documents/RainWorks/Engineer/Styles.");
        });
}

void VocalEngineProcessor::setStyleAmount (float amount)
{
    session.setStyleAmount (amount);
    for (auto& d : session.getRegionDescriptors())
        if (session.hasNotes (d.id))
            pendingRerender.insert (d.id);
    lastEditMs = juce::Time::getMillisecondCounter();
    sendChangeMessage();
}

float VocalEngineProcessor::getStyleAmount() const { return session.getStyleAmount(); }

float VocalEngineProcessor::getGlobalCorrection() const
{
    return (float) session.getTree().getProperty ("globalCorrection", 0.0f);
}

void VocalEngineProcessor::updateNote (const juce::String& id, int index, const ve::Note& note)
{
    auto plan = session.getNotePlan (id);
    if (index < 0 || index >= (int) plan.notes.size()) return;
    plan.notes[(size_t) index] = note;
    plan.notes[(size_t) index].userEdited = true;
    session.setNotes (id, plan.notes);
    pendingRerender.insert (id);
    lastEditMs = juce::Time::getMillisecondCounter();
    sendChangeMessage();
}

namespace
{
    bool loadPitchTrack (const ve::RegionDescriptor& d, ve::PitchTrack& t)
    {
        juce::MemoryBlock mb;
        return d.pitchFile.existsAsFile() && d.pitchFile.loadFileAsData (mb) && ve::PitchTrack::deserialise (mb, t);
    }
}

void VocalEngineProcessor::splitNote (const juce::String& id, int index, std::int64_t at)
{
    auto plan = session.getNotePlan (id);
    const auto d = session.getRegionDescriptor (session.findRegion (id));
    ve::PitchTrack track;
    const bool haveTrack = loadPitchTrack (d, track);
    const auto minLen = (std::int64_t) (0.03 * juce::jmax (1.0, d.captureSampleRate));
    if (! ve::NoteEdit::split (plan.notes, index, at, haveTrack ? &track : nullptr, minLen))
    {
        postMessage ("Can't split there: both halves need to be at least 30 ms long.");
        return;
    }
    session.setNotes (id, plan.notes);
    pendingRerender.insert (id);
    lastEditMs = juce::Time::getMillisecondCounter();
    sendChangeMessage();
}

void VocalEngineProcessor::mergeNoteWithNext (const juce::String& id, int index)
{
    auto plan = session.getNotePlan (id);
    const auto d = session.getRegionDescriptor (session.findRegion (id));
    ve::PitchTrack track;
    const bool haveTrack = loadPitchTrack (d, track);
    if (! ve::NoteEdit::mergeWithNext (plan.notes, index, haveTrack ? &track : nullptr))
        return;
    session.setNotes (id, plan.notes);
    pendingRerender.insert (id);
    lastEditMs = juce::Time::getMillisecondCounter();
    sendChangeMessage();
}

void VocalEngineProcessor::reloadAllRegions()
{
    rebuildPlayback();
    for (auto& d : session.getRegionDescriptors())
    {
        scheduleRender (d);
        if (! session.hasAnalysis (d.id))
            scheduleAnalysis (d);
        if (! session.hasNotes (d.id))
            schedulePitchTracking (d);
    }
    sendChangeMessage();
}

//==============================================================================
void VocalEngineProcessor::setMonitoringMode (ve::MonitoringMode m)
{
    monitoringMode.store ((int) m);
    session.setMonitoringMode (m);
    sendChangeMessage();
}

void VocalEngineProcessor::setKeyOverride (int k)
{
    session.setKeyOverride (k);
    resnapNotes();
    sendChangeMessage();
}

ve::KeyEstimate VocalEngineProcessor::snappingKey() const
{
    auto key = session.getEffectiveKey();
    key.scale = ve::sharedStyleLibrary().get (session.getStyleId()).scale;
    return key;
}

void VocalEngineProcessor::resnapNotes()
{
    const auto key = snappingKey();
    for (auto& d : session.getRegionDescriptors())
    {
        if (! session.hasNotes (d.id)) continue;
        auto plan = session.getNotePlan (d.id);
        ve::PitchTrack track;
        if (session.getUseIntent() && loadPitchTrack (d, track))
            ve::NoteIntent::apply (track, plan.notes, key);   // accidentals depend on the key
        else
            ve::NoteSegmenter::resnap (plan.notes, key);
        session.setNotes (d.id, plan.notes);
        pendingRerender.insert (d.id);
    }
}

void VocalEngineProcessor::setDataFolder (const juce::File& f)
{
    session.setDataFolder (f);
    capture.setDataFolder (f);
    cache.clear();
    reloadAllRegions();

    int missing = 0;
    for (auto& d : session.getRegionDescriptors())
        missing += d.missingFiles ? 1 : 0;
    postMessage ("Data folder set to " + f.getFullPathName()
                 + (missing > 0 ? " (" + juce::String (missing) + " takes still missing)" : juce::String()));
}

void VocalEngineProcessor::importTake (const juce::File& vocal, const juce::File& beat)
{
    if (! vocal.existsAsFile())
        return;

    ve::TempoPoint tempo;
    tempo.bpm = lastHostBpm.load();
    tempo.numerator = lastHostNum.load();
    tempo.denominator = lastHostDen.load();

    const double rate = engineSampleRate.load() > 0.0 ? engineSampleRate.load() : 48000.0;
    const auto folder = session.getDataFolder();
    auto result = std::make_shared<ve::CapturedRegion>();

    postMessage ("Importing " + vocal.getFileName() + (beat.existsAsFile() ? " + " + beat.getFileName() : juce::String()));

    jobs->submit ("Import song", {}, ve::JobPriority::high,
        [vocal, beat, folder, rate, tempo, result] (ve::JobContext& ctx)
        {
            return ve::TakeImporter::importTake (vocal, beat, folder, rate, tempo, &ctx, *result);
        },
        [this, result, name = vocal.getFileName()] (const ve::JobResult& res)
        {
            if (! res.success)
            {
                if (! res.cancelled)
                    postMessage ("Import failed: " + res.error);
                return;
            }
            session.addRegion (*result);
            const auto d = session.getRegionDescriptor (session.findRegion (result->id));
            scheduleRender (d);
            scheduleAnalysis (d);
            schedulePitchTracking (d);
            postMessage ("Imported " + name + " as a take from bar 1 ("
                         + juce::String ((double) result->lengthSamples / result->sampleRate, 1) + " s)");
        });
}

void VocalEngineProcessor::analyseAll()
{
    for (auto& d : session.getRegionDescriptors())
        scheduleAnalysis (d);
    sendChangeMessage();
}

void VocalEngineProcessor::analyseRegion (const juce::String& id)
{
    auto r = session.findRegion (id);
    if (r.isValid())
        scheduleAnalysis (session.getRegionDescriptor (r));
}

void VocalEngineProcessor::deleteRegion (const juce::String& id)
{
    jobs->cancelGroup ("render:" + id);
    jobs->cancelGroup ("analysis:" + id);
    jobs->cancelGroup ("pitch:" + id);
    if (session.removeRegion (id))
    {
        cache.removeRegion (id);
        loadingRegions.erase (id);
        rebuildPlayback();
        postMessage ("Take removed from session (its audio files are kept in the data folder)");
    }
}

//==============================================================================
void VocalEngineProcessor::refreshCachedState()
{
    stateDirty = false;
    juce::MemoryBlock mb;
    if (auto xml = session.toXml())
        copyXmlToBinary (*xml, mb);
    const juce::ScopedLock sl (cachedStateLock);
    cachedState = std::move (mb);
}

void VocalEngineProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (juce::MessageManager::existsAndIsCurrentThread())
        refreshCachedState();

    const juce::ScopedLock sl (cachedStateLock);
    destData = cachedState;
}

void VocalEngineProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    std::shared_ptr<juce::XmlElement> xml (getXmlFromBinary (data, sizeInBytes));
    if (xml == nullptr)
        return;

    if (juce::MessageManager::existsAndIsCurrentThread())
    {
        applyState (*xml);
        return;
    }

    juce::MessageManager::callAsync ([this, a = alive, xml]
    {
        if (a->load())
            applyState (*xml);
    });
}

void VocalEngineProcessor::applyState (const juce::XmlElement& xml)
{
    if (auto r = session.restoreFromXml (xml); r.failed())
    {
        postMessage ("Could not restore session: " + r.getErrorMessage());
        return;
    }

    jobs->cancelAll();
    loadingRegions.clear();
    monitoringMode.store ((int) session.getMonitoringMode());
    capture.setDataFolder (session.getDataFolder());
    voiceRenderer->setUseGpu (session.getVoiceGpu());
    cache.clear();
    reloadAllRegions();

    int missing = 0;
    for (auto& d : session.getRegionDescriptors())
        missing += d.missingFiles ? 1 : 0;
    if (missing > 0)
        postMessage (juce::String (missing) + " take(s) have missing audio files. Use Data folder > Locate... to point at the folder.");

    refreshCachedState();
    sendChangeMessage();
}

juce::AudioProcessorEditor* VocalEngineProcessor::createEditor()
{
    return new VocalEngineEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new VocalEngineProcessor();
}

juce::String VocalEngineProcessor::intentSummary (const std::vector<ve::Note>& notes)
{
    constexpr int kinds = 10;
    int counts[kinds] = {};
    for (auto& n : notes)
        for (int b = 0; b < kinds; ++b)
            if ((n.intent & (1 << b)) && (b != 5 || n.intentScale < 1.0f)) ++counts[b];   // spoken: only when it changed the pull
    static const char* names[] = { "slides", "vibrato", "scoops", "passing notes", "chosen accidentals", "spoken", "ad-libs",
                                   "breathy", "gritty", "belted" };
    juce::StringArray parts;
    for (int b = 0; b < kinds; ++b)
        if (counts[b] > 0) parts.add (juce::String (counts[b]) + " " + names[b]);
    juce::String msg = juce::String ((int) notes.size()) + " notes";
    if (! parts.isEmpty()) msg << ". Kept as sung or corrected gently: " << parts.joinIntoString (", ");
    if (! notes.empty() && counts[5] * 2 > (int) notes.size())
        msg << ". This take sounds mostly spoken or rapped, so pitch is left alone.";
    return msg;
}

void VocalEngineProcessor::setUseIntent (bool on)
{
    session.setUseIntent (on);
    const auto key = snappingKey();
    const float amount = getGlobalCorrection();
    for (auto& d : session.getRegionDescriptors())
    {
        if (! session.hasNotes (d.id)) continue;
        auto plan = session.getNotePlan (d.id);
        ve::PitchTrack track;
        if (on && loadPitchTrack (d, track))
            ve::NoteIntent::apply (track, plan.notes, key);
        else
            ve::NoteIntent::clear (plan.notes, key);
        for (auto& n : plan.notes)
            if (! n.userEdited) n.correction = ve::NoteIntent::correctionFor (n, amount, on);
        session.setNotes (d.id, plan.notes);
        pendingRerender.insert (d.id);
    }
    postMessage (on ? "Intent on: slides, vibrato, accidentals and spoken parts are kept as sung"
                    : "Intent off: every note is corrected the same way");
    lastEditMs = juce::Time::getMillisecondCounter();
    sendChangeMessage();
}
