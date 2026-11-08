The newest build, updated automatically whenever the files change.

- **midi32x.32x** - the ROM for the Sega 32X (with a Mega EverDrive PRO).
- **midimd.bin** - the same program for a Genesis / Mega Drive alone (no 32X: MIDI channels 11-16 play nothing).
- **midi32x.hex** - the Teensy firmware for the GenMDM interface, for both ROMs. Flash it with Teensy Loader. GenMDM's original firmware also works.

Put the ROM anywhere on the SD card, and your .tfi / .dmp / .y12 patches in a folder called MIDI_PATCHES at the top of the card.

Controls: B is back (up a folder, or out of the editor). In the editor, A changes a setting (up/down by 1, left/right by 10); A or B finishes.

Status: tested in the PicoDrive emulator; console testing is in progress.
