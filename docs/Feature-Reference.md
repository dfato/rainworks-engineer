# Feature reference

A section-by-section walkthrough of every feature, written as a test plan in FL Studio, plus the detail
behind each one (style fields, Intent rules, pitch-tracker measurements, known limitations). For a
friendlier how-to, start with the [FL Studio guide](FL-Studio-Guide.md).

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
4. Press play. Top right of the plugin shows a glowing pink light with a dB level. Mute insert 2: it turns to an amber dot ("no signal").

### 3. Host context

The transport read-out in the footer (right side) shows BPM, time signature, bar | beat, PPQ, sample position, play/rec state,
loop on/off with loop points, and sample rate.

- Change the tempo and time signature in FL: the values follow live.
- Turn on loop recording / select a loop range in the playlist: LOOP shows On and the range.
- A field the host does not provide shows `--`. A `*` after PPQ or SAMPLE means it was derived from the other
  value and the tempo.

### 4. Capture

1. Switch FL to **Song mode**. Click **Arm** (glows pink, light on).
2. Press play from bar 1 and let it run 8 bars, then stop. The timeline shows a red "Recording" block while it
   plays, then the take's waveform once it is written.
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

### 5. Rendered playback

1. Set Monitor to **Rendered**. Mute the original vocal clip in the playlist (or route it elsewhere) so you only
   hear the plugin.
2. Play from bar 1: you hear the captured take in time. Where takes overlap, the newest one plays.
3. Click around the timeline while playing, loop a range, stop and start: playback stays locked to the song
   position. Outside takes, the plugin passes its input through.
4. While a take is being recorded you always hear the live input.
5. **Bypass** passes audio untouched and disables capture even when armed.

### 6. Save and restore

1. With takes captured, Monitor = Rendered and a manual key set, save the FL project and close FL.
2. Reopen the project. The takes reload in the background ("Load take" in the status bar), then play exactly as
   before. Monitor mode and key override are restored. Arm is always off after loading.
3. Missing files: rename the session's `Regions` folder and reopen. Takes show red "Missing audio", the status
   bar says how many are missing, and the plugin passes input through there. Rename it back and use
   Data folder > Locate... to pick the session folder: the takes come back.

### 7. Sidechain analysis

1. Route the beat into the sidechain (section 2) and record a take of 8 bars or more.
2. Analysis starts automatically after each take. The chord lane fills in with one chord per beat (merged when
   repeated) and the key shows top right of the timeline and in the Key menu ("Auto (C major)").
3. **Analyse beat** re-runs it for all takes; right-click a take > Re-analyse sidechain for one. The status bar
   says "Analysing the beat of 2 takes..." and then "Beat analysed: key F minor, 15 chord changes". The
   button is greyed out while no take has beat audio, and says why in its tooltip.
4. Pick a key in the Key menu to override it; the timeline shows "(manual)". Pick Auto to go back.

Chord vocabulary: major, minor, 7, sus2, sus4, dim, or N.C. Beats follow the host tempo captured with the take.

### 8. Timeline

- Mouse wheel scrolls, Ctrl + wheel zooms around the mouse, drag on empty space scrolls.
- **Follow** keeps the playhead in view while playing (dragging turns it off).
- Bar numbers and beat lines come from the host tempo and time signature.
- Right-click a take to re-analyse or remove it from the session (its files stay on disk).

### 9. Notes and pitch correction

1. Record or import a take. After it loads, "Track pitch" runs and the **Notes** lane fills with blobs: one per
   sung note, at the pitch you sang. Colour = how far it is from the nearest in-key note
   (silver < 15 cents, amber < 35, red beyond). An outline shows the target when it differs.
2. Set Monitor to **Rendered** and raise **Correct** (small knob, top plate). Every note you haven't touched moves toward its
   target by that amount and the take re-renders in the background (you keep hearing the previous render until
   the new one is ready). Corrected notes glow pink; a faint ghost shows where they were sung.
