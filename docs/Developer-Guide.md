# Developer guide

How Engineer by RainWorks (internal name VocalEngine) is put together and how to build it. For using the
plugin, see the [FL Studio guide](FL-Studio-Guide.md); for a section-by-section test walkthrough and the
detail behind each feature, see the [feature reference](Feature-Reference.md).

VST3 + Standalone, JUCE 8 via CMake FetchContent, C++20. Windows (MSVC) is the target; Linux (GCC) builds
the engine and tests.

## Layout

| Folder | Contents |
|---|---|
| `Source/Plugin` | `VocalEngineProcessor` (audio thread glue, orchestration), `VocalEngineEditor` (wires state to UI) |
| `Source/Engine` | `HostContext`, `TripleBuffer`, `TempoMap`, `CaptureEngine`, `PlaybackEngine`, `RenderCache` + `RegionRenderer`, `SidechainAnalysis`, `JobQueue`, `ML/InferenceBackend`. No GUI includes. |
| `Source/State`  | `SessionModel` (juce::ValueTree session) |
| `Source/UI`     | `HeaderComponent`, `ControlBar`, `HostInfoComponent`, `TimelineComponent`, `StatusBar`, `EngineerLookAndFeel` (brushed aluminium + pink glow, all vector, Inter font embedded from `Resources/Fonts`). Components get plain data, never the processor. |
| `Source/Tools`  | `EngineerStyleLearn` (style learning CLI), `EngineerUiSnapshot` (renders the real editor and its drop-downs to PNG; `-DVE_BUILD_UI_SNAPSHOT=ON`, run under `xvfb-run` on Linux) |
| `Tests`         | `EngineTests` (headless, no GUI modules, which enforces the Engine/UI split), `OnnxSmokeTest` |

### Threads

- **Audio thread**: reads the play head into a `HostSnapshot`, publishes it through a lock-free triple buffer,
  pushes audio and timeline events into `juce::AbstractFifo`s (capture), and plays an immutable
  `PlaybackSnapshot`. No allocation, locks, file I/O or logging.
- **Capture writer thread**: drains the FIFOs and streams 32-bit float WAVs.
- **JobQueue workers** (2): load/render takes into the `RenderCache`, run sidechain analysis, pitch tracking and neural inference.
  Priority, progress, cancellation (per group), completion callbacks on the message thread.
- **Message thread**: owns the `SessionModel`, builds playback snapshots, and frees retired snapshots that the
  audio thread hands back through a FIFO, so nothing is freed on the audio thread.

### Render cache

- `RenderPlan` + `RegionRenderer`: the plan holds everything that shapes a take (notes, edits, style, voice).
  The cache key is region id + `plan.hash()`, so an edit misses the cache for that region only and just that
  take is re-rendered in the background. Playback keeps the previous render until the new one is ready.
- `ml::InferenceBackend`: neural pitch, technique detection and voice models run through this interface
  (ONNX Runtime in-process).

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
memory and runs one inference on a JobQueue worker.

With `VE_WITH_ONNX=ON` the plugin delay-loads ONNX Runtime from beside its own binary, and the build copies
`onnxruntime.dll`, `DirectML.dll` and `pitch-fcpe.onnx` into the VST3 bundle. Without it the plugin still
builds and runs, using the DSP pitch tracker and rules only (no voice models).

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


## Continuous integration

`.github/workflows/build.yml` runs on every push and pull request:

| Job | What it does |
| --- | --- |
| `windows-msvc` | Exports the FCPE pitch model, builds with ONNX Runtime + DirectML, runs the engine tests and ONNX smoke test, compares neural and DSP pitch on vocadito, checks the runtime DLLs and model are inside the VST3 bundle, and uploads **EngineByRainWorks-VST3-win64**. |
| `windows-no-onnx` | Builds the plugin with `VE_WITH_ONNX=OFF`. |
| `linux` | Builds and runs the headless engine tests. |

## UI snapshots

`EngineerUiSnapshot` drives the real processor and editor without a host and writes PNGs of the main view,
the selected-notes view, an empty session and every drop-down:

```
cmake -S . -B build-snap -DVE_BUILD_UI_SNAPSHOT=ON
cmake --build build-snap --target EngineerUiSnapshot
xvfb-run -a build-snap/EngineerUiSnapshot_artefacts/Release/EngineerUiSnapshot out vocal.wav beat.wav --style pop --scale 2
```

On Linux it needs the X11 development packages JUCE uses (libxrandr, libxinerama, libxcursor, libxext,
libxcomposite, libasound2, libfontconfig1, libgl1-mesa). The screenshots in `docs/ui` come from this tool.

## Other tools

| Folder | What it is |
| --- | --- |
| [`tools/style-learning`](../tools/style-learning/README.md) | Learns the built-in genre styles from openly licensed vocals and generates `BuiltinStyles.cpp` |
| [`tools/pitch-models`](../tools/pitch-models) | Exports FCPE to ONNX and evaluates neural vs DSP pitch |
| [`tools/technique-model`](../tools/technique-model) | Trains the vocal technique classifier |
| [`tools/voice-training`](../tools/voice-training/README.md) | Trains and exports voice models (RVC v2) |

The README screenshots were rendered from a 35 s dry vocal ("Take On The Seas", Tom McKenzie, AlbumDB,
CC BY 4.0, https://zenodo.org/records/19683001) and a synthesized F minor chord loop as the beat.
`docs/ui/social-preview.png` (1280 x 640) is for the repository's social preview.
