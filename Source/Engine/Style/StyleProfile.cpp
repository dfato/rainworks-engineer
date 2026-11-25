#include "StyleProfile.h"
#include <algorithm>

namespace ve
{
    namespace
    {
        // Child values win; objects merge recursively.
        juce::var merge (const juce::var& parent, const juce::var& child)
        {
            if (! parent.isObject() || ! child.isObject())
                return child.isVoid() ? parent : child;

            auto* out = new juce::DynamicObject();
            for (auto& p : parent.getDynamicObject()->getProperties())
                out->setProperty (p.name, p.value);
            for (auto& c : child.getDynamicObject()->getProperties())
                out->setProperty (c.name, merge (out->getProperty (c.name), c.value));
            return juce::var (out);
        }

        float num (const juce::var& obj, const char* section, const char* key, float fallback)
        {
            const auto s = obj.getProperty (section, {});
            const auto v = s.getProperty (key, {});
            return v.isVoid() ? fallback : (float) (double) v;
        }

        StyleProfile fromVar (const juce::var& v)
        {
            StyleProfile p;
            p.id = v.getProperty ("id", p.id).toString();
            p.name = v.getProperty ("name", p.id).toString();
            p.category = v.getProperty ("category", p.category).toString();
            p.inherits = v.getProperty ("inherits", {}).toString();
            p.description = v.getProperty ("description", {}).toString();

            p.correction       = juce::jlimit (0.0f, 1.0f, num (v, "pitch", "correction", p.correction));
            p.retuneMs         = juce::jlimit (0.0f, 500.0f, num (v, "pitch", "retuneMs", p.retuneMs));
            p.keepVariation    = juce::jlimit (0.0f, 1.0f, num (v, "pitch", "keepVariation", p.keepVariation));
            if (auto sc = v.getProperty ("pitch", {}).getProperty ("scale", {}); ! sc.isVoid())
                p.scale = scaleModeFromString (sc.toString());
            p.vibratoDepthCents = juce::jlimit (0.0f, 150.0f, num (v, "vibrato", "addDepthCents", p.vibratoDepthCents));
            p.vibratoRateHz    = juce::jlimit (1.0f, 12.0f, num (v, "vibrato", "rateHz", p.vibratoRateHz));
            p.vibratoOnsetMs   = juce::jlimit (0.0f, 2000.0f, num (v, "vibrato", "onsetMs", p.vibratoOnsetMs));
            p.slideMs          = juce::jlimit (0.0f, 400.0f, num (v, "slides", "betweenNotesMs", p.slideMs));
            p.slideMaxGapMs    = juce::jlimit (0.0f, 500.0f, num (v, "slides", "maxGapMs", p.slideMaxGapMs));
            p.scoopCents       = juce::jlimit (0.0f, 300.0f, num (v, "slides", "scoopCents", p.scoopCents));
            p.scoopMs          = juce::jlimit (0.0f, 400.0f, num (v, "slides", "scoopMs", p.scoopMs));
            p.quantize         = juce::jlimit (0.0f, 1.0f, num (v, "timing", "quantize", p.quantize));
            p.grid             = juce::jlimit (1, 64, (int) num (v, "timing", "grid", (float) p.grid));
            p.swing            = juce::jlimit (0.0f, 1.0f, num (v, "timing", "swing", p.swing));
            p.runs             = juce::jlimit (0.0f, 1.0f, num (v, "ornaments", "runs", p.runs));
            p.runNotes         = juce::jlimit (2, 8, (int) num (v, "ornaments", "runNotes", (float) p.runNotes));
            p.runNoteMs        = juce::jlimit (40.0f, 300.0f, num (v, "ornaments", "runNoteMs", p.runNoteMs));
            p.fallCents        = juce::jlimit (0.0f, 1200.0f, num (v, "ornaments", "fallCents", p.fallCents));
            p.fallMs           = juce::jlimit (0.0f, 600.0f, num (v, "ornaments", "fallMs", p.fallMs));
            p.brightness       = juce::jlimit (-1.0f, 1.0f, num (v, "tone", "brightness", p.brightness));
            p.warmth           = juce::jlimit (-1.0f, 1.0f, num (v, "tone", "warmth", p.warmth));
            p.grit             = juce::jlimit (0.0f, 1.0f, num (v, "tone", "grit", p.grit));
            p.breath           = juce::jlimit (0.0f, 1.0f, num (v, "tone", "breath", p.breath));
            p.compress         = juce::jlimit (0.0f, 1.0f, num (v, "dynamics", "compress", p.compress));
            return p;
        }
    }

    juce::var StyleProfile::toVar() const
    {
        auto section = [] (std::initializer_list<std::pair<const char*, juce::var>> kv)
        {
            auto* o = new juce::DynamicObject();
            for (auto& [k, val] : kv) o->setProperty (k, val);
            return juce::var (o);
        };
        auto* o = new juce::DynamicObject();
        o->setProperty ("id", id);
        o->setProperty ("name", name);
        o->setProperty ("category", category);
        o->setProperty ("pitch", section ({ { "correction", correction }, { "retuneMs", retuneMs }, { "keepVariation", keepVariation }, { "scale", toString (scale) } }));
        o->setProperty ("vibrato", section ({ { "addDepthCents", vibratoDepthCents }, { "rateHz", vibratoRateHz }, { "onsetMs", vibratoOnsetMs } }));
        o->setProperty ("slides", section ({ { "betweenNotesMs", slideMs }, { "maxGapMs", slideMaxGapMs }, { "scoopCents", scoopCents }, { "scoopMs", scoopMs } }));
        o->setProperty ("timing", section ({ { "quantize", quantize }, { "grid", grid }, { "swing", swing } }));
        o->setProperty ("ornaments", section ({ { "runs", runs }, { "runNotes", runNotes }, { "runNoteMs", runNoteMs },
                                                { "fallCents", fallCents }, { "fallMs", fallMs } }));
        o->setProperty ("tone", section ({ { "brightness", brightness }, { "warmth", warmth }, { "grit", grit }, { "breath", breath } }));
        o->setProperty ("dynamics", section ({ { "compress", compress } }));
        return juce::var (o);
    }

