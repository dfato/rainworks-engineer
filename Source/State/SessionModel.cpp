#include "SessionModel.h"

namespace ve
{
    namespace
    {
        juce::ValueTree makeEmptySession()
        {
            juce::ValueTree t (IDs::Session);
            const auto id = juce::Uuid().toDashedString();
            t.setProperty (IDs::version, SessionModel::currentVersion, nullptr);
            t.setProperty (IDs::sessionId, id, nullptr);
            t.setProperty (IDs::dataFolder, SessionModel::defaultDataFolderFor (id).getFullPathName(), nullptr);
            t.setProperty (IDs::monitoringMode, (int) MonitoringMode::input, nullptr);
            t.setProperty (IDs::keyOverride, -1, nullptr);
            t.appendChild (juce::ValueTree (IDs::Regions), nullptr);
            return t;
        }

        juce::String chromaToString (const Chroma& c)
        {
            juce::StringArray s;
            for (auto v : c) s.add (juce::String (v, 6));
            return s.joinIntoString (" ");
        }

        Chroma chromaFromString (const juce::String& str)
        {
            Chroma c {};
            juce::StringArray s;
            s.addTokens (str, " ", "");
            for (int i = 0; i < juce::jmin (12, s.size()); ++i)
                c[(size_t) i] = s[i].getFloatValue();
            return c;
        }

        // int64 values go through strings so they survive XML exactly.
        juce::int64 getInt64 (const juce::ValueTree& t, const juce::Identifier& id)
        {
            return t.getProperty (id).toString().getLargeIntValue();
        }
    }

    SessionModel::SessionModel() : tree (makeEmptySession()) {}

