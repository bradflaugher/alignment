/* ALIGNMENT - the mech's physics, the enemies and the hazards. Portable: the Game Boy
 * runs it every frame, the host tests run it to prove stages can really be finished. */
#ifndef AL_PHYS_H
#define AL_PHYS_H
#include <stdint.h>
#include "stage.h"

/* positions are in 1/16 px; the mech's box is the 16x16 sprite, its hitbox is
 * HB_W x HB_H inside it */
#define SUB 16
#define HB_X 3
#define HB_W 10
#define HB_Y 1
#define HB_H 14

/* tuning (1/16 px per frame, per frame squared) */
#define RUN_MAX 28      /* 1.75 px/frame */
#define RUN_ACC 4
#define RUN_DEC 6
#define AIR_ACC 3
#define GRAV 6          /* 0.375 px/frame^2 */
#define FALL_MAX 72     /* 4.5 px/frame */
#define JUMP_V 76       /* a 30 px jump */
#define FLIP_KICK 8     /* the first nudge when gravity turns */
#define COYOTE 5        /* frames you can still jump after running off a ledge */

/* input bits */
#define IN_L 0x01
#define IN_R 0x02
#define IN_JUMP 0x04        /* held */
#define IN_JUMP_NEW 0x08    /* just pressed */
#define IN_FLIP_NEW 0x10    /* just pressed */
#define IN_FIRE 0x20        /* held (the cannon repeats) */

/* events of one step */
#define EV_LAND 0x0001
#define EV_JUMP 0x0002
#define EV_FLIP 0x0004
#define EV_NOFLIP 0x0008    /* a flip refused (no air flip left, or inside a phase block) */
#define EV_DEAD 0x0010
#define EV_CELL 0x0020      /* picked up an energy cell (ev_cx, ev_cy) */
#define EV_CHECK 0x0040     /* touched the checkpoint */
#define EV_EXIT 0x0080      /* reached the exit gate */
#define EV_STEP 0x0100      /* a footstep (for the sound) */
#define EV_BONK 0x0200      /* hit a ceiling while jumping */
#define EV_ITEM 0x0400      /* picked up a power-up (ev_item, at ev_cx, ev_cy) */
#define EV_SHOOT 0x0800     /* the cannon fired */
#define EV_HURT 0x1000      /* the mech took a hit (and lived) */
#define EV_KILL 0x2000      /* an enemy was destroyed (at pixel ev_kx, ev_ky) */
#define EV_HIT 0x4000       /* an enemy was hit and lived */
#define EV_STOMP 0x8000     /* an enemy was crushed by the mech falling onto it */

enum { DIE_NONE = 0, DIE_PLASMA, DIE_DRONE, DIE_CRUSHER, DIE_LASER, DIE_BOLT, DIE_BOSS };

typedef struct {
    uint16_t x, y;      /* 1/16 px */
    int16_t vx, vy;
    uint8_t g;          /* G_DOWN / G_UP */
    uint8_t ground;     /* standing on something */
    uint8_t airflip;    /* the mid-air flip is still available */
    uint8_t facing;     /* 1 = left */
    uint8_t coyote;
    uint8_t dead;       /* DIE_* */
    uint8_t land_v;     /* speed of the last landing (for effects) */
    uint8_t step_t;
} Mech;

extern uint8_t ev_cx, ev_cy;    /* the cell of an EV_CELL / EV_ITEM */
extern uint8_t ev_item;         /* T_ITEM_* of an EV_ITEM */

void mech_spawn(Mech *m, uint8_t cx, uint8_t cy, uint8_t g) CORE_BANKED;
/* one frame; t is the stage clock (enemies and lasers move with it) */
uint16_t mech_step(Mech *m, Stage *S, uint8_t input, uint16_t t) CORE_BANKED;

uint8_t laser_on(uint16_t t) CORE_BANKED;
/* solid for the mech in gravity g, at pixel (px, py) */
uint8_t solid_px(const Stage *S, uint16_t px, uint8_t py, uint8_t g) CORE_BANKED;

#endif