3. Per note: drag a blob up/down to choose another target (semitone steps, hold Alt for cents), double-click to
   toggle correction on/off, right-click for 100% / 50% / 0% / reset. Edited notes get a white outline and the
   Correct knob no longer changes them.
4. Change the key (Key menu): untouched notes re-snap to the new scale.
5. Save and reopen the project: notes, edits and the corrected sound come back.

Correction moves each note as a whole (vibrato and slides are kept) using TD-PSOLA. Audio outside corrected
notes is passed through untouched.

### 10. Genre styles

1. With a take that has notes, set Monitor to **Rendered** and pick a **Style** (grouped by genre).
   Picking a style sets **Correct** to that style's default and re-renders every take.
2. Compare a few on the same take: **Natural** keeps vibrato and glides in gently; **Trap** / **Hard Tune**
   snap instantly to a flat line; **R&B** adds slides between close notes, a soft scoop and vibrato on held
   notes; **Rock** locks in fast with a straight tone; **Indie** stays loose.
3. The Correct knob and per-note edits still work on top of the style.
4. Save and reopen: the style is restored with the project.

Built in: 3 basic styles (Natural, Transparent, Hard Tune) and six genres (Pop, Rock, Indie and R&B learned from openly licensed vocals, Hip-Hop and Trap tuned by hand):