    juce::File SessionModel::defaultDataFolderFor (const juce::String& sessionId)
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("RainWorks").getChildFile ("Engineer").getChildFile ("Sessions")
                   .getChildFile (sessionId);
    }

    std::unique_ptr<juce::XmlElement> SessionModel::toXml() const
    {
        return tree.createXml();
    }

    juce::Result SessionModel::restoreFromXml (const juce::XmlElement& xml)
    {
        auto t = juce::ValueTree::fromXml (xml);
        if (! t.isValid() || ! t.hasType (IDs::Session))
            return juce::Result::fail ("Not an Engineer session");

        const int v = t.getProperty (IDs::version, 0);
        if (v > currentVersion)
            return juce::Result::fail ("Session was saved by a newer version (" + juce::String (v) + ")");

        if (t.getProperty (IDs::sessionId).toString().isEmpty())
            t.setProperty (IDs::sessionId, juce::Uuid().toDashedString(), nullptr);
        if (! t.getChildWithName (IDs::Regions).isValid())
            t.appendChild (juce::ValueTree (IDs::Regions), nullptr);
        t.setProperty (IDs::version, currentVersion, nullptr);

        // Data folder: prefer the saved one; if it vanished, try the default for this session id.
        juce::File folder (t.getProperty (IDs::dataFolder).toString());
        if (! juce::File::isAbsolutePath (t.getProperty (IDs::dataFolder).toString()) || ! folder.isDirectory())
        {
            const auto fallback = defaultDataFolderFor (t.getProperty (IDs::sessionId).toString());
            if (fallback.isDirectory() || ! juce::File::isAbsolutePath (t.getProperty (IDs::dataFolder).toString()))
                t.setProperty (IDs::dataFolder, fallback.getFullPathName(), nullptr);
        }

        tree.copyPropertiesAndChildrenFrom (t, nullptr);
        return juce::Result::ok();
    }

    juce::String SessionModel::getSessionId() const { return tree.getProperty (IDs::sessionId).toString(); }
    juce::File SessionModel::getDataFolder() const  { return juce::File (tree.getProperty (IDs::dataFolder).toString()); }
    void SessionModel::setDataFolder (const juce::File& f) { tree.setProperty (IDs::dataFolder, f.getFullPathName(), nullptr); }

    MonitoringMode SessionModel::getMonitoringMode() const
    {
        return (MonitoringMode) juce::jlimit (0, 2, (int) tree.getProperty (IDs::monitoringMode, 0));
    }
    void SessionModel::setMonitoringMode (MonitoringMode m) { tree.setProperty (IDs::monitoringMode, (int) m, nullptr); }

    int SessionModel::getKeyOverride() const { return juce::jlimit (-1, 23, (int) tree.getProperty (IDs::keyOverride, -1)); }
    void SessionModel::setKeyOverride (int k) { tree.setProperty (IDs::keyOverride, juce::jlimit (-1, 23, k), nullptr); }

    juce::ValueTree SessionModel::regions() const { return tree.getChildWithName (IDs::Regions); }
    int SessionModel::getNumRegions() const { return regions().getNumChildren(); }

    juce::ValueTree SessionModel::findRegion (const juce::String& id) const
    {
        return regions().getChildWithProperty (IDs::id, id);
    }

    void SessionModel::addRegion (const CapturedRegion& r)
    {
        juce::ValueTree v (IDs::Region);
        v.setProperty (IDs::id, r.id, nullptr);
        v.setProperty (IDs::order, juce::String (r.order), nullptr);
        v.setProperty (IDs::startSample, juce::String (r.startSample), nullptr);
        v.setProperty (IDs::startPpq, r.startPpq, nullptr);
        v.setProperty (IDs::lengthSamples, juce::String (r.lengthSamples), nullptr);
        v.setProperty (IDs::sampleRate, r.sampleRate, nullptr);
        v.setProperty (IDs::numChannels, r.numChannels, nullptr);
        v.setProperty (IDs::vocalFile, r.vocalFile, nullptr);
        v.setProperty (IDs::sidechainFile, r.sidechainFile, nullptr);
        v.setProperty (IDs::planHash, juce::String::toHexString ((juce::int64) RenderPlan::identity().hash()), nullptr);
        v.setProperty (IDs::droppedSamples, juce::String (r.droppedSamples), nullptr);

        juce::ValueTree tm (IDs::TempoMap);
        for (auto& p : r.tempoMap.getPoints())
        {
            juce::ValueTree pt (IDs::Point);
            pt.setProperty (IDs::sample, juce::String (p.sample), nullptr);
            pt.setProperty (IDs::ppq, p.ppq, nullptr);
            pt.setProperty (IDs::bpm, p.bpm, nullptr);
            pt.setProperty (IDs::num, p.numerator, nullptr);
            pt.setProperty (IDs::den, p.denominator, nullptr);
            tm.appendChild (pt, nullptr);
        }
        v.appendChild (tm, nullptr);
        regions().appendChild (v, nullptr);
    }

    bool SessionModel::removeRegion (const juce::String& id)
    {
        auto r = findRegion (id);
        if (! r.isValid()) return false;
        regions().removeChild (r, nullptr);
        return true;
    }

    TempoMap SessionModel::readTempoMap (const juce::ValueTree& region)
    {
        TempoMap m ((double) region.getProperty (IDs::sampleRate, 44100.0));
        for (auto pt : region.getChildWithName (IDs::TempoMap))
            m.addPoint ({ getInt64 (pt, IDs::sample), (double) pt.getProperty (IDs::ppq),
                          (double) pt.getProperty (IDs::bpm, 120.0), (int) pt.getProperty (IDs::num, 4),
                          (int) pt.getProperty (IDs::den, 4) });
        if (m.isEmpty())
            m.addPoint ({ 0, (double) region.getProperty (IDs::startPpq), 120.0, 4, 4 });
        return m;
    }

    RegionDescriptor SessionModel::getRegionDescriptor (const juce::ValueTree& r) const
    {
        const auto folder = getDataFolder();
        RegionDescriptor d;
        d.id = r.getProperty (IDs::id).toString();
        d.order = getInt64 (r, IDs::order);
        d.startSample = getInt64 (r, IDs::startSample);
        d.startPpq = r.getProperty (IDs::startPpq);
        d.lengthSamples = getInt64 (r, IDs::lengthSamples);
        d.captureSampleRate = r.getProperty (IDs::sampleRate, 44100.0);
        d.numChannels = r.getProperty (IDs::numChannels, 1);
        d.vocalFile = folder.getChildFile (r.getProperty (IDs::vocalFile).toString());
        const auto sc = r.getProperty (IDs::sidechainFile).toString();
        if (sc.isNotEmpty())
            d.sidechainFile = folder.getChildFile (sc);
        d.tempoMap = readTempoMap (r);
        d.planHash = (std::uint64_t) r.getProperty (IDs::planHash).toString().getHexValue64();
        d.missingFiles = ! d.vocalFile.existsAsFile();
        return d;
    }

    std::vector<RegionDescriptor> SessionModel::getRegionDescriptors() const
    {
        std::vector<RegionDescriptor> out;
        for (auto r : regions())
            out.push_back (getRegionDescriptor (r));
        return out;
    }

    void SessionModel::setAnalysis (const juce::String& regionId, const SidechainAnalysisResult& a)
    {
        auto r = findRegion (regionId);
        if (! r.isValid()) return;

        r.removeChild (r.getChildWithName (IDs::Analysis), nullptr);
        juce::ValueTree an (IDs::Analysis);
        an.setProperty (IDs::keyTonic, a.key.tonic, nullptr);
        an.setProperty (IDs::keyMinor, a.key.minor, nullptr);
        an.setProperty (IDs::keyConfidence, a.key.confidence, nullptr);
        an.setProperty (IDs::chroma, chromaToString (a.chromaSum), nullptr);

        juce::ValueTree chords (IDs::Chords);
        for (auto& c : a.chords)
        {
            juce::ValueTree ch (IDs::Chord);
            ch.setProperty (IDs::startPpq, c.startPpq, nullptr);
            ch.setProperty (IDs::endPpq, c.endPpq, nullptr);
            ch.setProperty (IDs::root, c.root, nullptr);
            ch.setProperty (IDs::quality, (int) c.quality, nullptr);
            ch.setProperty (IDs::confidence, c.confidence, nullptr);
            chords.appendChild (ch, nullptr);
        }
        an.appendChild (chords, nullptr);
        r.appendChild (an, nullptr);
    }

    bool SessionModel::hasAnalysis (const juce::String& regionId) const
    {
        return findRegion (regionId).getChildWithName (IDs::Analysis).isValid();
    }

    std::vector<ChordSpan> SessionModel::getChords (const juce::String& regionId) const
    {
        std::vector<ChordSpan> out;
        for (auto c : findRegion (regionId).getChildWithName (IDs::Analysis).getChildWithName (IDs::Chords))
            out.push_back ({ (double) c.getProperty (IDs::startPpq), (double) c.getProperty (IDs::endPpq),
                             (int) c.getProperty (IDs::root, -1), (ChordQuality) (int) c.getProperty (IDs::quality, 0),
                             (float) c.getProperty (IDs::confidence, 0.0f) });
        return out;
    }

    KeyEstimate SessionModel::getEstimatedKey() const
    {
        Chroma total {};
        bool any = false;
        for (auto r : regions())
        {
            auto an = r.getChildWithName (IDs::Analysis);
            if (! an.isValid()) continue;
            const auto c = chromaFromString (an.getProperty (IDs::chroma).toString());
            for (size_t i = 0; i < 12; ++i) total[i] += c[i];
            any = true;
        }
        return any ? Analysis::estimateKey (total) : KeyEstimate {};
    }

    KeyEstimate SessionModel::getEffectiveKey (bool* isOverride) const
    {
        const int o = getKeyOverride();
        if (isOverride != nullptr) *isOverride = o >= 0;
        if (o >= 0)
        {
            KeyEstimate k;
            k.tonic = o % 12;
            k.minor = o >= 12;
            k.confidence = 1.0f;
            return k;
        }
        return getEstimatedKey();
    }
}
