# 32X MIDI Controller

A GenMDM front end for the **Sega 32X** with a **Mega EverDrive PRO**: your
GenMDM interface stays in controller port 2, and the 32X replaces the GenMDM
cartridge with a full on-screen instrument editor.

- **Patch library:** every `.TFI`, `.DMP` and `.Y12` file in the SD card's
  `MIDI_PATCHES` folder (made on the first save if it is missing). Choosing one loads it into the target channel (1-6, or all six) at once.
- **FM editor:** algorithm, feedback, pan, AMS/FMS, the LFO and every operator
  setting, with the algorithm drawn as a diagram and each operator's envelope
  as a graph. Changes are heard immediately.
- **Live view:** what the GenMDM (or a DAW behind it) changes shows on screen.
- **Save and delete** on the SD card: overwrite, or a new `.TFI`, `.DMP` or
  `.Y12` file (`newpatch001.tfi` ...).
- **A guide built in:** START on the home screen, a page per topic, with the
  pad drawn and the button to try lit up.
- Works with the GenMDM's **original Teensy firmware**. Replacement firmware
  that sends raw MIDI (for POLY mode, the 32X's extra PWM voices and drums) is
  in progress (`teensy/`).

> Status: works in the PicoDrive emulator; not yet tested on a console.

## Controls

| Button | Library | Editor |
|---|---|---|
| D-pad | choose a patch (it loads); left/right a page | choose a setting |
| A | edit the target channel's voice | value up |
| B | (3-button pad: the controls panel) | value down |
| C | target channel 1-6 / ALL | next channel |
| X / Z | | value up / down by 10 |
| Y | delete the patch file | back to the library |
| START | the guide | save |
| MODE | the controls panel | the controls panel |

## Building

```bash
make            # rom/midi32x.32x
make check      # ROM checks, patch-format tests, emulator tests
```

Needs `sh-elf` and `m68k-elf` GCC in `/opt/toolchains/sega` (as for SegaTube).

## Credits

- The GenMDM port protocol was learned from MiGeRA's commented disassembly of
  the GenMDM cartridge (github.com/MiGeRA/genMDM-newbuild); the code here is
  new.
- File formats: TFI (TFM Music Maker), DMP (DefleMask), Y12 (Gens).
- The EverDrive I/O follows krikzz's mega-ed-pub (MIT).