    StyleProfile StyleProfile::withAmount (float amount) const
    {
        amount = juce::jlimit (0.0f, 2.0f, amount);
        if (std::abs (amount - 1.0f) < 1.0e-4f) return *this;
        StyleProfile p = *this;
        p.vibratoDepthCents = juce::jlimit (0.0f, 150.0f, vibratoDepthCents * amount);
        p.scoopCents = juce::jlimit (0.0f, 300.0f, scoopCents * amount);
        p.slideMs = juce::jlimit (0.0f, 400.0f, slideMs * amount);
        p.runs = juce::jlimit (0.0f, 1.0f, runs * amount);
        p.fallCents = juce::jlimit (0.0f, 1200.0f, fallCents * amount);
        p.brightness = juce::jlimit (-1.0f, 1.0f, brightness * amount);
        p.warmth = juce::jlimit (-1.0f, 1.0f, warmth * amount);
        p.grit = juce::jlimit (0.0f, 1.0f, grit * amount);
        p.breath = juce::jlimit (0.0f, 1.0f, breath * amount);
        p.compress = juce::jlimit (0.0f, 1.0f, compress * amount);
        // Less style also keeps more of the singer's own wobble; more style flattens it further.
        p.keepVariation = juce::jlimit (0.0f, 1.0f, keepVariation + (1.0f - keepVariation) * juce::jmax (0.0f, 1.0f - amount)
                                                       - keepVariation * 0.5f * juce::jmax (0.0f, amount - 1.0f));
        return p;
    }

    std::uint64_t StyleProfile::hash() const
    {
        // Only parameters that change the sound (correction is per note, so excluded).
        const float vals[] = { retuneMs, keepVariation, vibratoDepthCents, vibratoRateHz, vibratoOnsetMs,
                               slideMs, slideMaxGapMs, scoopCents, scoopMs, quantize, (float) grid, swing, (float) (int) scale,
                               runs, (float) runNotes, runNoteMs, fallCents, fallMs, brightness, warmth, grit, breath, compress };
        std::uint64_t h = 1469598103934665603ull;
        auto* p = reinterpret_cast<const unsigned char*> (vals);
        for (size_t i = 0; i < sizeof (vals); ++i) { h ^= p[i]; h *= 1099511628211ull; }
        return h;
    }

    //==========================================================================
    StyleLibrary::StyleLibrary()
    {
        for (auto* j : BuiltinStyles::json())
        {
            const auto err = addJson (j);
            jassert (err.isEmpty()); juce::ignoreUnused (err);
        }
    }

    juce::String StyleLibrary::addJson (const juce::String& json)
    {
        juce::var v;
        const auto r = juce::JSON::parse (json, v);
        if (r.failed()) return r.getErrorMessage();
        if (! v.isObject()) return "Style must be a JSON object";
        const auto id = v.getProperty ("id", {}).toString().trim();
        if (id.isEmpty()) return "Style has no \"id\"";
        raw[id] = v;
        return {};
    }

    juce::StringArray StyleLibrary::loadFolder (const juce::File& folder)
    {
        juce::StringArray errors;
        for (auto& f : folder.findChildFiles (juce::File::findFiles, false, "*.json"))
            if (const auto e = addJson (f.loadFileAsString()); e.isNotEmpty())
                errors.add (f.getFileName() + ": " + e);
        return errors;
    }

    juce::var StyleLibrary::resolve (const juce::String& id, int depth) const
    {
        auto it = raw.find (id);
        if (it == raw.end() || depth > 16) return {};
        const auto parentId = it->second.getProperty ("inherits", {}).toString();
        if (parentId.isEmpty() || parentId == id) return it->second;
        return merge (resolve (parentId, depth + 1), it->second);
    }

    StyleProfile StyleLibrary::get (const juce::String& id) const
    {
        auto v = resolve (id, 0);
        if (v.isVoid()) v = resolve ("natural", 0);
        auto p = fromVar (v);
        if (auto it = raw.find (id); it != raw.end())
        {
            // Identity fields are never inherited.
            p.id = id;
            p.name = it->second.getProperty ("name", id).toString();
            p.inherits = it->second.getProperty ("inherits", {}).toString();
        }
        return p;
    }

    std::vector<StyleLibrary::Entry> StyleLibrary::list() const
    {
        std::vector<Entry> out;
        for (auto& [id, v] : raw)
        {
            const auto p = get (id);
            out.push_back ({ id, p.name, p.category });
        }
        std::sort (out.begin(), out.end(), [] (const Entry& a, const Entry& b)
        {
            if (a.category != b.category) return a.category == "Basic" || (b.category != "Basic" && a.category < b.category);
            return a.name < b.name;
        });
        return out;
    }

    juce::File StyleLibrary::userStylesFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                   .getChildFile ("RainWorks").getChildFile ("Engineer").getChildFile ("Styles");
    }
}

namespace ve
{
    StyleLibrary& sharedStyleLibrary()
    {
        static StyleLibrary lib = []
        {
            StyleLibrary l;
            l.loadFolder (StyleLibrary::userStylesFolder());
            return l;
        }();
        return lib;
    }
}
