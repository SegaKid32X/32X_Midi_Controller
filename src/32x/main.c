/* main.c - the 32X MIDI Controller on the master SH-2.
 *
 * The marker string is checked by tests/verify_rom.py: if the linker ever
 * drops the program, the build fails instead of booting to a black screen.
 */
#include "hw.h"
#include "ui.h"
#include "synth.h"

const char midi32x_marker[] __attribute__((used)) = "32X MIDI CONTROLLER";

int main(void)
{
    (void)*(const volatile char *)midi32x_marker;      /* keep it linked */
    hw_init();
    synth_start();
    poll_hook = synth_poll;
    ui_start();
    for (;;) {
        if (!ui_continuous) {                       /* (the visualizer flips, so waits, itself) */
            while (hw_in_vblank()) synth_poll();    /* (the vblank's start: a frame) */
            while (!hw_in_vblank()) synth_poll();
        }
        ui_frame();
    }
    return 0;
}
