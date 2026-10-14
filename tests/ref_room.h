/* The readable reference implementation of the rules (tests only): the shipped
 * src/core/room.c is a speed-tuned rewrite and must agree with it on every turn. */
#ifndef AL_REF_ROOM_H
#define AL_REF_ROOM_H
#include "room.h"
extern uint8_t r_o[NOBJ];
extern uint8_t r_g, r_got;
extern const uint8_t ref_cell_x[RN];
extern const uint8_t ref_cell_y[RN];
void ref_room_clear(Room *R);
uint8_t ref_room_full(const Room *R);
void ref_sim_load(const Room *R);
uint8_t ref_sim_core(uint8_t act, TurnLog *log);
uint8_t ref_state_start(const Room *R, State *s);
uint8_t ref_sim_turn(const Room *R, State *s, uint8_t act, TurnLog *log);
uint8_t ref_doors_open(const Room *R, const State *s);
uint8_t ref_settle_state(const Room *R, State *s);
#endif
