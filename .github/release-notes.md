The newest build, updated automatically whenever the files change.

- **midi32x.32x** - the ROM for the Sega 32X (with a Mega EverDrive PRO).
- **midimd.bin** - the same program for a Genesis / Mega Drive alone (no 32X: MIDI channels 11-16 play nothing).
- **midi32x.hex** - the Teensy firmware for the GenMDM interface, for both ROMs. Flash it with Teensy Loader. GenMDM's original firmware also works.
- **sample_patches.zip** - 42 sample .tfi FM patches in folders (bass, keys, organ, brass, strings, leads, bells, plucks, drums, effects), a Variations folder with 168 (each one and three takes on it), and 17 PSG instruments (.dmp) in PSG and PSG Noise. Unzip it at the top of the SD card: it makes MIDI_PATCHES/Samples.

- **vaporwave_metal_y12.zip** - 49 .y12 patches from games known for a synthy vaporwave or a metal sound (guitars, chugs, bass, leads; e-pianos, pads, bells, synth leads, plucks), at full volume. Unzip at the top of the SD card.

Put the ROM anywhere on the SD card, and your .tfi / .dmp / .y12 patches in a folder called MIDI_PATCHES at the top of the card.

Controls: A chooses a patch now (the loaded one has a green mark); A on the chosen one edits it. B is back (up a folder, or out of the editor). In the editor, A changes a setting (up/down by 1, left/right by 10); A or B finishes. C goes through the targets: CH 1-6, ALL, PSG 1-3, NOISE (and the 32X voices). On a PSG target, A on the chosen instrument opens the PSG editor. Settings, LOUDNESS: FULL (the default) loads patches at full volume.

Status: tested in the PicoDrive emulator; console testing is in progress.
