#include "PluginEditor.h"
#include "UI/Theme.h"
#include "Engine/TakeImporter.h"
#include "Engine/Style/StyleProfile.h"
#include "Engine/ML/InferenceBackend.h"
#include "Engine/Voice/VoiceModel.h"

VocalEngineEditor::VocalEngineEditor (VocalEngineProcessor& p)
    : juce::AudioProcessorEditor (p),
      processor (p),
      header (JucePlugin_Name, JucePlugin_VersionString)
{
    header.setInterceptsMouseClicks (false, false);
    hostInfo.setInterceptsMouseClicks (false, false);
    addAndMakeVisible (header);
    addAndMakeVisible (controls);
    addAndMakeVisible (timeline);
    addAndMakeVisible (statusBar);
    addAndMakeVisible (hostInfo);
    // After the children exist, so they all get lookAndFeelChanged() (slider text boxes are rebuilt).
    setLookAndFeel (&lnf);
    tooltips.setLookAndFeel (&lnf);

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
    timeline.onNotesChanged = [this] (const std::vector<ve::ui::TimelineNoteEdit>& edits)
    {
        std::vector<VocalEngineProcessor::NoteEditItem> items;
        for (auto& e : edits) items.push_back ({ e.regionId, e.index, e.note });
        processor.updateNotes (items);
    };
    timeline.onUndo = [this] { processor.undoNoteEdit(); };
    timeline.onRedo = [this] { processor.redoNoteEdit(); };
    timeline.onSnapToScaleChanged = [this] (bool on) { processor.setSnapToScale (on); };
    timeline.onSplitNote = [this] (const juce::String& id, int idx, std::int64_t at) { processor.splitNote (id, idx, at); };
    timeline.onMergeNote = [this] (const juce::String& id, int idx) { processor.mergeNoteWithNext (id, idx); };
    controls.onCorrectionChanged = [this] (float a) { processor.setGlobalCorrection (a); };
    controls.onStyleAmountChanged = [this] (float a) { processor.setStyleAmount (a); };
    controls.onStyleChanged = [this] (const juce::String& id) { processor.setStyle (id); };
    controls.onLearnStyle = [this]
    {
        chooser = std::make_unique<juce::FileChooser> ("Choose dry vocals sung the way you want (only the singing style is learned)",
                                                       juce::File::getSpecialLocation (juce::File::userMusicDirectory),
                                                       "*.wav;*.aif;*.aiff;*.flac;*.ogg;*.mp3");
        chooser->launchAsync (juce::FileBrowserComponent::openMode | juce::FileBrowserComponent::canSelectFiles
                                  | juce::FileBrowserComponent::canSelectMultipleItems,
                              [this] (const juce::FileChooser& fc) { askToLearnStyle (fc.getResults()); });
    };
    refreshStyles();

    controls.onGpuChanged = [this] (bool on) { processor.setVoiceGpu (on); };
    controls.onIntentChanged = [this] (bool on) { processor.setUseIntent (on); };
    controls.onVoiceChanged = [this] (const juce::String& id) { processor.setVoice (id); };
    controls.onRescanVoices = [this] { processor.rescanVoices(); refreshVoices(); };
    controls.onOpenVoicesFolder = []
    {
        auto f = ve::VoiceLibrary::voicesFolder();
        f.createDirectory();
        f.startAsProcess();
    };
    refreshVoices();

    processor.addChangeListener (this);
    rebuildFromSession();

    setResizable (true, true);
    setResizeLimits (1540, 520, 3000, 1800);
    setSize (1580, 720);
    startTimerHz (30);
}

VocalEngineEditor::~VocalEngineEditor()
{
    stopTimer();
    processor.removeChangeListener (this);
    if (learnDialog != nullptr) learnDialog->setLookAndFeel (nullptr);
    tooltips.setLookAndFeel (nullptr);
    setLookAndFeel (nullptr);
}

