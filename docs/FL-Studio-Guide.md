# Engineer by RainWorks: FL Studio Guide

A step-by-step guide to using Engineer in FL Studio on Windows, written for musicians. Every control name and message below matches the plugin as of November 2026.

## What Engineer does and what you need

Engineer is an offline vocal editor that lives on a mixer insert. It takes your vocal as a take, finds every note, reads the key and chords from your beat, then plays back a tuned and styled version in sync with FL. You do not hear changes live while singing: you record or import a take, Engineer renders it in the background, and you hear the result with **Monitor** set to **Rendered**.

You need:

- FL Studio on 64-bit Windows (any edition that runs VST3 effects).
- The Windows build from GitHub Actions (next section). It includes the neural pitch model and GPU support.
- A dry vocal, either recorded through FL or exported as a WAV.
- Optional but recommended: your beat on its own mixer insert, so Engineer can detect the key and chords.

Two things catch everyone the first time. A fresh copy of the plugin starts with **Correct** at 0% and **Monitor** on **Input**, so nothing changes until you pick a style (or raise Correct) and switch Monitor to Rendered. Both are explained below.

## 1. Download and install

1. Open the repository on GitHub, go to **Actions**, click the **build** workflow and open the newest run on `main` with a green tick.
2. Scroll to **Artifacts** at the bottom of the run and download **EngineByRainWorks-VST3-win64**. You must be signed in to GitHub. Artifacts expire after 90 days, so use a recent run.
3. Unzip it. Inside is a folder called **Engineer by RainWorks.vst3**. It is a folder, not a single file, and it holds the plugin plus `onnxruntime.dll`, `DirectML.dll` and `pitch-fcpe.onnx`. Keep it whole.
4. Close FL Studio. Copy the whole **Engineer by RainWorks.vst3** folder into `C:\Program Files\Common Files\VST3\`. Windows will ask for admin permission.
5. Updating later: close FL, delete the old `Engineer by RainWorks.vst3` folder, then copy the new one in. Your projects, takes and styles live in Documents and are not touched.

Use the **windows-msvc** artifact named above. The other CI builds have no neural pitch, technique detection or voice models.

## 2. Load it in FL Studio

1. Open FL Studio. Go to **Options > Manage plugins** and click **Find more plugins** (or **Start scan**). **Engineer by RainWorks** appears under Effects, from the vendor RainWorks. Star it so it shows in your plugin list.
2. Put your vocal on its own mixer insert, for example **Insert 1**. For a recorded vocal, that's the insert your mic input is set on. For an audio clip, set the clip's channel to output to Insert 1.
3. Click an empty slot on Insert 1 and choose **Engineer by RainWorks**. Put it first in the chain, before EQ, compression or reverb, because it needs a dry vocal.
4. The window opens at about 1580 x 720 and can be resized from the corner. From top to bottom you'll see the header, the control bar, the host strip (BPM, bar, play state), the timeline (Takes, Chords and Notes lanes) and the status bar.
5. Press play. With **Monitor** on **Input** the vocal should sound exactly the same with the plugin on or off. The header shows **Input: mono** or **Input: stereo** once audio flows.

Use one instance per vocal track. Each instance keeps its own takes, settings and data folder.

## 3. Send the beat to the sidechain

The sidechain is how Engineer hears your beat to work out the key and the chord on each beat. It's optional, but without it (and without a key set by hand) notes snap to the nearest semitone, like a chromatic tuner, instead of to your song's key.

1. Put the beat (or an instrumental bus with everything except vocals) on its own insert, for example **Insert 2**.
2. **Select Insert 2** in the mixer (the source).
3. On **Insert 1** (the vocal insert with Engineer), right-click the small routing arrow at the bottom of the insert and choose **Sidechain to this track**. Insert 2 still goes to the Master; it now also feeds Insert 1 as a sidechain. Some guides use Shift + click on the arrow instead, which does the same.
4. Open Engineer, click the **cog** at the top left of the plugin window (plugin options), open the **Processing** tab, and find **Connections**. The second input is the sidechain (labelled like **2. Sidechain**). Click its circle to enable it, then drag or click the number field next to it and set it to **2** (the beat insert).
5. Press play. Top right of Engineer the light turns **green** with a level, like **Sidechain: -8.4 dBFS**. If the beat is silent it shows yellow **Sidechain: no signal**. Grey **Sidechain: bus disabled** means step 4 didn't take.

The beat is only listened to while a take is recorded. For imported songs you hand Engineer the beat file instead (next section), so routing isn't needed there.

FL's wrapper labels vary a little between versions and plugin formats, so if you don't see **2. Sidechain**, look for the second input pin in Connections. Checked against [UAD's FL Studio sidechain steps](https://help.uaudio.com/hc/en-us/articles/17949313152532) and [Devious Machines' FL routing guide](https://deviousmachines.freshdesk.com/support/solutions/articles/43000669375-fl-studio-audio-routing); not yet tested with Engineer itself.

## 4. Get a vocal into Engineer

There are two ways. Import is the fastest and most reliable for a finished song. Capture is for recording or punching in while FL plays.

### Option A: Import the whole song (recommended)

1. In FL, export the dry vocal from **bar 1** (File > Export > WAV, or right-click the clip and export). Export the beat from bar 1 too, if you have it. Starting both at bar 1 keeps them lined up.
2. Name the beat file with one of these words in it: **beat**, **inst**, **music**, **track**, **mix**, **backing** or **karaoke**. That's how Engineer tells the two apart. The other file is taken as the vocal.
3. In Engineer click **Import song...** and select both files (Ctrl + click), or drag both onto the plugin window. The window shows "Drop the vocal (and the beat) to import the whole song as one take".
4. The status bar says **Importing vocal.wav + beat.wav**, then **Imported vocal.wav as a take from bar 1 (182.4 s)**. Nothing has to play.
5. Engineer then loads the take, reads the key and chords from the beat, and finds the notes. The Notes lane shows **Tracking pitch...** until it's done (a few seconds for a 3-minute vocal).

WAV, AIFF, FLAC and Ogg work; MP3 works on Windows. The take uses FL's tempo at the moment you import, so set the project tempo first.

### Option B: Capture while FL plays

1. Switch FL to **Song** mode and place the playhead where you want to start.
2. Click **Arm** in Engineer. It turns red and reads **Armed**; the status bar says **Armed: press play in the host to record**.
3. Press play (and record in FL as normal if you're singing live). A red **Recording** block grows on the timeline and the status bar counts **Recording take: 12.3 s**. Every jump of the playhead or loop pass closes a take and starts a new one.
4. Stop. A blue take with its waveform appears. Newer takes sit on top of older ones, so you can punch in over a line.
5. Click **Arm** again to disarm, or every play records another take.

**Capture the whole song during export.** Arm Engineer, then File > Export > WAV over the whole song. FL renders faster than real time and Engineer captures the vocal and sidechain as one take without dropping audio. Disarm afterwards; you can delete the exported WAV.

In **Bypass**, capture is off even when armed (the status bar says **Armed (capture off in Bypass)**). While a take is recording you always hear the live input.

## 5. Every control

The control bar runs left to right in this order. Hover any control for a tooltip.

| Control | What it does | Default |
| --- | --- | --- |
| **Arm** / **Armed** | When armed, pressing play in FL records the vocal and sidechain as a new take. | Off (always off when a project opens) |
| **Monitor: Input** | You hear the live input, untouched. Use while recording. | Yes |
| **Monitor: Rendered** | You hear Engineer's processed takes in sync with FL. Outside takes, the input passes through. Use this to hear any correction. | |
| **Monitor: Bypass** | Untouched input and capture disabled. | |
| **Key** | **Auto (A minor)** uses the key detected from the beat, or 24 manual keys (12 major, 12 minor). A manual key shows "(manual)" on the timeline. | Auto |
| **Style** | Genre style, grouped by category. Picking one sets **Correct** to that style's default and re-renders every take. The last item is **Learn style from vocals...** | Natural |
| **Amount** | How strongly the style's vibrato, scoops, runs, falls and tone are applied. 0% = tuning only, 100% = as designed, 200% = exaggerated. Double-click for 100%. | 100% |
| **Voice** | **As recorded**, or a trained voice model. Also holds **Rescan voices folder**, **Open voices folder...** and **Render on GPU**. | As recorded |
| **Correct** | How far every note you haven't edited moves to its target. 0% = as sung, 100% = on the key. | 0% until you pick a style |
| **Intent** | On: reads what each note was meant to be (slides, vibrato, scoops, passing notes, chosen out-of-key notes, spoken lines) and keeps those as sung or corrects them gently. Off: every note is pulled to the key the same way. | On |
| **Import song...** | Pick the vocal file, and optionally the beat. Same as dragging files onto the window. | |
| **Analyse beat** | Re-runs key and chord detection on every take's sidechain. | |
| **Data folder** | **Show in file browser** opens this instance's take folder. **Locate / change data folder...** points it at a moved folder. | `Documents\RainWorks\Engineer\Sessions\<id>` |

**Header.** Product name and version on the left. On the right, **Input: mono / stereo / waiting** and the sidechain light (green with level, yellow "no signal", grey "bus disabled").

**Host strip.** Mirrors FL: **BPM**, **SIG**, **BAR | BEAT**, **PPQ**, **SAMPLE**, **PLAY**, **REC**, **LOOP (PPQ)** and **RATE**. "--" means FL didn't send that value. A \* after PPQ or SAMPLE means it was worked out from the other value.

**Timeline.**

- **Takes** lane: one blue block per take with its waveform. Red "Missing audio" means its files can't be found. Right-click a take for **Re-analyse sidechain** or **Remove take from session** (the audio files stay on disk).
- **Chords** lane: one chord per beat (major, minor, 7, sus2, sus4, dim, or N.C.), brighter when Engineer is more sure.
- **Notes** lane: one blob per sung note (next section).
- **Key: A minor** sits top right of the ruler.
- Mouse wheel scrolls, **Ctrl + wheel** zooms around the mouse, dragging empty space scrolls. **Follow** (top left) keeps the playhead in view; dragging turns it off.

**Status bar.** Left: the latest message (orange when something failed or is missing). Right: recording time, background jobs with progress (for example "Track pitch 40%"), or the number of takes.

## 6. Editing notes

Each blob in the **Notes** lane is one sung note. Edits re-render only that take in the background, about a quarter second after you let go; you keep hearing the old render until the new one is ready.

**Reading the blobs.**

- Colour of an uncorrected blob = how far it is from the nearest in-key note: **green** under 15 cents, **yellow** under 35, **red** beyond.
- **Blue** = corrected. It's drawn where it will sound, with a faint ghost where you sang it.
- A thin outline above or below = the target pitch, when it differs.
- A **white outline** = you edited it by hand. Correct and Intent no longer change that note.
- A faint outline to the side = where the note was sung before it was moved in time.
- C lines (C3, C4...) mark octaves.

**Mouse actions.**

| Do this | Result |
| --- | --- |
| Drag a blob up or down | Picks a new target, in semitones. The note is set to 100% correction. |
| **Alt** + drag up or down | Fine target in cents. |
| Double-click a blob | Toggles that note between corrected (100%) and as sung (0%). |
| **Shift** + drag left or right | Moves the note in time, snapping to 1/16. |
| **Shift + Alt** + drag | Moves it in time freely. |
| Right-click a blob | Opens the note menu (below). |

**Right-click menu.** The top shows the note name and how far off it is ("A  +23 cents from target") and, with Intent on, how it was read ("Sung as: Slide (40% of Correct)"). Then:

- **Correct fully (100%)**, **Correct halfway (50%)**, **Leave as sung (0%)**
- **Reset target to key**: back to the nearest note and on.
- **Reset timing**: undoes a Shift + drag (only shown as active when the note was moved).
- **Split note here**: cuts the note where you right-clicked, and each half gets its own pitch. Both halves must be at least 30 ms, or the status bar says "Can't split there".
- **Merge with next note**: joins it with the blob to its right.

When a note is moved in time it keeps its length and the gaps on either side stretch or squeeze (to a quarter or up to 4 times their length), so you can't drag a note far past its neighbour. Pitch, vibrato and formants are kept.

## 7. Styles, custom styles and voices

### Built-in styles

A style decides how hard and fast notes snap, which scale they snap to, and how much vibrato, scoops, slides, runs, falls, swing and tone shaping are added. There are 35 built in:

| Category | Styles |
| --- | --- |
| Basic | Natural, Transparent, Hard Tune |
| Pop | Pop, Pop Ballad, Dance Pop, K-Pop, Hyperpop |
| R&B / Soul | R&B, Contemporary R&B, Neo-Soul, Soul, Gospel |
| Hip-hop | Hip-Hop, Trap, Melodic Rap, Drill, Emo Rap |
| Rock | Rock, Pop-Punk, Post-Hardcore, Indie, Metal (clean) |
| Country / Folk | Country, Folk, Singer-Songwriter |
| Electronic | EDM, House, Future Bass |
| Latin / Caribbean / African | Reggaeton, Afrobeats, Dancehall |
| Other | Lo-Fi, Jazz, Musical Theatre |

Some styles also change which notes are allowed. R&B, Soul, Gospel and Rock allow the blues note. Country, Folk, Trap, Afrobeats and Dancehall use the pentatonic scale. Drill, Metal and Reggaeton use harmonic minor, and Jazz, Neo-Soul, House and Lo-Fi use a modal scale. Trap, EDM, Hyperpop and Hard Tune pull notes toward the 1/16 grid; Lo-Fi, Neo-Soul, Afrobeats, Drill and House add swing.

Picking a style resets **Correct** to that style's default, so pick the style first, then fine-tune Correct.

### Learn a style from vocals

1. Choose the style closest to what you want. The learned style builds on it.
2. Open **Style** and pick **Learn style from vocals...** at the bottom.
3. Select one or more dry, solo vocals sung the way you want. Several songs give a steadier result. Up to 15 minutes of each file is read.
4. Name it and click **Learn**. The status bar shows **Learning a style from 3 vocals...**, then **Learned style "My Style" from 3 vocals. Saved to Documents/RainWorks/Engineer/Styles.**
5. It's selected straight away and listed under **My styles**.

It learns how the singing moves (pitch accuracy, transition speed, vibrato, scoops, grit and breath), not the voice, lyrics or melody. Swing and quantize are not learned.

### Custom style files

Put `.json` files in `Documents\RainWorks\Engineer\Styles\` and restart FL. A style can inherit from another and change only what it needs:

```json
{ "id": "my-trap", "name": "My Trap", "inherits": "trap",
  "vibrato": { "addDepthCents": 20, "rateHz": 5.0, "onsetMs": 400 } }
