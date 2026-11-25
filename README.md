# Engineer by RainWorks (VocalEngine)

VST3 + Standalone vocal processing plugin for FL Studio on Windows. JUCE 8 via CMake FetchContent, C++20.

This build covers Stages 0 and 1: plugin foundation, host sync, capture, cached playback, saved state,
sidechain key/chord detection, the job system with an optional ONNX hook, and the timeline editor.
There is no pitch correction or ML rendering yet.

## Layout

| Folder | Contents |
|---|---|
| `Source/Plugin` | `VocalEngineProcessor` (audio thread glue, orchestration), `VocalEngineEditor` (wires state to UI) |
| `Source/Engine` | `HostContext`, `TripleBuffer`, `TempoMap`, `CaptureEngine`, `PlaybackEngine`, `RenderCache` + `RegionRenderer`, `SidechainAnalysis`, `JobQueue`, `ML/InferenceBackend`. No GUI includes. |
| `Source/State`  | `SessionModel` (juce::ValueTree session) |
| `Source/UI`     | `HeaderComponent`, `ControlBar`, `HostInfoComponent`, `TimelineComponent`, `StatusBar`. Components get plain data, never the processor. |
| `Tests`         | `EngineTests` (headless, no GUI modules, which enforces the Engine/UI split), `OnnxSmokeTest` |

### Threads

- **Audio thread**: reads the play head into a `HostSnapshot`, publishes it through a lock-free triple buffer,
  pushes audio and timeline events into `juce::AbstractFifo`s (capture), and plays an immutable
  `PlaybackSnapshot`. No allocation, locks, file I/O or logging.
- **Capture writer thread**: drains the FIFOs and streams 32-bit float WAVs.
- **JobQueue workers** (2): load/render takes into the `RenderCache`, run sidechain analysis, future inference.
  Priority, progress, cancellation (per group), completion callbacks on the message thread.
- **Message thread**: owns the `SessionModel`, builds playback snapshots, and frees retired snapshots that the
  audio thread hands back through a FIFO, so nothing is freed on the audio thread.

### Hooks for later stages

- `RenderPlan` + `RegionRenderer`: Stage 2 adds the note plan to `RenderPlan`; its `hash()` changes, the cache
  key (region id + plan hash) misses, and only that region is re-rendered. Playback does not change.
- `TimelineComponent::getNoteLaneBounds()`: reserved lane for Stage 2 note blobs.
- `ml::InferenceBackend`: Stage 5 renderer talks to this interface, in-process (ONNX Runtime) or a sidecar.

## Build on Windows (MSVC)

Needs Visual Studio 2022 or newer (Desktop development with C++) and CMake 3.22+ (bundled with VS). Without `-G`, CMake picks the newest Visual Studio installed.

