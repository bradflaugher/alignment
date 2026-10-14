/* ALIGNMENT - turning a room into tiles. */
#ifndef AL_RENDER_H
#define AL_RENDER_H
#include <stdint.h>
#include "../core/room.h"
#include "assets.h"

#define ROOM_TW 20      /* a room is 20 x 18 tiles */
#define ROOM_TH 18

extern uint8_t r_wall_tile[];
extern uint8_t r_mt_tiles[];
extern uint8_t r_mt_pal[];

/* the BG map tile where the current room's top-left corner is (wraps at 32) */
extern uint8_t room_ox, room_oy;
/* crates being animated as sprites are left out of the BG (bit per crate slot) */
extern uint8_t render_hide;
extern uint8_t rbuf_t[ROOM_TW * ROOM_TH];
extern uint8_t rbuf_a[ROOM_TW * ROOM_TH];

/* build the whole room into rbuf_t / rbuf_a */
void render_build(const Room *R, const State *s) BANKED;
/* copy parts of the buffer to the map at origin (ox, oy) */
void render_blit_all(uint8_t ox, uint8_t oy) BANKED;
void render_blit_col(uint8_t col, uint8_t ox, uint8_t oy) BANKED;
void render_blit_row(uint8_t row, uint8_t ox, uint8_t oy) BANKED;
/* redraw one cell of the current room (at room_ox, room_oy) */
void render_cell(const Room *R, const State *s, uint8_t c) BANKED;
/* redraw every cell whose look depends on gravity, doors or the exit */
void render_dynamic(const Room *R, const State *s) BANKED;

#endif
