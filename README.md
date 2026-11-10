# 32X MIDI Controller

A GenMDM front end for the **Sega 32X** with a **Mega EverDrive PRO**: your
GenMDM interface stays in controller port 2, and the 32X replaces the GenMDM
cartridge with a full on-screen instrument editor.

- **Patch library:** every `.TFI`, `.DMP` and `.Y12` file in the SD card's
  `MIDI_PATCHES` folder, and in folders inside it (as deep as you like: A
  opens a folder, B goes back up). `MIDI_PATCHES` is made on the first save
  if it is missing; new files go into the folder you are looking at. Choosing one loads it into the target channel (1-6, or all six) at once.
- **FM editor:** algorithm, feedback, pan, AMS/FMS, the LFO and every operator
  setting, with the algorithm drawn as a diagram and each operator's envelope
  as a graph. Changes are heard immediately.
- **Live view:** what the GenMDM (or a DAW behind it) changes shows on screen.
- **Save and delete** on the SD card: overwrite, or a new `.TFI`, `.DMP` or
  `.Y12` file (`newpatch001.tfi` ...).
- **A guide built in:** START on the home screen, a page per topic, with the
  pad drawn and the button to try lit up.
- Works with the GenMDM's **original Teensy firmware**, or with **our
  firmware** (`release/midi32x.hex`), which sends the MIDI itself to the
  console: then the console plays it - GenMDM's whole CC map, plus POLY mode,
  velocity, the sustain pedal, stereo spread - and nothing is ever lost.

> Status: in use on a console; new features are tested in the PicoDrive emulator first.

