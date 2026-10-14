/* ALIGNMENT - the mech at war: its cannon, its armour, the enemies it shoots or
 * stomps, the turrets' bolts and the bosses. Portable like the physics: the Game Boy
 * plays it, the host bot proves stages and bosses can be beaten with it. */
#ifndef AL_WORLD_H
#define AL_WORLD_H
#include <stdint.h>
#include "stage.h"
#include "phys.h"

#define NSHOT 3
#define HULL_MAX 3
#define INV_FRAMES 64       /* after a hit, frames before the next one counts */
#define FIRE_CD 9           /* frames between shots (power shot: 12) */
#define SHOT_SPEED 4        /* px a frame (power shot: 5) */
#define SHOT_LIFE 40        /* frames */
#define MAX_NEAR 8          /* enemies looked at in a frame */
#define NBOLT 4             /* a boss's bolts in the air */

typedef struct {
    uint16_t x;             /* top-left of the 8x8 sprite, px */
    uint8_t y;
    int8_t vx;
    uint8_t life;           /* frames left; 0 = free */
} Shot;

typedef struct {
    Mech m;
    Shot shot[NSHOT];
    uint8_t hull;           /* hits left */
    uint8_t inv;            /* invulnerable frames left */
    uint8_t shield;         /* soaks the next hit */
    uint8_t power;          /* power shot: 2 damage */
    uint8_t bomb;           /* the next flip sends a shockwave */
    uint8_t cd;             /* cannon cooldown */
    uint8_t strict;         /* every hit is fatal (the host bot plays like this) */
} Player;

typedef struct {
    uint16_t x;             /* top-left of the 8x8 sprite, px */
    uint8_t y;
    int8_t vx, vy;
    uint8_t life;
} Bolt;

typedef struct {
    uint8_t kind, level;    /* BK_*, 1..2 */
    uint8_t active, dead;   /* the fight is on / the boss is destroyed */
    uint8_t hp, hpmax;
    uint8_t state, st;      /* what it is doing, frames it has done it */
    uint8_t cycle;
    uint8_t clock;
    uint16_t x;             /* top-left of its sprite (32x32; the twin 16x16), px */
    uint8_t y;
    int8_t vx, vy;
    uint8_t g;              /* sentinel, twin: the gravity it walks in */
    uint8_t open;           /* can be hurt now */
    uint8_t flash;          /* frames of hit flash */
    uint8_t memo_g, flip_in, hop_cd;    /* the twin copies the mech's flips */
    Bolt b[NBOLT];
} Boss;

/* the enemies near the mech this frame: where they are and their boxes (the front end
 * draws these) */
typedef struct {
    uint16_t x;             /* top-left of the 16x16 sprite, px */
    uint8_t y, len;         /* (len: a crusher's reach) */
    uint8_t i;              /* index in Stage.enemy */
    uint8_t kind;
    uint16_t bx;            /* hit box */
    uint8_t by, bw, bh;
} Near;
extern uint8_t near_n;
extern Near near[MAX_NEAR];

/* more events of a frame (world_step) */
#define EV2_BOMB 0x01       /* the gravity bomb went off */
#define EV2_BOSS_ON 0x02    /* the arena sealed, the fight starts */
#define EV2_BOSS_HIT 0x04
#define EV2_BOSS_DIE 0x08   /* destroyed: the exit door is open */
#define EV2_SHIELD 0x10     /* the shield took a hit */
#define EV2_TINK 0x20       /* a shot bounced off armour */
#define EV2_BOSS_FIRE 0x40
#define EV2_SLAM 0x80       /* the piston hit the floor */
extern uint8_t ev2;
extern uint16_t ev_kx;      /* the centre of the enemy of an EV_KILL / EV_HIT, px */
extern uint8_t ev_ky;

void player_spawn(Player *p, uint8_t cx, uint8_t cy) CORE_BANKED;
/* the boss of the stage's arena, waiting (also after the mech is lost) */
void boss_reset(Boss *b, Stage *S) CORE_BANKED;
/* one frame of everything: returns EV_* (and sets ev2) */
uint16_t world_step(Player *p, Stage *S, Boss *b, uint8_t in, uint16_t t) CORE_BANKED;

#endif
