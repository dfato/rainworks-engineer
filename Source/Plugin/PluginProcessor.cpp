#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "Engine/EngineConfig.h"
#include "Engine/SidechainAnalysis.h"
#include "Engine/TakeImporter.h"
#include "Engine/NotePlan.h"
#include "Engine/PitchTracker.h"
#include "Engine/PitchCorrectRenderer.h"

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
        [r = (d.plan.rendererId == ve::RenderPlan::capturedId ? renderer : pitchRenderer), d, rate, result] (ve::JobContext& ctx)
        {
            return r->render (d, d.plan, rate, ctx, *result);
        },
        [this, id = d.id, hash = d.planHash, result] (const ve::JobResult& res)
        {
            loadingRegions.erase (id);
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

    struct Output { ve::PitchTrack track; std::vector<ve::Note> notes; };
    auto result = std::make_shared<Output>();
    const auto key = session.getEffectiveKey();
    const auto pitchRel = ve::CaptureEngine::regionsSubfolder() + "/" + d.id + "_pitch.bin";
    const auto pitchFile = session.getDataFolder().getChildFile (pitchRel);

    jobs->submit ("Track pitch", "pitch:" + d.id, ve::JobPriority::normal,
        [d, key, pitchFile, result] (ve::JobContext& ctx)
        {
            juce::AudioBuffer<float> audio;
            double rate = 0.0;
            if (auto r = ve::loadAudioFile (d.vocalFile, 0.0, nullptr, audio, rate); r.failed())
                return r;
            if (auto r = ve::PitchTracker::track (audio, rate, {}, &ctx, result->track); r.failed())
                return r;
            result->notes = ve::NoteSegmenter::segment (result->track, key);
            const auto mb = result->track.serialise();
            if (! pitchFile.replaceWithData (mb.getData(), mb.getSize()))
                return juce::Result::fail ("Could not write " + pitchFile.getFullPathName());
            return juce::Result::ok();
        },
        [this, id = d.id, pitchRel, result] (const ve::JobResult& res)
        {
            if (res.success)
            {
                const float amount = getGlobalCorrection();
                for (auto& n : result->notes) n.correction = amount;
                session.setNotes (id, result->notes, pitchRel);
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
            if (! n.userEdited) n.correction = amount;
        session.setNotes (d.id, plan.notes);
        pendingRerender.insert (d.id);
    }
    lastEditMs = juce::Time::getMillisecondCounter();
    sendChangeMessage();
}

void VocalEngineProcessor::setStyle (const juce::String& id)
{
    const auto style = ve::sharedStyleLibrary().get (id);
    session.setStyleId (style.id);
    setGlobalCorrection (style.correction); // marks every take for re-render
    postMessage ("Style: " + style.name + (style.description.isNotEmpty() ? " - " + style.description : juce::String()));
}

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

void VocalEngineProcessor::resnapNotes()
{
    const auto key = session.getEffectiveKey();
    for (auto& d : session.getRegionDescriptors())
    {
        if (! session.hasNotes (d.id)) continue;
        auto plan = session.getNotePlan (d.id);
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