**Making your own sounds:** see [How to make a patch](#how-to-make-a-patch-tfi)
below, and the interactive [FM Patch Lab](docs/patch-lab.html).

**No 32X?** `release/midimd.bin` is the same program for a Genesis / Mega
Drive alone (with the Mega EverDrive PRO): the library, the editor, saving,
the guide, the settings and the MIDI monitor, with the same Teensy firmware
(or GenMDM's own). Without the 32X there is no 32X sound (MIDI channels 11-16
play nothing), and the screen redraws more slowly (about 0.1-0.2 s), since the
Genesis's 68000 draws it as well as running the sound and the port.

Sample patches: `release/sample_patches.zip` (42 FM patches in folders, 17 PSG
instruments in `PSG` and `PSG Noise`, plus 168
in `Variations`; unzip it at
the top of the SD card, it makes `MIDI_PATCHES/Samples`), made by
`tools/make_sample_patches.py` from classic YM2612 recipes.

## Setting up

1. GenMDM interface in controller port 2, a pad (6 buttons is best) in port 1.
2. `midi32x.32x` on the EverDrive's SD card, anywhere.
3. Patch files (`.tfi`, `.dmp`, `.y12`) in a folder `MIDI_PATCHES` at the top
   of the SD card.
4. MIDI into the GenMDM as always (USB from a computer, or its MIDI jack).

The top right corner says `GENMDM: STOCK` (its own firmware) or
`GENMDM: MIDI` (ours) once it hears the interface.

## PSG instruments (MIDI channels 7-10, our firmware)

The PSG (three square-wave voices and the noise) can play **DefleMask
`.dmp` "standard" instruments** (Genesis or Master System): a volume
sequence, an arpeggio (or fixed notes) and, for the noise, white/periodic
steps, each stepped once a frame while a note sounds, with loops. On the
home screen, C until the target says `PSG 1`, `PSG 2`, `PSG 3` or `NOISE`
(MIDI channels 7, 8, 9, 10), then choose a `.dmp` PSG instrument: the panel
draws its sequences, X hears it. Without one a PSG note is a plain tone at
the key's volume, as before. 17 samples are in `sample_patches.zip`
(`Samples/PSG`, `Samples/PSG Noise`).

**The PSG editor**: A on the chosen PSG instrument. Each sequence is a bar
graph, a step a frame: LEFT/RIGHT the step, UP/DOWN its value, A the next
sequence (volume, arpeggio, noise), X / Z a step more / less, Y the loop at
this step, START more (relative / fixed arpeggio, save over the file or as a
new `.dmp`), B back. Every change is heard at once (hold X).

## Loudness

Patches from games are often saved quieter than full (the game set the
level). With **Settings, LOUDNESS: FULL** (the default) a patch loads with
its loudest carrier at full volume and the others kept in proportion; `AS
SAVED` loads it exactly as the file has it. (Saving a patch saves what you
hear.)

`release/vaporwave_metal_y12.zip`: 49 `.y12` patches picked for a synthy
vaporwave and a metal sound from the games known for them (Thunder Force
IV, Alien Soldier, Mega Turrican, Contra Hard Corps, Elemental Master,
Streets of Rage 1-3, Sonic 1-3, Beyond Oasis, Ecco, Phantasy Star IV ...),
taken from vitaflo's genesis-game-instruments archive, sorted by how they
sound, at full volume.

## Our Teensy firmware (optional)

With GenMDM's own firmware the Teensy decides how MIDI plays; with ours the
console does, which brings:

- **GENMDM mode** (default): MIDI channels 1-6 play FM channels 1-6, 7-9 the
  PSG, 10 the noise; every GenMDM CC works as before (DAW projects, the
  GenMDM editor, the Max for Live devices).
- **POLY mode**: one MIDI channel plays all six FM channels, like GENajam:
  the oldest note gives way, sustain pedal, stereo spread, mod wheel = LFO.
- **Velocity** (a piano-like curve), the **sustain pedal**, pitch bend,
  NTSC/PAL tuning: START on the home screen, Settings.
- **A handshake**: every MIDI byte arrives, even while the SD card is busy.
- **The 32X's own sound (PWM)** on MIDI channels 11-16: **software FM** on
  11-14 (6 more voices playing the same patch files) and a **drum kit** from
  WAV files on 16 (below).

## The 32X's FM voices (MIDI channels 11-14, our firmware)

Six more FM voices, computed by the 32X's second SH-2 the way the YM2612
works (its eight algorithms, feedback, its envelopes and rates, TL, MUL,
detune) and mixed into the 32X's PWM output. Each of channels 11-14 has its
own patch and plays chords (the six voices are shared). On the home screen,
C until the target says `32X 11` ... `32X 14`: choosing a patch loads it
there, A edits it, START saves it, X plays middle C. Velocity, pitch bend,
CC 7, CC 10 and the sustain pedal work. Not yet: SSG-EG and the LFO on these
voices. They sound close to the YM2612, but cleaner (the 32X has no 9-bit
DAC).

## Drums (MIDI channel 16, our firmware)

Put WAV drum samples (8 or 16 bit, mono or stereo, any sample rate) in a
folder `MIDI_DRUMS` at the top of the SD card. Each file gets a note:

- a name starting with a number uses that note: `36 Kick.wav`, `038 snare.wav`;
- otherwise words in the name pick the General MIDI drum note: kick 36,
  rim 37, snare 38, clap 39, closed hat 42, pedal hat 44, open hat 46, toms
  45/47/50, crash 49, ride 51, tambourine 54, cowbell 56, shaker 70, ...;
- otherwise the next free note from 60.

A closed or pedal hi-hat stops an open one. Velocity, CC 7 (volume) and
CC 10 (pan) work; Settings has the 32X's volume. On the home screen, C
until the target says DRUMS lists the kit, and X plays each sample in turn.
The samples load into the EverDrive's cartridge memory (up to about 3 MB in
all).

To flash it: install Teensy Loader (pjrc.com/teensy/loader.html), plug the
GenMDM's USB in, open `release/midi32x.hex`, turn on Auto, press the Teensy's
button. To go back, do the same with GenMDM's own `GENMDM_103.cpp.hex`. The
Teensy then shows up as "GenMDM 32X" on your computer.

The firmware's source is `teensy/midi32x.c` (`make -C teensy`, needs
avr-gcc); it uses PJRC's USB MIDI code (MIT).

## Controls

| Button | Library | Editor |
|---|---|---|
| D-pad | choose a patch (it loads); left/right a page | choose a setting; while changing one: up/down by 1, left/right by 10 |
| A | choose the patch now (green mark); on the chosen one: edit it | change the setting / done |
| B | up a folder (3-button pad, top folder: the controls) | back to the library (while changing: done) |
| C | target: CH 1-6, ALL, PSG 1-3, NOISE (and the 32X voices) | next channel |
| X | hear it: middle C while held | value up by 10 |
| Z | | value down by 10 |
| Y | delete the patch file | back to the library |
| START | menu: the guide, settings, look again | save |
| MODE | the controls panel | the controls panel |

## How to make a patch (TFI)

