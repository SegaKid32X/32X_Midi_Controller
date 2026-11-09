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

> Status: works in the PicoDrive emulator; not yet tested on a console.

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
| C | target channel 1-6 / ALL | next channel |
| X | hear it: middle C while held | value up by 10 |
| Z | | value down by 10 |
| Y | delete the patch file | back to the library |
| START | menu: the guide, settings, look again | save |
| MODE | the controls panel | the controls panel |

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