void VocalEngineEditor::paint (juce::Graphics& g)
{
    using namespace ve::ui;
    const auto all = getLocalBounds().toFloat();

    // Base chassis, a shade darker than the top plate.
    Paint::brushedMetal (g, all, 0.0f, 0.93f);

    // Top plate with the raised centre tab that carries the Amount knob.
    for (int i = 4; i >= 1; --i)
    {
        g.setColour (juce::Colours::black.withAlpha (0.045f * (float) (5 - i)));
        g.fillPath (plateShape, juce::AffineTransform::translation (0.0f, (float) i));
    }
    {
        juce::Graphics::ScopedSaveState s (g);
        g.reduceClipRegion (plateShape);
        Paint::brushedMetal (g, plateBounds.toFloat(), 0.0f, 1.03f);
    }
    g.setColour (juce::Colours::white.withAlpha (0.9f));
    g.strokePath (plateShape, juce::PathStrokeType (1.2f), juce::AffineTransform::translation (0.0f, 1.0f));
    g.setColour (Theme::metalEdge);
    g.strokePath (plateShape, juce::PathStrokeType (1.0f));

    // Side rails: light tubes beside the takes screen, a grille and tube beside the notes.
    const auto takes = timeline.getTakesScreenBounds().translated (timeline.getX(), timeline.getY());
    const auto notes = timeline.getNotesScreenBounds().translated (timeline.getX(), timeline.getY());
    for (auto* rail : { &leftRail, &rightRail })
    {
        const float cx = (float) rail->getCentreX();
        const bool left = rail == &leftRail;
        auto tubeA = juce::Rectangle<float> (cx - 4.0f, (float) takes.getY() + 18.0f, 8.0f, (float) takes.getHeight() - 36.0f);
        if (tubeA.getHeight() > 20.0f) Paint::lightTube (g, tubeA, 0.55f);
        auto lower = juce::Rectangle<float> (cx - 11.0f, (float) notes.getY() + 14.0f, 22.0f, (float) notes.getHeight() - 28.0f);
        if (left && lower.getHeight() > 120.0f)
        {
            auto grilleArea = lower.removeFromTop (lower.getHeight() * 0.55f);
            Paint::grille (g, grilleArea);
            lower.removeFromTop (10.0f);
            Paint::lightTube (g, lower.withSizeKeepingCentre (8.0f, lower.getHeight()), 0.7f);
        }
        else if (lower.getHeight() > 20.0f)
            Paint::lightTube (g, lower.withSizeKeepingCentre (8.0f, lower.getHeight()), 0.7f);
        Paint::screw (g, { cx, (float) takes.getY() + 6.0f }, 4.0f);
        Paint::screw (g, { cx, (float) notes.getBottom() - 4.0f }, 4.0f);
    }

    // Footer groove under the status text.
    Paint::groove (g, footer.toFloat().reduced (0.0f, 2.0f), 10.0f);
}

void VocalEngineEditor::resized()
{
    const auto bounds = getLocalBounds();
    const int w = bounds.getWidth();
    const int plateH = 122, tabTop = 2, plateTop = 14;
    plateBounds = juce::Rectangle<int> (0, 0, w, plateH + 4);

    // Plate outline: full width, with a centre tab rising to the top edge around the knob.
    {
        const float x0 = 8.0f, x1 = (float) w - 8.0f, y0 = (float) plateTop, y1 = (float) plateH, r = 16.0f;
        const float cx = (float) w * 0.5f, tabHalf = 92.0f, shoulder = 46.0f;
        juce::Path p;
        p.startNewSubPath (x0 + r, y0);
        p.lineTo (cx - tabHalf - shoulder, y0);
        p.cubicTo (cx - tabHalf - shoulder * 0.35f, y0, cx - tabHalf - shoulder * 0.25f, (float) tabTop, cx - tabHalf + 10.0f, (float) tabTop);
        p.lineTo (cx + tabHalf - 10.0f, (float) tabTop);
        p.cubicTo (cx + tabHalf + shoulder * 0.25f, (float) tabTop, cx + tabHalf + shoulder * 0.35f, y0, cx + tabHalf + shoulder, y0);
        p.lineTo (x1 - r, y0);
        p.quadraticTo (x1, y0, x1, y0 + r);
        p.lineTo (x1, y1 - r);
        p.quadraticTo (x1, y1, x1 - r, y1);
        p.lineTo (x0 + r, y1);
        p.quadraticTo (x0, y1, x0, y1 - r);
        p.lineTo (x0, y0 + r);
        p.quadraticTo (x0, y0, x0 + r, y0);
        p.closeSubPath();
        plateShape = p;
    }

    header.setBounds (26, plateTop + 6, w - 52, 26);
    controls.setBounds (8, tabTop, w - 16, plateH - tabTop);

    auto r = bounds.withTrimmedTop (plateH + 10);
    footer = r.removeFromBottom (32).reduced (44, 3);
    r.removeFromBottom (4);
    leftRail = r.removeFromLeft (40);
    rightRail = r.removeFromRight (40);
    timeline.setBounds (r);
    statusBar.setBounds (footer);
    layoutFooter();
}

void VocalEngineEditor::layoutFooter()
{
    // Transport read-out sits between the status message and the take/job text on the right.
    const int rightW = statusBar.getRightTextWidth();
    const int hostW = juce::jmin (560, juce::jmax (0, footer.getWidth() / 2 - 20));
    auto host = juce::Rectangle<int> (footer.getRight() - rightW - 20 - hostW, footer.getY(), hostW, footer.getHeight());
    hostInfo.setBounds (host);
    statusBar.setMiddleGap ({ host.getX() - footer.getX(), host.getRight() - footer.getX() });
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
    g.drawRoundedRectangle (getLocalBounds().reduced (6).toFloat(), 14.0f, 2.0f);
    g.setColour (ve::ui::Theme::text);
    g.setFont (ve::ui::Theme::font (18.0f, true));
    g.drawText ("Drop the vocal (and the beat) to import the whole song as one take",
                getLocalBounds(), juce::Justification::centred);
}