A full guide to every knob of the Mega Drive's FM chip (the YM2612), and how
to turn them into the sound you want.

**Interactive version:** [`docs/patch-lab.html`](docs/patch-lab.html) is the
same guide with a working FM synth beside it. Every section has *Try it*
buttons that set the knobs and play notes; you can play it from a keyboard
on screen or your computer's keys, open your `.tfi` / `.dmp` / `.y12` files,
shape them, and save the result. Download the file and open it in a browser.
On the console, the editor also shows two lines about whichever setting the
cursor is on.

### The idea in one paragraph

Each of the six FM channels plays a voice made of four **operators** (OP1 to
OP4). An operator is a sine wave with its own volume envelope. A **carrier**
is heard: it goes to the speakers. A **modulator** is not heard: it bends the
pitch of the operator it feeds so fast that the wave changes shape, adding
overtones. A louder modulator gives a brighter sound. Making a patch is
choosing the wiring (ALG), each operator's pitch ratio (MUL), loudness (TL)
and envelope, then listening and adjusting. In the cartridge's editor the
carriers are orange and the modulators green.

### Doing it on the console

1. On the home screen, choose a patch to start from (a sample patch, or any
   file), with the target set to the channel you play (C changes it).
2. Press **A** on the chosen patch to open the editor. Play your keyboard
   while you edit: every change is heard at once.
3. The D-pad picks a knob. **A** starts changing it: up/down by 1, left/right
   by 10, A or B when done. X / Z jump by 10 at once on a 6-button pad. C
   moves to the next channel.
4. **START** saves: over the file, or as a new `.tfi`, `.dmp` or `.y12` in
   the folder you are in (`newpatch001.tfi` ...). **B** goes back.

### ALG, the algorithm (0-7)

Which operators feed which. The ones at the end of a chain are carriers.

