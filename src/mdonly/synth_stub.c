/* synth_stub.c - the Genesis-only build has no 32X voices (MIDI 11-16) */
#include "synth.h"

char synth_names[MAX_SAMPLES][24];
int synth_count;

void synth_start(void) {}
void synth_poll(void) {}
int  synth_load_drums(const char *kit) { (void)kit; return 0; }
int  synth_note_of(int i) { (void)i; return 0; }
void synth_set_volume(int v) { (void)v; }
void synth_set_patch(int ch, const fm_patch *p) { (void)ch; (void)p; }
void synth_get_patch(int ch, fm_patch *p) { (void)ch; patch_default(p); }
