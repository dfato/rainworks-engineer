#include "PluginEditor.h"
#include "UI/Theme.h"

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

    processor.addChangeListener (this);
    rebuildFromSession();

    setResizable (true, true);
    setResizeLimits (820, 420, 3000, 1800);
    setSize (1100, 560);
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