```

The groups you can set are `pitch`, `vibrato`, `slides`, `timing`, `ornaments`, `tone` and `dynamics`. The repository [README](../README.md) lists every field. A file with the same `id` as a built-in replaces it.

### Voice models

A voice model re-sings every take in a trained voice, following your notes, style and timing.

1. Train and export a voice with the guide in [tools/voice-training](../tools/voice-training/README.md) (about 1 to 2 hours on an RTX 3060). It lands in `Documents\RainWorks\Engineer\Voices\<name>\`. The shared content encoder goes in `Documents\RainWorks\Engineer\Models\content-encoder.onnx`; export it once with the same tools.
2. Open **Voice** and pick **Rescan voices folder**. The status bar says "1 voice model(s) found".
3. Pick the voice. The status bar says "Voice: Name (rendering every take, this can take a minute)". The old sound plays until each render is done, then "Voice render done on GPU (DirectML)" or on CPU.
4. **Render on GPU** in the same menu switches between GPU and CPU. **As recorded** goes back to your own voice.

Voice models have only been tested with stand-in models so far, so treat this as a preview.

## 8. Recommended starting settings

Pick the style first (it sets Correct), then adjust. Always listen with **Monitor: Rendered**, and route the beat to the sidechain or set the **Key** by hand.

| Use case | Style | Correct | Amount | Intent |
| --- | --- | --- | --- | --- |
| Natural tuning, any genre | Natural or Transparent | Style default (about 90%) | 100% | On |
| Hard tuned trap or melodic rap | Trap or Hard Tune | 100% | 100% | Off (so every note snaps) |
| Melodic rap that keeps its slides | Melodic Rap | Style default | 100% | On |
| Rapped verse with a sung hook | Hip-Hop | Style default | 100% | On (spoken lines are left alone) |
| Pop lead | Pop or Dance Pop | Style default | 100% | On |
| Ballad | Pop Ballad | Style default | 100% | On |
| R&B, soul, gospel with runs | R&B, Soul or Gospel | Style default | 100% to 130% | On (keeps blue notes) |
| Rock or punk | Rock or Pop-Punk | Style default | 100% | On |
| Lo-fi or neo-soul, laid back | Lo-Fi or Neo-Soul | Style default | 100% | On |
| Light touch-up, keep the performance | Transparent | 40% to 60% | 0% to 50% | On |
| Pitch only, no added effects | Any | Your choice | 0% | On |

Then fix by hand what's left: drag sour notes (red blobs) to the right note, double-click any note you want left alone, and right-click > **Leave as sung (0%)** on ad-libs that sound too robotic.

If the result sounds odd, check the key first. A wrong key (Auto picking the relative minor or a nearby key) is the most common cause. Pick the right key from the **Key** menu.

## 9. Saving, reopening and bouncing

**Saving.** Everything is saved inside the FL project: takes, notes, your edits, style, Amount, Correct, Intent, voice, key and Monitor mode. Arm is always off when a project opens.

**Where audio lives.** Take audio is not inside the .flp. Each instance stores it in `Documents\RainWorks\Engineer\Sessions\<session id>\Regions\` as 32-bit WAVs (about 23 MB per stereo minute at 48 kHz). Use **Data folder > Show in file browser** to see it. Back this folder up with the project, and copy it along if you move the project to another PC.

**Reopening.** Takes reload in the background ("Load take" on the right of the status bar) and play as before.

**Moved or missing audio.** Takes turn red ("Missing audio") and the status bar says how many. Use **Data folder > Locate / change data folder...** and pick the session folder; the takes come back.

**Save as.** A copied project shares the same data folder as the original. That's fine for versions of the same song, but removing takes in one affects what's on disk for both.

**Bouncing the tuned vocal.**

1. Set **Monitor** to **Rendered** and make sure the status bar shows no running jobs.
2. Leave Engineer on Insert 1. Inside a take its output replaces whatever comes in, so the original clip can stay where it is. Outside takes the input passes through, so keep any vocal parts you didn't bring into Engineer playing.
3. Export the song, or render Insert 1 as a stem (File > Export, split mixer tracks), or record Insert 1 into a new audio clip. Keep Engineer disarmed while exporting unless you want a new take.

**Tempo.** Change the project tempo before importing or recording. Takes stay at their original time if you change tempo afterwards.

## 10. Troubleshooting

| You see or hear | What it means | Fix |
| --- | --- | --- |
| Engineer isn't in FL's plugin list | FL hasn't scanned it, or the .vst3 folder isn't in the right place | Check `C:\Program Files\Common Files\VST3\Engineer by RainWorks.vst3` exists as a folder, then Options > Manage plugins > Find more plugins |
| Nothing sounds different | Monitor is on Input, or Correct is 0% | Set **Monitor** to **Rendered** and pick a style or raise **Correct** |
| Changes take a moment to show up | Each edit re-renders that take in the background | Wait for the job on the right of the status bar to finish |
| **Sidechain: bus disabled** (grey) | The sidechain input isn't enabled in FL's wrapper | Section 3, step 4 |
| **Sidechain: no signal** (yellow) | The sidechain is connected but silent | Check the beat insert plays and is the one selected in Connections |
| **Key: not analysed** | No beat was captured or imported with the take | Route the sidechain and record again, import with the beat file, or pick the key by hand |
| Wrong key | Auto picked the relative or a nearby key | Pick the right key in the **Key** menu |
| **Pitch analysis: DSP pitch tracker (...)** | The neural pitch model couldn't load, so the older tracker is used. The reason is in the brackets, e.g. "the pitch model (pitch-fcpe.onnx) isn't installed" | Reinstall the full **EngineByRainWorks-VST3-win64** folder, or put `pitch-fcpe.onnx` in `Documents\RainWorks\Engineer\Models\`. Then remove the take and import it again |
| **Pitch analysis: Neural pitch (FCPE) + technique detection** | All good | None |
| **...(technique detection off: ...)** | Pitch is fine, but grit/breath/belt detection didn't run | Intent still works from the pitch curve |
| "This take sounds mostly spoken or rapped, so pitch is left alone" | Intent decided the take is rap | Turn **Intent** off if you want it tuned anyway |
| Notes lane says **Tracking pitch...** for a long time | Pitch tracking is still running, or failed | Check the status bar for "Pitch tracking failed: ..." |
| **Import failed: ...** | The file couldn't be read | Re-export as WAV and import again |
| Vocal and beat swapped on import | Neither file name had a beat word | Rename the beat to include "beat" or "inst" |
| Take in the wrong place | Exported files didn't start at bar 1, or tempo changed after import | Export from bar 1, set tempo first, then re-import |
| **Take captured with N samples of dropout (disk too slow?)** | The disk couldn't keep up while recording | Record again, or use Import |
| Red **Missing audio** / "N take(s) have missing audio files" | The session folder moved or was deleted | **Data folder > Locate / change data folder...** |
| "Can't split there" | One half would be under 30 ms | Right-click further into the note |
| Voice menu greyed out | Voice models can't run in this build; the tooltip says why | Use the windows-msvc artifact with `onnxruntime.dll` inside the bundle |
| "Voice ... is not installed" | The voice folder is missing or incomplete | Check `Documents\RainWorks\Engineer\Voices\<name>\`, then **Rescan voices folder** |
| "... voice model(s) found. Skipped: ..." | A voice folder has a problem | The reason follows "Skipped:" |
| Render ran on "CPU (GPU failed: ...)" | DirectML couldn't use the GPU | Update GPU drivers; it still renders on the CPU |
| "Learn style failed: no singing found in those files" | The files had no clear solo singing | Use dry, solo vocals |
| Clicks where takes overlap | No crossfades between takes yet | Re-record over the whole phrase, or use one imported take |
| Every play records a new take | Arm is still on | Click **Armed** to disarm |

**Limits to know about.** Takes load fully into RAM. The standalone app has no FL transport, so recording and Rendered playback only work inside FL. Breathy or doubled (more than one voice) material can confuse the pitch tracker.