| ALG | Wiring | Carriers | Good for |
|---|---|---|---|
| 0 | 1 → 2 → 3 → 4 | OP4 | The harshest: distorted guitar, growling bass, metallic leads |
| 1 | (1 + 2) → 3 → 4 | OP4 | Guitars and basses with two sources of grit |
| 2 | (1 + (2 → 3)) → 4 | OP4 | Basses, plucked and struck sounds, clavinet |
| 3 | ((1 → 2) + 3) → 4 | OP4 | Brass, bass, guitar |
| 4 | (1 → 2) + (3 → 4) | OP2, OP4 | Two independent pairs: electric piano, brass, strings, layers |
| 5 | 1 → (2, 3, 4) | OP2, OP3, OP4 | Thick leads, organs, choirs |
| 6 | (1 → 2) + 3 + 4 | OP2, OP3, OP4 | A bright pair plus two sines: organs, bells with body |
| 7 | 1 + 2 + 3 + 4 | all four | No FM (but OP1's feedback): drawbar organ, additive sounds |

### TL, total level (0-127, 0 = loudest, 0.75 dB a step)

- On a **carrier**, TL is volume. Carriers usually sit at 0-10. Every 8 steps
  is 6 dB (about half as loud). A patch that is too quiet has carriers set
  too high; the cartridge's LOUDNESS setting (on by default) brings the
  loudest carrier to 0 when a patch loads.
- On a **modulator**, TL is brightness. Modulators usually sit at 20-45:
  below 20 the sound turns buzzy and noisy, above 60 a modulator does almost
  nothing. Turning a modulator's TL down (towards 0) is the first move to
  brighten a sound; up, to darken it.

### MUL, the multiplier (0-15, 0 = half)

The operator's frequency as a multiple of the note. On a carrier it moves the
pitch (2 = an octave up, 3 = an octave and a fifth); on a modulator it picks
the overtones.

| Modulator : carrier | Sound |
|---|---|
| 1 : 1 | Every harmonic, like a sawtooth: brass, strings, leads |
| 2 : 1 | Odd harmonics, like a square: hollow, clarinet, organ |
| 3 : 1, 4 : 1 | Nasal, reedy |
| 1 : 2, 1 : 3 | The carrier above the modulator: thin, vocal |
| 7, 9, 11, 13, 14 : 1 | Inharmonic: bells, chimes, metal, electric piano tines |
| 0 (half) | An octave down: sub bass, or body under a modulator |

### FB, feedback (0-7, OP1 only)

OP1 feeds its own output back into its pitch. 1-3 turns it into a saw-like
wave (harmonics without another operator), 4-5 rough, 6-7 gritty and noisy:
distorted guitars, growling basses, snares and cymbals. It is heard only
through OP1: as a carrier (ALG 7) or through what OP1 modulates.

### DT, detune (-3 to +3)

A tiny pitch offset, larger on high notes. Two carriers at the same MUL with
opposite DT (+2 and -2, or +3 and -3) beat against each other: the classic
chorus that thickens pads, strings, electric pianos and supersaw leads. On a
modulator, DT makes the tone move slowly.

### The envelope: AR, D1R, SL, D2R, RR

Every operator has one. On carriers it is the volume over time; on modulators
it is the brightness over time (a modulator that dies faster than its carrier
is the "filter closing" of a pluck or a piano).

| Knob | Range | What it does |
|---|---|---|
| AR | 0-31 | Attack rate. 31 instant, 20-26 soft, 10-16 slow swell. **0 never starts: silence** |
| D1R | 0-31 | First decay: from the peak down to SL. 0 = no fall |
| SL | 0-15 | Sustain level, 3 dB a step. 0 = stay at the peak; 15 = all the way down |
| D2R | 0-31 | Second decay, while the key is held. 0 = hold forever; 1-5 = a slow piano-like fade |
| RR | 0-15 | Release after the key comes up. 15 = cut off; 7-9 = short tail; 2-5 = long ring |
| RS | 0-3 | Rate scaling: high notes get faster envelopes. 1-2 for piano, guitar, mallets |

Shapes to start from: organ AR 31 D1R 0 SL 0 D2R 0 RR 8 · pluck AR 31 D1R
12 SL 15 RR 8 · piano AR 31 D1R 6 SL 4 D2R 3 RR 7 (RS 2) · pad AR 12 D1R 2
SL 2 D2R 0 RR 4.

### LFO, AMS, FMS, AM: vibrato and tremolo

- **LFO**: one oscillator for the whole chip: off, or 8 speeds (3.98, 5.56,
  6.02, 6.37, 6.88, 9.63, 48.1, 72.2 Hz). Changing it changes every channel.
- **FMS** (0-7): vibrato depth, ±3.4 up to ±80 cents. 2-3 sounds natural.
- **AMS** (0-3): tremolo depth (0, 1.4, 5.9, 11.8 dB), applied only to the
  operators whose **AM** switch is on. AM on a carrier wobbles the volume, on
  a modulator the brightness (wah-like).

### SSG-EG (off, or shapes 1-8)

Makes an operator's envelope loop or flip instead of ending: buzzing,
throbbing and sweeping effects. It uses D1R, SL and D2R. Shapes as numbered in
the editor: 1 `\\\\` (decay, repeat), 2 `\___` (once, then silence), 3
`\/\/`, 4 `\‾‾‾`, 5 `////`, 6 `/‾‾‾`, 7 `/\/\`, 8 `/___`. Emulators disagree
about SSG-EG more than anything else, so check it on the console.

### PAN

Left, right, both or off, per channel. Not stored in `.tfi` or `.dmp` files
(it belongs to the channel, not the instrument); in POLY mode, STEREO SPREAD
pans each note for you.

### Recipes

ALG, FB, then each operator as MUL DT TL RS AR D1R SL D2R RR:

| Sound | ALG | FB | OP1 | OP2 | OP3 | OP4 |
|---|---|---|---|---|---|---|
| E.Piano | 4 | 0 | 1 0 30 2 31 9 4 2 6 | 1 0 4 1 31 7 3 2 6 | 14 0 42 2 31 14 6 4 7 | 1 0 4 1 31 6 3 2 6 |
| Finger bass | 0 | 5 | 0 0 28 1 31 12 3 3 8 | 1 0 38 1 31 14 4 3 8 | 1 1 30 1 31 10 3 2 8 | 1 0 0 1 31 6 2 4 9 |
| Brass | 4 | 6 | 1 0 24 1 18 4 2 2 7 | 1 0 4 1 22 3 1 1 7 | 1 1 26 1 18 4 2 2 7 | 1 -1 6 1 22 3 1 1 7 |
| Strings | 4 | 3 | 1 1 30 0 14 2 1 0 6 | 1 0 8 0 14 2 1 0 6 | 2 -1 34 0 14 2 1 0 6 | 1 -1 8 0 14 2 1 0 6 |
| Drawbar organ | 7 | 0 | 1 0 14 0 31 0 0 0 10 | 2 0 16 0 31 0 0 0 10 | 4 0 22 0 31 0 0 0 10 | 0 0 14 0 31 0 0 0 10 |
| Tubular bell | 4 | 2 | 7 0 30 2 31 6 5 2 4 | 1 0 6 1 31 5 4 2 4 | 11 1 34 2 31 7 5 2 4 | 2 0 10 1 31 5 4 2 4 |
| Saw lead | 0 | 7 | 1 0 20 1 31 2 1 0 9 | 1 0 24 1 31 2 1 0 9 | 1 1 26 1 31 2 1 0 9 | 1 0 2 1 31 2 1 0 9 |
| Supersaw | 7 | 4 | 1 3 10 0 30 1 1 0 7 | 1 1 10 0 30 1 1 0 7 | 1 -1 10 0 30 1 1 0 7 | 1 -3 10 0 30 1 1 0 7 |
| Metal guitar | 1 | 7 | 1 0 26 1 31 2 1 0 7 | 3 0 34 1 31 3 2 0 7 | 1 0 22 1 31 1 1 0 7 | 1 0 0 1 31 2 2 1 8 |
| Kick | 0 | 4 | 0 0 16 3 31 18 10 10 12 | 1 0 30 3 31 18 10 10 12 | 0 0 26 3 31 16 10 10 12 | 0 0 0 3 31 14 9 10 12 |

More in `sample_patches.zip` and `vaporwave_metal_y12.zip`.

### Tuning by ear

| It sounds... | Try |
|---|---|
| Dull, muffled | Lower a modulator's TL 5-10 steps; raise its MUL; add FB 2-4 |
| Harsh, buzzy | Raise the modulators' TL; lower FB; lower high modulator MULs |
| Too quiet | Carriers' TL towards 0 (or LOUDNESS: FULL in the settings) |
| Thin | More carriers (ALG 4-7), detuned (+2 / -2); a carrier at MUL 0 or 1 for body |
| Clicks at the start | Carriers' AR a little lower (28-29) |
| Clicks or cuts at the end | Lower RR (7-9) |
| Rings on, smears | Raise RR, raise D2R, add RS 1-2 |
| Static, lifeless | A modulator that decays faster than its carrier (D1R up, SL up); a little FMS |
| Silent | A carrier with AR 0 or TL 127, or SL 15 with a fast D1R; PAN off |
| Wants metal grit | ALG 0-3, FB 6-7, modulators at TL 20-30, the carrier sustained (SL 0-2, D2R 0-1) |
| Wants vaporwave lush | ALG 4-5, detuned carriers, AR 14-20, RR 3-5, a touch of vibrato (FMS 2) |

### The files

| Format | From | Stores | Leaves out |
|---|---|---|---|
| `.tfi` | TFM Music Maker | ALG, FB; per operator MUL, DT, TL, RS, AR, D1R, D2R, RR, SL, SSG | AMS, FMS, AM, PAN, LFO |
| `.dmp` | DefleMask | All of TFI, plus AMS, FMS, AM | PAN, LFO |
| `.y12` | Gens | The chip's registers including AM, a 15-letter name | AMS, FMS, PAN, LFO |

So a patch that depends on vibrato or tremolo is best saved as `.dmp`.

A `.tfi` is 42 bytes: ALG, FB, then four blocks of ten bytes in the chip's
order **OP1, OP3, OP2, OP4**, each MUL, DT, TL, RS, AR, D1R, D2R, RR, SL,
SSG. DT is stored 0-6 (3 = none, 0-2 = -3..-1, 4-6 = +1..+3); SSG is 0 (off)
or 8-15 (shapes 1-8).

## Building

```bash
make            # rom/midi32x.32x and rom/midimd.bin (the Genesis alone)
make check      # ROM checks; host tests (patch formats, the MIDI engine,
                # the Teensy link simulated); emulator tests
make -C teensy  # teensy/midi32x.hex
```

Needs `sh-elf` and `m68k-elf` GCC in `/opt/toolchains/sega` (as for SegaTube).

## Credits

- The GenMDM port protocol was learned from MiGeRA's commented disassembly of
  the GenMDM cartridge (github.com/MiGeRA/genMDM-newbuild); the code here is
  new.
- File formats: TFI (TFM Music Maker), DMP (DefleMask), Y12 (Gens).
- The EverDrive I/O follows krikzz's mega-ed-pub (MIT).
