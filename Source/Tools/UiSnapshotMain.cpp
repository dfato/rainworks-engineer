// EngineerUiSnapshot: renders the real editor (and its menus) to PNG files without a host.
//
//   EngineerUiSnapshot <outDir> [vocal.wav] [beat.wav] [--scale 2] [--size 1580x720] [--style pop]
//
// Writes, into outDir:
//   01-main-view.png        a take loaded, analysed, host transport playing
//   02-graph-selected.png   same, with every note selected (Ctrl+A)
//   03-empty-no-take.png    a fresh session, nothing loaded, no host transport
//   04-hunted.png           the take after one press of Hunt (fixed seed)
//   menus/*.png             every drop-down and right-click menu, drawn with the editor's LookAndFeel
//
// It drives the real processor and editor, so what you see is what FL Studio shows.
// Built only with -DVE_BUILD_UI_SNAPSHOT=ON (Linux needs an X display; use xvfb-run).

#include "Plugin/PluginProcessor.h"
#include "Plugin/PluginEditor.h"
#include "UI/ControlBar.h"
#include "UI/TimelineComponent.h"
#include <juce_audio_processors/juce_audio_processors.h>

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter();

namespace
{
    struct FakePlayHead final : juce::AudioPlayHead
    {
        double ppq = 0.0, bpm = 120.0, sampleRate = 44100.0;
        bool playing = true;

        juce::Optional<PositionInfo> getPosition() const override
        {
            PositionInfo p;
            p.setBpm (bpm);
            p.setTimeSignature (TimeSignature { 4, 4 });
            p.setPpqPosition (ppq);
            p.setPpqPositionOfLastBarStart (std::floor (ppq / 4.0) * 4.0);
            p.setTimeInSamples ((juce::int64) (ppq * 60.0 / bpm * sampleRate));
            p.setTimeInSeconds (ppq * 60.0 / bpm);
            p.setIsPlaying (playing);
            p.setIsLooping (false);
            return p;
        }
    };

    void pump (int ms)
    {
        juce::MessageManager::getInstance()->runDispatchLoopUntil (ms);
    }

    template <typename T>
    T* findChild (juce::Component& root)
    {
        if (auto* t = dynamic_cast<T*> (&root)) return t;
        for (auto* c : root.getChildren())
            if (auto* t = findChild<T> (*c)) return t;
        return nullptr;
    }

    template <typename T>
    void findAll (juce::Component& root, juce::Array<T*>& out)
    {
        if (auto* t = dynamic_cast<T*> (&root)) out.add (t);
        for (auto* c : root.getChildren()) findAll<T> (*c, out);
    }

    bool savePng (const juce::Image& img, const juce::File& f)
    {
        f.getParentDirectory().createDirectory();
        f.deleteFile();
        juce::FileOutputStream out (f);
        juce::PNGImageFormat png;
        const bool ok = out.openedOk() && png.writeImageToStream (img, out);
        std::cout << (ok ? "wrote " : "FAILED ") << f.getFullPathName() << std::endl;
        return ok;
    }

    juce::Image snapshot (juce::Component& c, float scale)
    {
        return c.createComponentSnapshot (c.getLocalBounds(), true, scale);
    }

    // Draws a PopupMenu (and its first submenu, if `openSub` names one) the way
    // PopupMenu's window does, using the given LookAndFeel.
    juce::Image drawMenu (const juce::PopupMenu& menu, juce::LookAndFeel& lf, float scale, int minWidth, int tickedId = 0)
    {
        struct Row { const juce::PopupMenu::Item* item; int h; };
        std::vector<Row> rows;
        int w = minWidth;
        for (juce::PopupMenu::MenuItemIterator it (menu); it.next();)
        {
            auto& item = it.getItem();
            int iw = 0, ih = 0;
            if (item.isSectionHeader)
            {
                ih = 22;
                iw = juce::GlyphArrangement::getStringWidthInt (lf.getPopupMenuFont().boldened(), item.text) + 40;
            }
            else
                lf.getIdealPopupMenuItemSize (item.text, item.isSeparator, -1, iw, ih);
            if (item.subMenu != nullptr) iw += 20;
            w = juce::jmax (w, iw + 8);
            rows.push_back ({ &item, ih });
        }
        const int border = lf.getPopupMenuBorderSize();
        int h = 2 * border;
        for (auto& r : rows) h += r.h;

        juce::Image img (juce::Image::ARGB, juce::roundToInt ((float) w * scale), juce::roundToInt ((float) h * scale), true);
        juce::Graphics g (img);
        g.addTransform (juce::AffineTransform::scale (scale));
        lf.drawPopupMenuBackgroundWithOptions (g, w, h, juce::PopupMenu::Options());
        // The ticked item is the current choice; the next selectable item is drawn hovered,
        // so both states can be reviewed in one picture.
        int y = border;
        bool hoverNext = false;
        for (auto& r : rows)
        {
            auto& item = *r.item;
            const juce::Rectangle<int> area (0, y, w, r.h);
            if (item.isSectionHeader)
                lf.drawPopupMenuSectionHeaderWithOptions (g, area, item.text, juce::PopupMenu::Options());
            else
            {
                const bool ticked = item.isTicked || (tickedId != 0 && item.itemID == tickedId);
                const bool hover = hoverNext && ! item.isSeparator && item.isEnabled;
                if (hover) hoverNext = false;
                lf.drawPopupMenuItem (g, area, item.isSeparator, item.isEnabled, hover, ticked,
                                      item.subMenu != nullptr, item.text, item.shortcutKeyDescription,
                                      item.image.get(), item.colour == juce::Colour() ? nullptr : &item.colour);
                if (ticked) hoverNext = true;
            }
            y += r.h;
        }
        return img;
    }
}

