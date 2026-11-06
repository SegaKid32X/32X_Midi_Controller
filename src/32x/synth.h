/* synth.h - the 32X's own voices through the PWM (synth.c) */
#ifndef SYNTH_H
#define SYNTH_H
#define MAX_SAMPLES 64
extern char synth_names[MAX_SAMPLES][24];
extern int synth_count;
void synth_start(void);                 /* the PWM and the slave's mixer */
void synth_poll(void);                  /* fetch the 68000's events (call often) */
int  synth_load_drums(const char *kit); /* the WAVs in MIDI_DRUMS (kit: a folder in it, or 0) */
int  synth_note_of(int i);
void synth_set_volume(int v);           /* 0-10 */
#endif
