# Style learning

Engineer's genre styles describe how a genre is **sung**: how close notes sit to pitch, how fast the
voice moves between notes, how much vibrato and when it starts, scoops into notes, and timing. They do
not describe whose voice it is. This folder holds the tool chain that measures those things from
reference vocals and nudges the built-in styles toward what was measured.

The measuring code is the engine's `StyleLearner` (`Source/Engine/Style/StyleLearner.*`). The same code
runs in the plugin (**Style > Learn style from vocals...**) and in the `EngineerStyleLearn` command-line
tool built next to the tests.

## What is measured

| Measurement | How | Feeds |
|---|---|---|
| Distance of note centres from the semitone grid | median over notes ≥150 ms, after removing the take's overall tuning offset | `pitch.correction` |
| Note-to-note move time | 10% to 90% of the pitch move between connected notes | `slides.betweenNotesMs`, `pitch.retuneMs` |
| Pitch spread on held notes without vibrato | std. dev. in cents after the attack | `pitch.keepVariation` |
| Vibrato | autocorrelation of the detrended pitch on notes ≥450 ms (3.5 to 9 Hz); rate, depth, onset, and the share of long notes that have it | `vibrato.*` |
| Scoops | attacks from silence that start ≥35 cents low and rise; depth, length and how often | `slides.scoopCents`, `slides.scoopMs` |
| Timing | onsets against a 16th grid with searched phase, tempo (±1%) and swing; needs a known tempo | `timing.quantize`, `timing.swing` |

Every value is a median, and each group of values is trusted in proportion to how much evidence it has
(number of notes, and number of separate recordings, since one singer's habits are not a genre).

## Data used for the built-in styles

Only openly licensed recordings are used (CC BY 4.0). No audio is committed or shipped: only the
statistics in `manifest.json` and the resulting profiles in `learned/`. `manifest.json` lists every
file analysed, its source page, licence, genre label and the measured numbers.

- **University of Rochester Audio-Visual Solo Singing Performance (URSing) Dataset** by Bochen Li, Yuxuan Wang, Zhiyao Duan. CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). https://zenodo.org/records/6404999 B. Li, Y. Wang, Z. Duan. Audiovisual singing voice separation. TISMIR 4(1), 2021.
- **Larynx Microphone Singer-Songwriter Dataset (LM-SSD)** by Simon Schwär, Michael Krause, Michael Fast, Sebastian Rosenzweig, Frank Scherbaum, Meinard Müller. CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). https://zenodo.org/records/20287765 S. Schwär et al. A Dataset of Larynx Microphone Recordings for Singing Voice Reconstruction. TISMIR 7(1), 2024.
- **AlbumDB: A multitrack dataset of a ten-song album** by Thomas McKenzie, Eloi Moliner, Alec Wright. CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). https://zenodo.org/records/19683001 T. McKenzie, E. Moliner, A. Wright. A multitrack dataset of a ten-song album with stereo and immersive 7.1.4 masters. AES 160th Convention, 2026. Album 'Extraordinary' by Tom McKenzie.
- **vocadito: A dataset of solo vocals with f0, note, and lyric annotations** by Rachel Bittner, Katherine Pasalo, Juan José Bosch, Gabriel Meseguer-Brocal, David Rubinstein. CC BY 4.0 (https://creativecommons.org/licenses/by/4.0/). https://zenodo.org/records/5578807

URSing and LM-SSD performances are covers of popular songs; the genre of each was assigned by us from the
song performed (marked `"genreSource": "ours"` in the manifest). URSing vocals were pitch-refined in
Melodyne by the dataset authors, so they show what a produced pop vocal looks like. vocadito is used only
as an untreated reference and does not tune any style.

### Coverage

The built-ins are three utility modes (Natural, Transparent, Hard Tune) and six genres: Pop, Rock, Indie,
R&B, Hip-Hop and Trap. Reference vocals labelled with a finer genre count toward the core genre they sound
like (`GENRE_FOLD` in `learn_styles.py`): pop ballads, dance-pop, K-pop and musical theatre toward Pop;
singer-songwriter, folk and country toward Indie.

Learning works by **contrast**. Each genre's measurements are compared with all the material pooled
together, and the hand-tuned style (`handtuned.json`) is moved by that difference times `--gain` (2 for
the built-ins), scaled by how much evidence the genre had. What every singer does alike cancels out, so
a genre only moves where its singers really differ from the rest. Each style's `"locked"` list in
`handtuned.json` names what learning may not move: its signature, set by hand. Every genre locks its scale
(always the song's key), its default Correct amount, its tone and its compression. Rock, Hip-Hop and Trap
lock their whole pitch feel too, because the open data has no modern rock, rap or trap singing: the
recordings labelled rock are soft-rock covers (Hey Jude, Yellow, The Scientist). Styles with no data keep
their hand-tuned sound exactly. The two rap recordings in URSing are kept as reference only, since rap pitch
statistics describe speech-like delivery.

| Style | Recordings | What learning moved |
|---|---|---|
| indie | 41 (indie 11, singer-songwriter 20, folk 8, country 2) | retune 45 to 56 ms, glides 40 to 54 ms |
| pop | 37 (pop 14, pop ballad 19, musical theatre 2, dance-pop 1, K-pop 1) | vibrato rate only (pop is close to the pooled average) |
| rock | 6 (thin) | vibrato rate and onset only (rest locked) |
| rnb | 1 (very thin) | vibrato a little |
| hip-hop | 0 | none: hand-tuned |
| trap | 0 | none: hand-tuned |

The hand-tuned values were checked on 21 real lead vocals (3 or 4 per genre) from MUSDB18-HQ, a research-only
dataset that is used for listening tests and measurements only: nothing from it is learned, committed or
shipped. On every test vocal the genre lands 75-100% of held notes within 10 cents and 87-100% within 25 cents
of the target (measured by re-tracking the output), with Trap the flattest line and Indie the loosest.

Timing (`quantize`, `swing`) was not learned for any style: none of these datasets gives the song tempo.

### Gaps and where the data would come from

No openly licensed, downloadable, dry vocals were found for trap, hip-hop hooks, modern rock or
contemporary R&B. ccMixter has about 1,500 CC BY / CC0 acapellas tagged across many of
these genres, but it refuses downloads from scripts, so they were not used. Freesound also has CC0 and
CC BY vocal packs by genre; its API needs a free API key. Either source can be added to `sources.json`
(each item needs its licence and source page) and the pipeline re-run.

## Re-running

```
cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target EngineerStyleLearn
TOOL=build/EngineerStyleLearn_artefacts/Release/EngineerStyleLearn
python tools/style-learning/learn_styles.py fetch   --work ~/style-work
python tools/style-learning/learn_styles.py analyze --work ~/style-work --tool $TOOL
python tools/style-learning/learn_styles.py learn   --work ~/style-work --tool $TOOL --gain 2 --apply
```

`fetch` reads only the needed vocal files out of each Zenodo zip with HTTP range requests (about 3 GB in
all, not the 27 GB of zips) and needs `ffmpeg`. `--apply` regenerates `Source/Engine/Style/BuiltinStyles.cpp` from `handtuned.json` and the learned values. Edit `handtuned.json`, not the generated file.

To learn a single style from your own vocals without the plugin:

```
$TOOL --base rnb --id my-rnb --name "My R&B" --out my-rnb.json take1.wav take2.wav@92
```

`@92` gives that file's tempo so timing can be learned too. Put the JSON in
`Documents/RainWorks/Engineer/Styles/` to use it.

To hear a style on a vocal without the plugin (pitch and expression only, no timing moves):

```
$TOOL --render pop out.wav my-take.wav
```