void VocalEngineEditor::changeListenerCallback (juce::ChangeBroadcaster*)
{
    rebuildFromSession();
}

void VocalEngineEditor::refreshVoices()
{
    std::vector<ve::ui::ControlBar::VoiceItem> items;
    for (auto& v : ve::sharedVoiceLibrary().list())
        items.push_back ({ v.id, v.name });
    controls.setVoices (items, ve::ml::isOnnxAvailable(), ve::ml::onnxUnavailableReason());
}

void VocalEngineEditor::refreshStyles()
{
    shownStyleListVersion = processor.getStyleListVersion();
    std::vector<ve::ui::ControlBar::StyleItem> items;
    for (auto& e : ve::sharedStyleLibrary().list())
        items.push_back ({ e.id, e.name, e.category });
    controls.setStyles (items);
}

void VocalEngineEditor::askToLearnStyle (const juce::Array<juce::File>& files)
{
    if (files.isEmpty())
        return;
    learnDialog = std::make_unique<juce::AlertWindow> ("Learn style from vocals",
        "Engineer measures how these vocals are sung (pitch moves, vibrato, scoops, slides) and saves it as a style "
        "built on the current one. It does not copy the voice.",
        juce::MessageBoxIconType::NoIcon, this);
    learnDialog->setLookAndFeel (&lnf);
    learnDialog->addTextEditor ("name", files[0].getFileNameWithoutExtension() + " style", "Style name");
    learnDialog->addButton ("Learn", 1, juce::KeyPress (juce::KeyPress::returnKey));
    learnDialog->addButton ("Cancel", 0, juce::KeyPress (juce::KeyPress::escapeKey));
    learnDialog->enterModalState (true, juce::ModalCallbackFunction::create ([this, files] (int result)
    {
        const auto name = learnDialog != nullptr ? learnDialog->getTextEditorContents ("name") : juce::String();
        if (result == 1)
            processor.learnStyle (files, name);
        juce::MessageManager::callAsync ([safe = juce::Component::SafePointer<VocalEngineEditor> (this)]
        {
            if (safe != nullptr && safe->learnDialog != nullptr) { safe->learnDialog->setLookAndFeel (nullptr); safe->learnDialog.reset(); }
        });
    }), false);
}

void VocalEngineEditor::rebuildFromSession()
{
    const auto& session = processor.getSession();
    if (shownStyleListVersion != processor.getStyleListVersion())
        refreshStyles();

    controls.setArmed (processor.isArmed());
    controls.setMode ((int) processor.getMonitoringMode());
    controls.setKeyOverride (session.getKeyOverride());
    controls.setDataFolderText (session.getDataFolder().getFullPathName());
    controls.setCorrection (processor.getGlobalCorrection());
    controls.setStyleAmount (processor.getStyleAmount());
    controls.setStyle (session.getStyleId());
    controls.setVoice (session.getVoiceId());
    controls.setIntent (session.getUseIntent());
    controls.setGpu (session.getVoiceGpu(), ve::ml::isGpuSupported());

    const auto est = session.getEstimatedKey();
    controls.setEstimatedKeyText (est.isValid() ? ve::Analysis::keyName (est.tonic, est.minor) : "not analysed");
    controls.setCanAnalyse (processor.countTakesWithBeat() > 0);

    ve::ui::TimelineData d;
    bool isOverride = false;
    const auto key = session.getEffectiveKey (&isOverride);
    d.keyText = key.isValid() ? ve::Analysis::keyName (key.tonic, key.minor) : "not analysed";
    d.keyIsOverride = isOverride;
    d.scaleKey = processor.getSnappingKey();
    if (d.scaleKey.isValid())
    {
        d.scaleText = ve::Analysis::keyName (d.scaleKey.tonic, d.scaleKey.minor);
        if (d.scaleKey.scale != ve::ScaleMode::key)
            d.scaleText << ", " << ve::toString (d.scaleKey.scale);
    }
    else
        d.scaleText = "no key yet: snapping to semitones";
    d.snapToScale = session.getSnapToScale();
    d.canUndo = processor.canUndoNoteEdit();
    d.canRedo = processor.canRedoNoteEdit();

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
                                 ? juce::Colour (0xffa45400) : ve::ui::Theme::ink);
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
    if (right != lastRightText)
    {
        lastRightText = right;
        statusBar.setRightText (right);
        layoutFooter();
    }
}
