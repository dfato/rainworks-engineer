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

Needs Visual Studio 2022 (Desktop development with C++) and CMake 3.22+ (bundled with VS).

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
```

Outputs:
- `build\VocalEngine_artefacts\Release\VST3\Engineer by RainWorks.vst3`
- `build\VocalEngine_artefacts\Release\Standalone\Engineer by RainWorks.exe`

Install the VST3 by copying the whole `.vst3` folder to `C:\Program Files\Common Files\VST3\`.

### Optional: ONNX Runtime smoke test

```
cmake -S . -B build-onnx -G "Visual Studio 17 2022" -A x64 -DVE_WITH_ONNX=ON
cmake --build build-onnx --config Release --target VocalEngineOnnxSmokeTest
ctest --test-dir build-onnx -C Release -R onnx_smoke --output-on-failure
```

CMake downloads the prebuilt ONNX Runtime (`VE_ONNX_VERSION`, default 1.20.1). To use one you already have,
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
3. **Analyse sidechain** re-runs it for all takes; right-click a take > Re-analyse sidechain for one.
4. Pick a key in the Key menu to override it; the timeline shows "(manual)". Pick Auto to go back.

Chord vocabulary: major, minor, 7, sus2, sus4, dim, or N.C. Beats follow the host tempo captured with the take.

### 8. Timeline (Step 8)

- Mouse wheel scrolls, Ctrl + wheel zooms around the mouse, drag on empty space scrolls.
- **Follow** keeps the playhead in view while playing (dragging turns it off).
- Bar numbers and beat lines come from the host tempo and time signature.
- Right-click a take to re-analyse or remove it from the session (its files stay on disk).
- The bottom lane is reserved for Stage 2 note blobs.

## Known limitations (to fix after the full build)

- Takes are aligned by sample position. If you change the project tempo after recording, takes stay at their
  original time, not their original bar.
- Takes load fully into RAM (about 23 MB per stereo minute at 48 kHz). Streaming comes later.
- Standalone has no host transport, so capture and Rendered playback do nothing there.
- The session id is copied when you "Save as" an FL project, so both projects share one data folder.
- No crossfades at take boundaries yet (punch-ins can click).
