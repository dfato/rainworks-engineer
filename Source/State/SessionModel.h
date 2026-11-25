#pragma once

#include <juce_data_structures/juce_data_structures.h>
#include "Engine/CaptureEngine.h"
#include "Engine/RenderCache.h"
#include "Engine/SidechainAnalysis.h"

namespace ve
{
    enum class MonitoringMode : int { input = 0, rendered = 1, bypass = 2 };

    namespace IDs
    {
        #define VE_ID(name) inline const juce::Identifier name { #name };
        VE_ID (Session) VE_ID (version) VE_ID (sessionId) VE_ID (dataFolder) VE_ID (monitoringMode) VE_ID (keyOverride)
        VE_ID (Regions) VE_ID (Region) VE_ID (id) VE_ID (order) VE_ID (startSample) VE_ID (startPpq) VE_ID (lengthSamples)
        VE_ID (sampleRate) VE_ID (numChannels) VE_ID (vocalFile) VE_ID (sidechainFile) VE_ID (planHash) VE_ID (droppedSamples)
        VE_ID (TempoMap) VE_ID (Point) VE_ID (sample) VE_ID (ppq) VE_ID (bpm) VE_ID (num) VE_ID (den)
        VE_ID (Analysis) VE_ID (keyTonic) VE_ID (keyMinor) VE_ID (keyConfidence) VE_ID (chroma)
        VE_ID (Chords) VE_ID (Chord) VE_ID (endPpq) VE_ID (root) VE_ID (quality) VE_ID (confidence)
        #undef VE_ID
    }

    // The session as a juce::ValueTree. Message thread only.
    //
    // Session
    //   Regions
    //     Region (id, order, startSample, startPpq, lengthSamples, sampleRate, numChannels,
    //             vocalFile, sidechainFile [relative to dataFolder], planHash, droppedSamples)
    //       TempoMap / Point (sample, ppq, bpm, num, den)
    //       Analysis (keyTonic, keyMinor, keyConfidence, chroma) / Chords / Chord
    class SessionModel
    {
    public:
        static constexpr int currentVersion = 1;

        SessionModel();

        juce::ValueTree& getTree() noexcept { return tree; }
        const juce::ValueTree& getTree() const noexcept { return tree; }

        // ---- persistence -----------------------------------------------------
        std::unique_ptr<juce::XmlElement> toXml() const;
        // Replaces the session. Returns an error (and keeps the old session) if the XML is not ours.
        juce::Result restoreFromXml (const juce::XmlElement&);

        // ---- settings --------------------------------------------------------
        juce::String getSessionId() const;
        juce::File getDataFolder() const;
        void setDataFolder (const juce::File&);
        static juce::File defaultDataFolderFor (const juce::String& sessionId);

        MonitoringMode getMonitoringMode() const;
        void setMonitoringMode (MonitoringMode);

        // -1 = automatic, 0..11 major tonic, 12..23 minor tonic.
        int getKeyOverride() const;
        void setKeyOverride (int);

        // ---- regions ---------------------------------------------------------
        void addRegion (const CapturedRegion&);
        bool removeRegion (const juce::String& id);
        juce::ValueTree findRegion (const juce::String& id) const;
        int getNumRegions() const;

        std::vector<RegionDescriptor> getRegionDescriptors() const;
        RegionDescriptor getRegionDescriptor (const juce::ValueTree& region) const;

        void setAnalysis (const juce::String& regionId, const SidechainAnalysisResult&);
        bool hasAnalysis (const juce::String& regionId) const;
        std::vector<ChordSpan> getChords (const juce::String& regionId) const;

        // Estimated key over all regions (ignores the override).
        KeyEstimate getEstimatedKey() const;
        // The key in force: the override if set, else the estimate.
        KeyEstimate getEffectiveKey (bool* isOverride = nullptr) const;

        static TempoMap readTempoMap (const juce::ValueTree& region);

    private:
        juce::ValueTree regions() const;
        juce::ValueTree tree;
    };
}
