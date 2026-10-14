/* ALIGNMENT - stage rendering. */
#ifndef AL_RENDER_H
#define AL_RENDER_H
#include <stdint.h>
#include <gb/gb.h>
#include "../core/stage.h"

extern uint8_t render_checked;  /* the checkpoint has been reached (its pad is lit) */

/* build the whole stage's tile map into cartridge RAM (once per stage) */
void render_prebuild(const Stage *S) BANKED;
/* queue stage column cx for the BG map (written in VBlank) */
void render_column(const Stage *S, uint8_t cx) BANKED;
/* redraw one cell now (energy cell taken, checkpoint lit) */
void render_cell(const Stage *S, uint8_t cx, uint8_t cy) BANKED;

#endif