| Genre | Pitch | Movement | Tone |
| --- | --- | --- | --- |
| **Pop** | 100%, fast (14 ms), human (keeps 40% of the singer's wobble) | smooth 40 ms glides, light vibrato on held ends | bright and airy, compressed |
| **Rock** | 100%, fast (13 ms), straight (keeps about 30%) | no added scoops, falls or vibrato | bright, slightly lean, heavily compressed; no added grit, so the singer's own rasp is all you hear |
| **Indie** | 95%, relaxed (about 55 ms), keeps most of the drift | wider glides | warm and soft, lightly compressed |
| **R&B** | 100%, smooth (22 ms), keeps runs and vibrato | 60 ms slides, soft 30-cent scoops, a few runs | warm and close |
| **Hip-Hop** | sung hooks tuned cleanly (20 ms); rapped lines with no clear pitch are left as sung (Intent) | pulled a quarter of the way to the 1/16 grid, light swing | punchy, compressed |
| **Trap** | hard, near-instant (3 ms), flat line | quick glides, pulled firmly to the grid | bright and squashed |

Rock covers modern rock, pop-punk, emo and post-hardcore clean vocals. Every genre tunes to the song's own
key, so none of them rewrites the melody onto another scale.

Earlier versions had 31 genres. A project saved with one of them opens with the genre that replaced it
(for example Post-Hardcore and Pop-Punk open as Rock, Melodic Rap as Trap, Gospel and Neo-Soul as R&B,
Singer-Songwriter and Folk as Indie, Pop Ballad and EDM as Pop). A custom style that inherits from a
retired genre inherits from its replacement.

**Custom styles.** Drop `.json` files into `Documents\RainWorks\Engineer\Styles\` and restart FL. A style can
inherit from any other and override only what it changes:

```json
{ "id": "my-trap", "name": "My Trap", "inherits": "trap",
  "vibrato": { "addDepthCents": 20, "rateHz": 5.0, "onsetMs": 400 } }
```

Fields: `pitch` (`correction` 0-1, `retuneMs`, `keepVariation` 0-1, `scale`), `vibrato` (`addDepthCents`, `rateHz`,
`onsetMs`), `slides` (`betweenNotesMs`, `maxGapMs`, `scoopCents`, `scoopMs`), `timing` (`quantize` 0-1, `grid` divisions per whole note, `swing` 0-1),
`ornaments` (`runs` 0-1, `runNotes` 2-8, `runNoteMs`, `fallCents`, `fallMs`), `tone` (`brightness` -1..1,
`warmth` -1..1, `grit` 0-1, `breath` 0-1) and `dynamics` (`compress` 0-1).
A file with the same `id` as a built-in replaces it.

**Runs, falls and tone.** Each style also has its own ornaments and tone:

- **Runs** are quick lines through the song's own scale on held notes. In the middle of a phrase a run
  turns around the note and comes back. At a phrase end it walks down and lands on a lower scale note.
  R&B uses them on a few long notes; the other genres leave runs to the singer.
- **Falls** drop the pitch off the end of a phrase. No built-in genre adds them; a custom style can.
- **Tone** sets brightness (presence and air), warmth (low mids), grit (rasp that follows the voice's level)
  and breath (aspiration noise pulsed by the voice's own cycles), plus **compression**. Rock, Pop and Trap are bright and
  tightly compressed. R&B and Indie are warm. No built-in genre adds grit or breath.

The loudness of the take is kept, and the gaps between phrases stay exactly as recorded, so the tone stage
never raises the hiss floor (before, the compressor's make-up gain lifted it 2 to 7 dB). The **Amount** knob (centre of the top plate) sets how strongly all of this is
applied: 0% is tuning only, 100% is the style as designed, and 200% exaggerates it (double-click for 100%).
At Correct 0% the take still plays exactly as sung.

**Scales.** `pitch.scale` picks which notes of the key the targets snap to, so a style can change the
melody as well as the delivery: `key` (the key's own major or natural minor), `pentatonic`, `blues`
(adds the blue note), `harmonic` (raised 7th), `modal` (mixolydian in major keys, dorian in minor) or
`chromatic` (nearest semitone). A note moved to another scale degree is moved all the way, whatever the
Correct amount, so it never lands between two notes. All built-in styles use `key`; the other scales are
there for custom styles.

**Learn a style from vocals.** At the bottom of the Style menu, pick **Learn style from vocals...**, choose
one or more dry, solo vocals sung the way you want, and name the style. Engineer measures how they are sung
(how close notes sit to pitch, how fast the voice moves between notes, vibrato rate/depth/onset and how
often it is used, scoops into notes) and saves a new style built on the one currently selected, under
**My styles** (also written to `Documents\RainWorks\Engineer\Styles\`). Only those numbers are kept: it
does not copy the voice, words or melody, so it shapes your take toward the reference's phrasing without
making you sound like that singer. Several references (ideally different songs) give a steadier result.
Timing (swing/quantize) is not learned here because a reference file's tempo is unknown.

The built-in genre styles were tuned the same way from openly licensed singing, with each genre's
signature (scale, tone, compression and, for Rock, Hip-Hop and Trap, the whole pitch feel) set by hand (see
[`tools/style-learning/README.md`](../tools/style-learning/README.md) for the sources, licences and which genres had data).

### 11. Timing: quantize, swing and note nudges

1. Pick **Trap**, **Hip-Hop** or **Hard Tune** on a take sung slightly off the beat. Corrected notes
   are pulled toward the 1/16 grid (by the style's `quantize` times each note's correction: Hard Tune 50%,
   Trap 35%, Hip-Hop 25%). The note lane shows the blob where it will sound, with a faint outline where it
   was sung.
2. **Hip-Hop** adds light swing: every second 1/16 step sits a little late. A custom style can set more.
3. **Shift + drag** a blob left or right to move that note in time. It snaps to 1/16; hold **Alt** too for
   free movement. Right-click > **Reset timing** puts it back.
4. Each note keeps its length and moves as a block; the gaps around it stretch or squeeze (up to 4x either
   way). Pitch, vibrato and formants are kept. Audio away from moved notes is untouched.
5. Right-click a blob > **Split note here** cuts it where you clicked (each half gets its own pitch);
   **Merge with next note** joins it with the following blob.

### 11b. Graph editing: selection, snap to scale, undo

The Notes lane is the note graph; it now takes half the timeline height and has a tool row above it.

1. **Select**: click a blob to select it (white outline). **Ctrl/Cmd-click** or **Shift-click** adds or removes
   notes; **drag on empty graph space** for a box selection (hold Shift/Ctrl to add to it); **Ctrl+A** selects
   everything, **Esc** clears. Right-click a blob > **Select all notes at this pitch**.
2. **Move together**: drag any selected blob and the whole selection moves (up/down in pitch, **Shift + drag** in
   time, snapping to 1/16; hold Alt for free moves).
3. **Snap to scale** (tool row, on by default, saved with the project): pitch moves land on the notes of the key
   and the style's scale (shown at the right, e.g. "A minor, pentatonic"; scale rows are shaded in the graph).
   Off: semitone steps. Alt always moves freely in cents. Right-click > **Snap to scale** or **S** puts selected
   notes onto the nearest scale note.
4. **Correct / Vibrato / Drift sliders** edit the selection: Correct = how far notes are pulled (0-100%);
   Vibrato = vibrato kept (0% flat line, 100% as the style decides, 200% double); Drift = slow pitch wander kept
   inside the note (0% dead steady, 100% as sung; far left = Auto, set by Correct). Notes with custom vibrato or
   drift show a small "v0" / "d50" tag. The right-click menu has the same options for the selection.
5. **Undo / Redo**: buttons, or **Ctrl+Z** and **Ctrl+Shift+Z / Ctrl+Y**. Covers drags, sliders, menu edits,
   split and merge. Undo only puts back what you edited, so a key or style change made since is kept.
6. **Keys** (click in the graph first so it has focus): **Up/Down** = one scale step (semitone with Snap off),
   **Shift+Up/Down** = octave, **Alt+Up/Down** = 5 cents, **Left/Right** = move 1/16 in time,
   **Enter** = correct 100%, **Delete/Backspace** = leave as sung (0%). If FL Studio plays notes instead, turn off
   "Typing keyboard to piano roll" in FL's Options menu.

### 12. Intent

**Intent** (next to Correct, on by default) reads what each note was meant to be before correcting it:

| Read as | What happens |
| --- | --- |
| Slide (pitch glides through the note) | 40% of Correct, so the glide stays |
| Vibrato on a held note | Corrected, and the singer's own vibrato is kept even on flatter styles (not on Hard Tune) |
| Scoop into a note | 80% of Correct |
| Passing note (short, between two others) | 50% of Correct, and it keeps the semitone it was sung on |
| Short note sung cleanly on a semitone outside the key (a run, a quick turn) | Stays on that semitone |
| Chosen accidental (held, steady, on a semitone outside the key) | Stays on that semitone instead of being pulled into the key |
| Spoken or rapped line | Left as sung |
| Ad-lib (much quieter than the take) | 60% of Correct |
| Screamed or growled (technique classifier sure of it) | 60% of Correct, so a scream doesn't warble. Some rasp is marked "gritty" and corrected as usual |
| Breathy or belted (technique classifier) | Marked only; corrected as usual |
| Spoken or rapped (technique classifier) | Marked only; pitch evidence (above) decides whether it's corrected |

1. Import or record a take sung with some slides, a blue note and a spoken line. The status bar lists what
   was found ("12 notes. Kept as sung or corrected gently: 2 slides, 1 chosen accidental, ...").
2. Right-click a note: the menu says how it was read ("Sung as: Slide (40% of Correct)").
3. Turn **Intent** off to compare: every note is pulled to the key the same way.
4. Notes you edit by hand are never changed by Intent.

### 12a. Neural pitch and technique detection

Pitch and technique are read by two neural networks on a background thread when the build has ONNX
Runtime (the Windows build does). Without them the DSP tracker (YIN) and the pitch-curve rules work alone,
as before.

- **Pitch: FCPE** (torchfcpe, MIT licence). It finds the note on gritty, breathy and reverberant takes where
  YIN slips an octave or hears pitch in noise. YIN still refines each frame where the two agree, so clean
  notes keep YIN's precision. `pitch-fcpe.onnx` (43 MB) ships inside the VST3 bundle; it can also live in
  `Documents\RainWorks\Engineer\Models`. A 3 minute vocal takes about 3 seconds on the CPU.
- **Technique: a small classifier** (embedded, 1 MB) trained in `tools/technique-model` on VocalSet and the
  Extreme Metal Vocals Dataset (CC BY 4.0) plus CMU ARCTIC speech (CC0). Every 10 ms it rates vibrato,
  breathy, belt, grit and spoken/rapped. Intent uses it (table above), and **Learn style from vocals...** turns
  how gritty and airy the references are into the style's tone (grit, breath).

Measured on vocadito (40 solo vocals with reference pitch, CC BY 4.0), as raw pitch accuracy (frames within
50 cents of the true pitch) and octave errors:

| | DSP (before) | Neural |
| --- | --- | --- |
| Clean takes | 96.8%, 1.9% octave errors | 97.7%, 0.1% |
| Room reverb | 89.4%, 3.2% | 92.7%, 0.0% |
| Noise floor (15 dB SNR) | 95.2%, 2.3% | 97.4%, 0.1% |
| Notes heard where nothing is sung (clean) | 21.6% of silent frames | 11.0% |

Notes found correctly (onset within 50 ms, pitch within 50 cents of vocadito's note annotations) go from
67.9% to 69.9% on clean takes and from 58.8% to 65.7% with reverb, mostly because far fewer phantom notes
are made from breaths and room tails.

The final tuning is about the same as before: rendered with Pop and Intent on, 98-99% of held notes land
within 25 cents either way (DSP 99.0-99.3%, neural 98.4-99.0% across clean, reverb and noisy copies), with
a slightly tighter median (1.2-1.5 cents against 1.3-1.6). The DSP path was already near the ceiling on
those takes; the neural path's gain is that it is wrong less often about *what* was sung.

`tools/pitch-models/eval_pitch.py` reproduces the tracker numbers; the Windows CI build runs it on every push.

1. Import a take. The status bar says "Pitch analysis: Neural pitch (FCPE) + technique detection".
   If it says "DSP pitch tracker (...)", the reason is in the brackets.
2. Right-click a screamed or breathy note: "Sung as: Gritty (60% of Correct)".

### 12b. Voice models

Voice models re-sing each take in a trained voice, following the notes, style and timing.
Training a model is covered in [tools/voice-training/README.md](../tools/voice-training/README.md)
(RTX 3060 12 GB: about 1 to 2 hours per voice).

1. Export a voice (that guide, step 4). It lands in `Documents\RainWorks\Engineer\Voices\<name>`.
2. **Voice** menu > **Rescan voices folder**, then pick the voice. The status bar says when it starts.
3. With Monitor on **Rendered**, takes play in the new voice once each render finishes. The old
   render keeps playing until then.
4. Pitch edits, styles and timing still apply: the model sings the planned pitch.
5. **As recorded** returns to the original voice. Save and reopen: the voice choice is kept.

If ONNX Runtime can't load, the Voice menu is greyed out and its tooltip says why.

**GPU.** Voice models render on the GPU through DirectML (any DirectX 12 GPU, including the RTX 3060).
The Voice menu's **Render on GPU** item switches between GPU and CPU. When a render finishes, the status
bar says where it ran, for example "on GPU (DirectML)". If the GPU can't be used, it says "CPU (GPU failed: ...)"
and the render still completes on the CPU.

## Known limitations

- Takes are aligned by sample position. If you change the project tempo after recording, takes stay at their
  original time, not their original bar.
- Takes load fully into RAM (about 23 MB per stereo minute at 48 kHz). Streaming comes later.
- Standalone has no host transport, so capture and Rendered playback do nothing there.
- The session id is copied when you "Save as" an FL project, so both projects share one data folder.
- No crossfades at take boundaries yet (punch-ins can click).
- Moving a note only re-times the gaps either side of it, so pulling a note past its neighbour is limited
  (gaps squeeze to a quarter of their length at most). Very long gaps of breath noise can sound grainy when
  stretched a lot.
- Quantize uses the tempo recorded with the take; it ignores tempo changes made after recording.
- The content encoder isn't bundled yet: export it once with the training tools.
- Voice rendering has only been tested with stand-in models; no real trained voice has been
  run through the plugin yet.
- Heavily breathy or polyphonic material confuses the pitch tracker (it assumes one voice).