int main (int argc, char* argv[])
{
    juce::ScopedJuceInitialiser_GUI gui;

    juce::StringArray args;
    for (int i = 1; i < argc; ++i) args.add (argv[i]);
    float scale = 2.0f;
    juce::String style;
    int width = 1580, height = 720;
    juce::StringArray positional;
    for (int i = 0; i < args.size(); ++i)
    {
        if (args[i] == "--scale" && i + 1 < args.size()) scale = args[++i].getFloatValue();
        else if (args[i] == "--style" && i + 1 < args.size()) style = args[++i];
        else if (args[i] == "--size" && i + 1 < args.size())
        {
            auto s = args[++i];
            width = s.upToFirstOccurrenceOf ("x", false, false).getIntValue();
            height = s.fromFirstOccurrenceOf ("x", false, false).getIntValue();
        }
        else positional.add (args[i]);
    }
    if (positional.isEmpty())
    {
        std::cout << "usage: EngineerUiSnapshot <outDir> [vocal] [beat] [--scale 2] [--size 1580x720] [--style id]" << std::endl;
        return 1;
    }
    const juce::File outDir = juce::File::getCurrentWorkingDirectory().getChildFile (positional[0]);
    const juce::File vocal = positional.size() > 1 ? juce::File::getCurrentWorkingDirectory().getChildFile (positional[1]) : juce::File();
    const juce::File beat  = positional.size() > 2 ? juce::File::getCurrentWorkingDirectory().getChildFile (positional[2]) : juce::File();

    const auto dataDir = juce::File::createTempFile ("engineer-ui-snapshot");
    dataDir.createDirectory();

    auto render = [&] (bool withTake, const juce::String& name, bool selectAll, bool menus, bool hunted = false)
    {
        std::unique_ptr<VocalEngineProcessor> proc (dynamic_cast<VocalEngineProcessor*> (createPluginFilter()));
        const double sr = 44100.0;
        const int block = 512;
        proc->setPlayConfigDetails (1, 2, sr, block);
        proc->prepareToPlay (sr, block);
        proc->setDataFolder (dataDir.getChildFile (name));

        FakePlayHead head;
        head.sampleRate = sr;
        head.ppq = 10.25;
        if (withTake)
            proc->setPlayHead (&head);

        auto runBlock = [&]
        {
            juce::AudioBuffer<float> buf (juce::jmax (2, proc->getTotalNumInputChannels()), block);
            buf.clear();
            juce::MidiBuffer midi;
            proc->processBlock (buf, midi);
        };

        if (style.isNotEmpty())
            proc->setStyle (style);
        if (withTake && vocal.existsAsFile())
        {
            proc->importTake (vocal, beat);
            // Wait for import, analysis, pitch tracking and render.
            for (int i = 0; i < 2400; ++i)
            {
                runBlock();
                pump (50);
                bool ready = proc->getSession().getNumRegions() > 0;
                for (auto& d : proc->getSession().getRegionDescriptors())
                    ready = ready && proc->getSession().hasNotes (d.id) && ! proc->isRegionLoading (d.id)
                                  && proc->getRenderCache().find (d.id, d.planHash) != nullptr;
                if (ready && proc->getJobStatus().empty()) break;
            }
        }
        runBlock();

        std::unique_ptr<juce::AudioProcessorEditor> ed (proc->createEditor());
        ed->setSize (width, height);
        ed->setVisible (true);
        for (int i = 0; i < 10; ++i) { runBlock(); pump (40); }

        if (selectAll)
            if (auto* tl = findChild<ve::ui::TimelineComponent> (*ed))
            {
                tl->keyPressed (juce::KeyPress ('a', juce::ModifierKeys::commandModifier, 0));
                pump (100);
            }

        if (hunted)
            if (auto* tl = findChild<ve::ui::TimelineComponent> (*ed))
            {
                std::cout << tl->huntWith (7).toStdString() << std::endl;
                for (int i = 0; i < 20; ++i) { runBlock(); pump (40); }
            }

        savePng (snapshot (*ed, scale), outDir.getChildFile (name + ".png"));

        if (menus)
        {
            auto& lf = ed->getLookAndFeel();
            juce::Array<juce::ComboBox*> boxes;
            findAll (*ed, boxes);
            for (auto* box : boxes)
                if (auto* root = box->getRootMenu())
                    savePng (drawMenu (*root, lf, scale, box->getWidth(), box->getSelectedId()),
                             outDir.getChildFile ("menus/" + (box->getName().isNotEmpty() ? box->getName() : juce::String ("combo"))
                                                  + "-dropdown.png"));
            if (auto* tl = findChild<ve::ui::TimelineComponent> (*ed))
                savePng (drawMenu (tl->getHuntMenu(), lf, scale, 200), outDir.getChildFile ("menus/hunt-menu.png"));
        }
        ed.reset();
        proc->releaseResources();
    };

    render (true, "01-main-view", false, true);
    render (true, "02-graph-selected", true, false);
    render (false, "03-empty-no-take", false, false);
    render (true, "04-hunted", false, false, true);
    dataDir.deleteRecursively();
    return 0;
}
