/* ALIGNMENT - a gravity puzzler for Game Boy / Game Boy Color.
 * Bank 0 entry point: hardware set-up, then the (banked) game. */
#include <gb/gb.h>
#include "gfx.h"
#include "sound.h"

void game_main(void) BANKED;

void main(void)
{
    gfx_init();
    sound_init();
    enable_interrupts();
    game_main();
}
