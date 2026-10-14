/* physbot - prove a stage can be finished with the real physics and real button presses.
 *
 * Breadth-first search over mech states (position, speed, gravity, air flip...), where a
 * move is one of 9 inputs (left / right / nothing, each with or without a jump or a flip
 * pressed) held for a few frames. Everything is real: the geometry, plasma, phase blocks,
 * and the drones, crushers and laser gates on the stage clock (they all repeat every 512
 * frames, so the clock modulo 512 is part of a state). Also a library: physbot_solve() is
 * used by the tests and by msgen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stage.h"
#include "phys.h"
#include "physbot.h"

#define HOLD 4
#define HASH_BITS 22
#define HASH_N (1u << HASH_BITS)
#define MAXQ (6u << 20)

typedef struct {
    Mech m;
    uint32_t parent;
    uint8_t act;
    uint8_t jheld;      /* B was held at the end of this node's block */
    uint16_t t;         /* the stage clock */
} Node;

static uint64_t *seen;
static Node *q;
/* best-first: a queue of node indices per pixel column, always taking the rightmost
 * column, oldest first (so the path does not dawdle) */
#define NB 2048
static uint32_t *bnext;         /* next node in the same bucket */
static uint32_t bhead[NB], btail[NB];
static int bmax;

static void bpush(uint32_t i)
{
    int b = q[i].m.x >> 8;
    bnext[i] = 0xFFFFFFFFu;
    if (bhead[b] == 0xFFFFFFFFu) bhead[b] = i;
    else bnext[btail[b]] = i;
    btail[b] = i;
    if (b > bmax) bmax = b;
}

static uint32_t bpop(void)
{
    uint32_t i;
    while (bmax >= 0 && bhead[bmax] == 0xFFFFFFFFu) bmax--;
    if (bmax < 0) return 0xFFFFFFFFu;
    i = bhead[bmax];
    bhead[bmax] = bnext[i];
    return i;
}

static uint64_t key_of(const Mech *m)
{
    uint64_t k = 0;
    k |= (uint64_t)(m->x >> 4);                 /* 12 bits */
    k |= (uint64_t)(m->y >> 4) << 12;           /* 8 */
    k |= (uint64_t)((m->vx + 64) & 127) << 20;  /* 7 */
    k |= (uint64_t)((m->vy + 128) & 255) << 27; /* 8 */
    k |= (uint64_t)m->g << 35;
    k |= (uint64_t)m->ground << 36;
    k |= (uint64_t)m->airflip << 37;
    k |= (uint64_t)m->coyote << 38;             /* 3 */
    return k + 1;
}

static uint64_t key_node(const Node *n)
{
    return key_of(&n->m) ^ ((uint64_t)n->jheld << 45) ^ ((uint64_t)((n->t >> 2) & 127) << 46);
}

static int insert(uint64_t k)
{
    uint32_t h = (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> (64 - HASH_BITS));
    while (seen[h]) {
        if (seen[h] == k) return 0;
        h = (h + 1) & (HASH_N - 1);
    }
    seen[h] = k;
    return 1;
}

static const uint8_t acts[9][2] = {
    /* held, pressed on the first frame */
    { 0, 0 }, { IN_L, 0 }, { IN_R, 0 },
    { IN_JUMP, IN_JUMP_NEW }, { IN_L | IN_JUMP, IN_JUMP_NEW }, { IN_R | IN_JUMP, IN_JUMP_NEW },
    { 0, IN_FLIP_NEW }, { IN_L, IN_FLIP_NEW }, { IN_R, IN_FLIP_NEW },
};

int physbot_solve(const Stage *src, unsigned *states, char *path, int pathmax)
{
    static Stage S;
    uint32_t qh, qt = 0, i;
    uint16_t c;
    int a, f, win = -1;
    if (!seen) {
        seen = calloc(HASH_N, sizeof(uint64_t));
        q = malloc(MAXQ * sizeof(Node));
        bnext = malloc(MAXQ * sizeof(uint32_t));
    }
    memset(bhead, 0xFF, sizeof(bhead));
    bmax = -1;
    memset(seen, 0, HASH_N * sizeof(uint64_t));
    memcpy(&S, src, sizeof(Stage));
    /* (pickups change nothing for the physics) */
    for (c = 0; c < (uint16_t)S.w * SH; c++)
        if (S.cell[c] == T_CELL || S.cell[c] == T_CHECK) S.cell[c] = T_EMPTY;
    mech_spawn(&q[0].m, S.start_x, S.start_y, G_DOWN);
    /* settle on the floor first, as the game does (with the clock stopped) */
    for (f = 0; f < 24; f++) mech_step(&q[0].m, &S, 0, 0);
    q[0].t = 0;
    q[0].parent = 0xFFFFFFFFu;
    q[0].jheld = 0;
    insert(key_node(&q[0]));
    qt = 1;
    bpush(0);
    while (win < 0 && (qh = bpop()) != 0xFFFFFFFFu) {
        for (a = 0; a < 9 && win < 0; a++) {
            Node n;
            uint16_t ev = 0;
            n.m = q[qh].m;
            n.t = q[qh].t;
            for (f = 0; f < HOLD; f++) {
                uint8_t press = f == 0 ? acts[a][1] : 0;
                /* a jump only starts on a fresh press of B, as on the Game Boy */
                if ((press & IN_JUMP_NEW) && q[qh].jheld) press &= (uint8_t)~IN_JUMP_NEW;
                ev = mech_step(&n.m, &S, (uint8_t)(acts[a][0] | press), ++n.t);
                if (ev & (EV_DEAD | EV_EXIT)) break;
            }
            n.jheld = (uint8_t)((acts[a][0] & IN_JUMP) != 0);
            if (ev & EV_DEAD) continue;
            n.parent = qh;
            n.act = (uint8_t)a;
            if (ev & EV_EXIT) {
                if (qt < MAXQ) q[qt] = n;
                win = (int)qt;
                break;
            }
            if (!insert(key_node(&n))) continue;
            if (qt >= MAXQ) break;
            q[qt] = n;
            bpush(qt);
            qt++;
        }
    }
    if (states) *states = qt;
    if (win < 0) return 0;
    if (path) {
        /* the inputs, newest last: one char per HOLD frames ('0'..'8') */
        int len = 0;
        uint32_t k = (uint32_t)win;
        char tmp[8192];
        while (q[k].parent != 0xFFFFFFFFu && len < (int)sizeof(tmp)) {
            tmp[len++] = (char)('0' + q[k].act);
            k = q[k].parent;
        }
        if (len >= pathmax) len = pathmax - 1;
        for (i = 0; i < (uint32_t)len; i++) path[i] = tmp[len - 1 - i];
        path[len] = 0;
    }
    return 1;
}
