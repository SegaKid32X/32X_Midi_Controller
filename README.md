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

## Setting up

1. GenMDM interface in controller port 2, a pad (6 buttons is best) in port 1.
2. `midi32x.32x` on the EverDrive's SD card, anywhere.
3. Patch files (`.tfi`, `.dmp`, `.y12`) in a folder `MIDI_PATCHES` at the top
   of the SD card.
4. MIDI into the GenMDM as always (USB from a computer, or its MIDI jack).

The top right corner says `GENMDM: STOCK` (its own firmware) or
`GENMDM: MIDI` (ours) once it hears the interface.

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

To flash it: install Teensy Loader (pjrc.com/teensy/loader.html), plug the
GenMDM's USB in, open `release/midi32x.hex`, turn on Auto, press the Teensy's
button. To go back, do the same with GenMDM's own `GENMDM_103.cpp.hex`. The
Teensy then shows up as "GenMDM 32X" on your computer.

The firmware's source is `teensy/midi32x.c` (`make -C teensy`, needs
avr-gcc); it uses PJRC's USB MIDI code (MIT).

## Controls

| Button | Library | Editor |
|---|---|---|
| D-pad | choose a patch (it loads); left/right a page | choose a setting |
| A | edit the target channel's voice | value up |
| B | up a folder (3-button pad, top folder: the controls) | value down |
| C | target channel 1-6 / ALL | next channel |
| X | hear it: middle C while held | value up by 10 |
| Z | | value down by 10 |
| Y | delete the patch file | back to the library |
| START | menu: the guide, settings, look again | save |
| MODE | the controls panel | the controls panel |

## Building

```bash
make            # rom/midi32x.32x
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
