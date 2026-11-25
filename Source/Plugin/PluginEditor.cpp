#include "PluginEditor.h"
#include "UI/Theme.h"
#include "Engine/TakeImporter.h"
#include "Engine/Style/StyleProfile.h"

VocalEngineEditor::VocalEngineEditor (VocalEngineProcessor& p)
    : juce::AudioProcessorEditor (p),
      processor (p),
      header (JucePlugin_Name, JucePlugin_VersionString)
{
    addAndMakeVisible (header);
    addAndMakeVisible (controls);
    addAndMakeVisible (hostInfo);
    addAndMakeVisible (timeline);
    addAndMakeVisible (statusBar);

    controls.onArmChanged = [this] (bool a) { processor.setArmed (a); controls.setArmed (a); };
    controls.onModeChanged = [this] (int m) { processor.setMonitoringMode ((ve::MonitoringMode) m); };
    controls.onKeyOverrideChanged = [this] (int k) { processor.setKeyOverride (k); };
    controls.onAnalyse = [this] { processor.analyseAll(); };
    controls.onImport = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose the vocal file (select the beat too if you have it)",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc) { importFiles (fc.getResults()); });
    };
    controls.onOpenDataFolder = [this]
    {
        auto f = processor.getSession().getDataFolder();
        if (! f.isDirectory()) f.createDirectory();
        f.revealToUser();
    };
    controls.onLocateDataFolder = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose the Engineer data folder for this project",
                                                       processor.getSession().getDataFolder());
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectDirectories,
                              [this] (const juce::FileChooser& fc)
                              {
                                  const auto f = fc.getResult();
                                  if (f.isDirectory())
                                      processor.setDataFolder (f);
                              });
    };

    timeline.onDeleteRegion = [this] (const juce::String& id) { processor.deleteRegion (id); };
    timeline.onAnalyseRegion = [this] (const juce::String& id) { processor.analyseRegion (id); };
    timeline.onNoteChanged = [this] (const juce::String& id, int idx, const ve::Note& n) { processor.updateNote (id, idx, n); };
    controls.onCorrectionChanged = [this] (float a) { processor.setGlobalCorrection (a); };
    controls.onStyleChanged = [this] (const juce::String& id) { processor.setStyle (id); };
    {
        std::vector<ve::ui::ControlBar::StyleItem> items;
        for (auto& e : ve::sharedStyleLibrary().list())
            items.push_back ({ e.id, e.name, e.category });
        controls.setStyles (items);
    }

    processor.addChangeListener (this);
    rebuildFromSession();

    setResizable (true, true);
    setResizeLimits (1180, 520, 3000, 1800);
    setSize (1280, 700);
    startTimerHz (30);
}

VocalEngineEditor::~VocalEngineEditor()
{
    stopTimer();
    processor.removeChangeListener (this);
}

void VocalEngineEditor::paint (juce::Graphics& g)
{
    g.fillAll (ve::ui::Theme::background);
}

void VocalEngineEditor::resized()
{
    auto r = getLocalBounds().reduced (8);
    header.setBounds (r.removeFromTop (34));
    r.removeFromTop (4);
    controls.setBounds (r.removeFromTop (36));
    r.removeFromTop (6);
    hostInfo.setBounds (r.removeFromTop (38));
    r.removeFromTop (6);
    statusBar.setBounds (r.removeFromBottom (22));
    r.removeFromBottom (4);
    timeline.setBounds (r);
}

bool VocalEngineEditor::isInterestedInFileDrag (const juce::StringArray& files)
{
    for (auto& f : files)
        if (ve::TakeImporter::isSupportedAudioFile (juce::File (f)))
            return true;
    return false;
}

void VocalEngineEditor::filesDropped (const juce::StringArray& files, int, int)
{
    dragHover = false;
    repaint();
    juce::Array<juce::File> list;
    for (auto& f : files)
        list.add (juce::File (f));
    importFiles (list);
}

void VocalEngineEditor::importFiles (const juce::Array<juce::File>& files)
{
    juce::File vocal, beat;
    ve::TakeImporter::classifyFiles (files, vocal, beat);
    if (vocal != juce::File())
        processor.importTake (vocal, beat);
}

void VocalEngineEditor::paintOverChildren (juce::Graphics& g)
{
    if (! dragHover)
        return;
    g.fillAll (juce::Colours::black.withAlpha (0.55f));
    g.setColour (ve::ui::Theme::accent);
    g.drawRect (getLocalBounds().reduced (6), 2);
    g.setColour (ve::ui::Theme::text);
    g.setFont (ve::ui::Theme::font (18.0f, true));
    g.drawText ("Drop the vocal (and the beat) to import the whole song as one take",
                getLocalBounds(), juce::Justification::centred);
}

void VocalEngineEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuildFromSession();
}

void VocalEngineEditor::rebuildFromSession()
{
    const auto& session = processor.getSession();

    controls.setArmed (processor.isArmed());
    controls.setMode ((int) processor.getMonitoringMode());
    controls.setKeyOverride (session.getKeyOverride());
    controls.setDataFolderText (session.getDataFolder().getFullPathName());
    controls.setCorrection (processor.getGlobalCorrection());
    controls.setStyle (session.getStyleId());

    const auto est = session.getEstimatedKey();
    controls.setEstimatedKeyText (est.isValid() ? ve::Analysis::keyName (est.tonic, est.minor) : "not analysed");

    ve::ui::TimelineData d;
    bool isOverride = false;
    const auto key = session.getEffectiveKey (&isOverride);
    d.keyText = key.isValid() ? ve::Analysis::keyName (key.tonic, key.minor) : "not analysed";
    d.keyIsOverride = isOverride;

    for (auto& desc : session.getRegionDescriptors())
    {
        ve::ui::TimelineRegion r;
        r.id = desc.id;
        r.order = desc.order;
        r.tempoMap = desc.tempoMap;
        r.startPpq = desc.tempoMap.sampleToPpq (0.0);
        r.endPpq = desc.tempoMap.sampleToPpq ((double) desc.lengthSamples);
        r.missing = desc.missingFiles;
        r.loading = processor.isRegionLoading (desc.id);
        r.hasSidechain = desc.sidechainFile != juce::File();
        if (auto rendered = processor.getRenderCache().find (desc.id, desc.planHash))
        {
            r.peaks = rendered->peaks;
            r.renderedRate = rendered->sampleRate;
        }
        r.chords = session.getChords (desc.id);
        {
            const auto plan = session.getNotePlan (desc.id);
            const auto warp = ve::TimeWarp::build (plan, ve::sharedStyleLibrary().get (session.getStyleId()),
                                                   desc.tempoMap, desc.lengthSamples);
            for (auto& n : plan.notes)
                r.noteOutStart.push_back (warp.outputAt ((double) n.start));
            r.notes = plan.notes;
        }
        r.trackingPitch = ! session.hasNotes (desc.id) && ! desc.missingFiles;
        d.regions.push_back (std::move (r));
    }
    std::sort (d.regions.begin(), d.regions.end(), [] (auto& a, auto& b) { return a.order < b.order; });
    timeline.setData (std::move (d));

    const auto msg = processor.getLastMessage();
    statusBar.setLeftText (msg);
    statusBar.setLeftColour (msg.containsIgnoreCase ("fail") || msg.containsIgnoreCase ("missing") || msg.containsIgnoreCase ("could not")
                                 ? ve::ui::Theme::warn : ve::ui::Theme::text);
}

void VocalEngineEditor::timerCallback()
{
    const auto s = processor.getStatus();
    header.setStatus ({ s.sidechainEnabled, s.sidechainPeak, s.mainInputChannels });

    if (processor.readHostSnapshot (lastHost))
    {
        hostInfo.setSnapshot (lastHost);
        timeline.setHost (lastHost);
    }

    const auto live = processor.getCaptureStatus();
    const double bpm = lastHost.hasBpm ? lastHost.bpm : 120.0;
    const double sr = lastHost.sampleRate > 0.0 ? lastHost.sampleRate : 44100.0;
    timeline.setLiveCapture (live.capturing, live.startPpq, (double) live.frames / sr * bpm / 60.0);

    // Right side of the status bar: capture + jobs.
    juce::String right;
    if (live.capturing)
        right << "Recording take: " << juce::String ((double) live.frames / sr, 1) << " s";
    else if (processor.isArmed())
        right << (processor.getMonitoringMode() == ve::MonitoringMode::bypass ? "Armed (capture off in Bypass)" : "Armed: press play in the host to record");

    for (auto& j : processor.getJobStatus())
    {
        if (right.isNotEmpty()) right << "   |   ";
        right << j.name << (j.running ? " " + juce::String (juce::roundToInt (j.progress * 100.0f)) + "%" : " (queued)");
    }
    if (right.isEmpty())
        right << processor.getSession().getNumRegions() << " take(s)";
    statusBar.setRightText (right);
}