```
cmake -S . -B build -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Outputs:
- `build\VocalEngine_artefacts\Release\VST3\Engineer by RainWorks.vst3`
- `build\VocalEngine_artefacts\Release\Standalone\Engineer by RainWorks.exe`

Install the VST3 by copying the whole `.vst3` folder to `C:\Program Files\Common Files\VST3\`.

### Optional: ONNX Runtime smoke test

```
cmake -S . -B build-onnx -A x64 -DVE_WITH_ONNX=ON
cmake --build build-onnx --config Release --target VocalEngineOnnxSmokeTest
ctest --test-dir build-onnx -C Release -R onnx_smoke --output-on-failure
```

CMake downloads ONNX Runtime from its NuGet package (`VE_ONNX_VERSION`, default 1.20.1). To use one you already have,
pass `-DVE_ONNX_ROOT=C:\path\to\onnxruntime-win-x64-1.20.1`. The test builds a tiny `Y = X + X` model in
memory (no model files ship) and runs one inference on a JobQueue worker. The plugin itself does not link
ONNX Runtime yet.

## Bus layout

- Main in: mono or stereo vocal. Main out: same as main in.
- Sidechain in: stereo (optional; the plugin runs with it disconnected).
- Latency: 0.

## Where takes are stored

Each plugin instance has a session id saved in the FL project. Takes go to
`Documents\RainWorks\Engineer\Sessions\<session id>\Regions\` as `<take id>_vocal.wav` and
`<take id>_sidechain.wav` (32-bit float). The session stores paths relative to that folder.
Use **Data folder > Show in file browser** to see it, or **Locate / change data folder...** to point a project
at a moved folder.

---

## Testing in FL Studio

### 1. Install and load

1. Copy the `.vst3` folder into `C:\Program Files\Common Files\VST3\`.
2. Options > Manage plugins > Find more plugins. "Engineer by RainWorks" appears under Effects.
3. Put a vocal audio clip on a playlist track routed to mixer insert 1. Add **Engineer by RainWorks** to insert 1.
4. Play. With Monitor = Input, the vocal must sound identical with the plugin on or bypassed.

### 2. Route the instrumental into the sidechain

1. Route the beat to its own mixer insert, e.g. insert 2.
2. Select insert 2. Right-click the routing arrow at the bottom of insert 1 and choose **Sidechain to this track**.
   Insert 2 still feeds Master and now also feeds insert 1 as a sidechain source.
3. In the plugin window click the wrapper's cog icon, open the **Processing** tab, find **Connections**, and set
   the plugin's sidechain input (second input port) to insert 2.
4. Press play. Top right of the plugin shows a green light with a dB level. Mute insert 2: it turns yellow.

### 3. Host context (Step 2)

The strip under the controls shows BPM, time signature, bar | beat, PPQ, sample position, play/rec state,
loop on/off with loop points, and sample rate.

- Change the tempo and time signature in FL: the values follow live.
- Turn on loop recording / select a loop range in the playlist: LOOP shows On and the range.
- A field the host does not provide shows `--`. A `*` after PPQ or SAMPLE means it was derived from the other
  value and the tempo.

### 4. Capture (Step 3)

1. Switch FL to **Song mode**. Click **Arm** (turns red).
2. Press play from bar 1 and let it run 8 bars, then stop. The timeline shows a red "Recording" block while it
   plays, then a blue take with its waveform once it is written.
3. Play from bar 5 for 2 bars and stop: a second take appears on top (punch-in).
4. Turn on looping over 2 bars and play: every loop pass becomes its own take (a jump in the timeline always
   closes a take).
5. Data folder > Show in file browser: one `_vocal.wav` (and `_sidechain.wav` if the sidechain is routed) per take.
6. Disarm before moving on, or every play records a new take.

### 4b. Whole song in one go

**Import (fastest).** Export your vocal (and optionally the beat) from FL starting at bar 1, then either click
**Import song...** and select one or both files, or drag them onto the plugin. A file whose name contains
beat/inst/music/track/mix is used as the beat; the other is the vocal. The take lands at bar 1 using the
project's current tempo, the beat feeds sidechain analysis, and nothing has to play. WAV, AIFF, FLAC and Ogg
work everywhere; MP3 works on Windows.

**Capture during export.** Arm the plugin, then File > Export > WAV with the song range. FL renders faster
than real time and the plugin captures the whole song as one take while it does (it waits for the disk
instead of dropping audio during offline renders). Disarm afterwards. You can delete the exported WAV.

### 5. Rendered playback (Step 4)

1. Set Monitor to **Rendered**. Mute the original vocal clip in the playlist (or route it elsewhere) so you only
   hear the plugin.
2. Play from bar 1: you hear the captured take in time. Where takes overlap, the newest one plays.
3. Click around the timeline while playing, loop a range, stop and start: playback stays locked to the song
   position. Outside takes, the plugin passes its input through.
4. While a take is being recorded you always hear the live input.
5. **Bypass** passes audio untouched and disables capture even when armed.

### 6. Save and restore (Step 5)

1. With takes captured, Monitor = Rendered and a manual key set, save the FL project and close FL.
2. Reopen the project. The takes reload in the background ("Load take" in the status bar), then play exactly as
   before. Monitor mode and key override are restored. Arm is always off after loading.
3. Missing files: rename the session's `Regions` folder and reopen. Takes show red "Missing audio", the status
   bar says how many are missing, and the plugin passes input through there. Rename it back and use
   Data folder > Locate... to pick the session folder: the takes come back.

### 7. Sidechain analysis (Step 6)

1. Route the beat into the sidechain (section 2) and record a take of 8 bars or more.
2. Analysis starts automatically after each take. The chord lane fills in with one chord per beat (merged when
   repeated) and the key shows top right of the timeline and in the Key menu ("Auto (C major)").
3. **Analyse beat** re-runs it for all takes; right-click a take > Re-analyse sidechain for one.
4. Pick a key in the Key menu to override it; the timeline shows "(manual)". Pick Auto to go back.

Chord vocabulary: major, minor, 7, sus2, sus4, dim, or N.C. Beats follow the host tempo captured with the take.

### 8. Timeline (Step 8)

- Mouse wheel scrolls, Ctrl + wheel zooms around the mouse, drag on empty space scrolls.
- **Follow** keeps the playhead in view while playing (dragging turns it off).
- Bar numbers and beat lines come from the host tempo and time signature.
- Right-click a take to re-analyse or remove it from the session (its files stay on disk).
- The bottom lane is reserved for Stage 2 note blobs.

### 9. Notes and pitch correction (Stage 2)

1. Record or import a take. After it loads, "Track pitch" runs and the **Notes** lane fills with blobs: one per
   sung note, at the pitch you sang. Colour = how far it is from the nearest in-key note
   (green < 15 cents, yellow < 35, red beyond). An outline shows the target when it differs.
2. Set Monitor to **Rendered** and raise **Correct** (top bar). Every note you haven't touched moves toward its
   target by that amount and the take re-renders in the background (you keep hearing the previous render until
   the new one is ready). Corrected notes turn blue; a faint ghost shows where they were sung.
3. Per note: drag a blob up/down to choose another target (semitone steps, hold Alt for cents), double-click to
   toggle correction on/off, right-click for 100% / 50% / 0% / reset. Edited notes get a white outline and the
   Correct slider no longer changes them.
4. Change the key (Key menu): untouched notes re-snap to the new scale.
5. Save and reopen the project: notes, edits and the corrected sound come back.

Correction moves each note as a whole (vibrato and slides are kept) using TD-PSOLA. Audio outside corrected
notes is passed through untouched.

### 10. Genre styles (Stage 3)

1. With a take that has notes, set Monitor to **Rendered** and pick a **Style** (grouped by genre).
   Picking a style sets **Correct** to that style's default and re-renders every take.
2. Compare a few on the same take: **Natural** keeps vibrato and glides in gently; **Trap** / **Hard Tune**
   snap instantly to a flat line; **R&B** and **Melodic Rap** add slides between close notes and scoops into
   notes; **Pop Ballad**, **Gospel**, **Musical Theatre** add vibrato on held notes.
3. The Correct slider and per-note edits still work on top of the style.
4. Save and reopen: the style is restored with the project.

Built in: 3 basic styles (Natural, Transparent, Hard Tune) and 31 genre styles across Pop, R&B / Soul,
Hip-hop, Rock, Country / Folk, Electronic, Latin / Caribbean / African and Other.

**Custom styles.** Drop `.json` files into `Documents\RainWorks\Engineer\Styles\` and restart FL. A style can
inherit from any other and override only what it changes:

```json
{ "id": "my-trap", "name": "My Trap", "inherits": "trap",
  "vibrato": { "addDepthCents": 20, "rateHz": 5.0, "onsetMs": 400 } }
```

Fields: `pitch` (`correction` 0-1, `retuneMs`, `keepVariation` 0-1), `vibrato` (`addDepthCents`, `rateHz`,
`onsetMs`), `slides` (`betweenNotesMs`, `maxGapMs`, `scoopCents`, `scoopMs`), `timing` (`quantize`, `grid`).
A file with the same `id` as a built-in replaces it.

## Known limitations (to fix after the full build)

- Takes are aligned by sample position. If you change the project tempo after recording, takes stay at their
  original time, not their original bar.
- Takes load fully into RAM (about 23 MB per stereo minute at 48 kHz). Streaming comes later.
- Standalone has no host transport, so capture and Rendered playback do nothing there.
- The session id is copied when you "Save as" an FL project, so both projects share one data folder.
- No crossfades at take boundaries yet (punch-ins can click).
- Pitch only so far: note timing edits, splitting/merging blobs and the styles' `timing` settings
  (quantize/swing) need time-stretching, which comes next.
- Heavily breathy or polyphonic material confuses the pitch tracker (it assumes one voice).
