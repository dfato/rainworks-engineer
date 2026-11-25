# Engineer by RainWorks (VocalEngine)

VST3 + Standalone vocal processing plugin. JUCE 8 via CMake FetchContent, C++20.

## Layout

| Folder | Contents |
|---|---|
| `Source/Plugin` | `VocalEngineProcessor`, `VocalEngineEditor` |
| `Source/Engine` | Jobs, capture, analysis, render cache (no UI includes) |
| `Source/State`  | Session model (Step 5) |
| `Source/UI`     | Components; take plain values, never the processor |

## Build on Windows (MSVC)

Needs Visual Studio 2022 (Desktop development with C++) and CMake 3.22+ (bundled with VS).

```
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
```

Outputs:
- `build\VocalEngine_artefacts\Release\VST3\Engineer by RainWorks.vst3`
- `build\VocalEngine_artefacts\Release\Standalone\Engineer by RainWorks.exe`

Install the VST3 by copying the whole `.vst3` folder to `C:\Program Files\Common Files\VST3\`.

## Bus layout

- Main in: mono or stereo vocal. Main out: same as main in.
- Sidechain in: stereo (optional; plugin runs with it disconnected).
- Latency: 0.

## FL Studio: testing Step 1

1. Copy the `.vst3` folder into `C:\Program Files\Common Files\VST3\`.
2. FL Studio: Options > Manage plugins > Find more plugins (or Start scan). "Engineer by RainWorks" appears under Effects.
3. Put an audio clip of a vocal on a playlist track routed to mixer insert 1 (the vocal track).
4. On insert 1, add **Engineer by RainWorks** in an effect slot. The editor shows the name, version, "Main input: stereo" (or mono) once audio plays, and a sidechain status line.
5. Play. The vocal must sound identical with the plugin on and bypassed (passthrough).

### Routing the instrumental into the sidechain

1. Route your instrumental (beat) to its own mixer insert, say insert 2.
2. In the mixer, select **insert 2** (the source). Right-click the small routing arrow at the bottom of **insert 1** (the vocal) and choose **Sidechain to this track**. Insert 2 still feeds Master; it now also feeds insert 1 as a sidechain source.
3. Open the plugin window on insert 1, click the wrapper's cog icon (top-left), go to the **Processing** tab and find the **Connections** section. Set the plugin's sidechain input (the second input port, labelled Sidechain) to insert 2.
4. Press play. The plugin's status line should turn green: "Sidechain: receiving signal (-x dBFS peak)". Stop or mute insert 2 and it goes yellow ("enabled, no signal").
5. Remove the sidechain routing: the plugin must keep passing the vocal through with no errors.
