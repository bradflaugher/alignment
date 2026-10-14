/* physbot - prove a stage can be finished with the real game code and real buttons.
 *
 * A best-first search over complete world states (the mech, its shots, every enemy's
 * armour, the boss) where a move is one of 9 inputs (left / right / nothing, each with
 * or without a jump or a gravity flip pressed) held for 4 frames, with the cannon held
 * down the whole time (autofire). Everything is real: geometry, plasma, phase blocks,
 * drones, crushers, laser gates, turret bolts and the boss. The bot plays strict:
 * any hit at all is a lost mech, so a path it finds is one a player could take without
 * a scratch (power-ups are taken as they come, as the game would). The stage clock (enemies repeat every 512
 * frames) is part of a state.
 *
 * Progress, for "best": how far right the mech is; inside a boss arena, how much armour
 * the boss has lost; after the boss, how far right again. Also a library:
 * physbot_solve() is used by the tests and by msgen. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stage.h"
#include "phys.h"
#include "world.h"
#include "physbot.h"

#define HOLD 4
#define HASH_BITS 23
#define HASH_N (1u << HASH_BITS)
#define MAXQ (3u << 20)

typedef struct {
    Player p;
    Boss b;
    uint8_t hp[MAX_ENEMIES];
    uint16_t items;     /* power-ups taken (bit per entry of item_at) */
    uint16_t t;
    uint32_t parent;
    uint8_t act;
    uint8_t jheld;      /* B was held at the end of this node's block */
} Node;

static uint64_t *seen;
static Node *q;
/* best-first: a queue of node indices per progress value, oldest first */
#define NB 2048
static uint32_t *bnext;
static uint32_t bhead[NB], btail[NB];
static int bmax;
static Stage S;
#define MAX_ITEMS 16
static uint16_t item_at[MAX_ITEMS];
static uint8_t item_t[MAX_ITEMS], nitems;
static uint32_t best_i;
static int best_p;

static int progress(const Node *n)
{
    int cx = n->p.m.x >> 8;
    if (S.arena.kind == BOSS_NONE || !n->b.dead) return cx;
    return 200 + cx;
}

static void bpush(uint32_t i)
{
    int b = progress(&q[i]);
    bnext[i] = 0xFFFFFFFFu;
    if (b > best_p) { best_p = b; best_i = i; }
    if (bhead[b] == 0xFFFFFFFFu) {
        bhead[b] = i;
        btail[b] = i;
    } else {
        bnext[btail[b]] = i;
        btail[b] = i;
    }
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

/* FNV-1a over what matters of a state */
static uint64_t mix(uint64_t h, uint32_t v)
{
    int k;
    for (k = 0; k < 4; k++) {
        h ^= (v >> (k * 8)) & 0xFF;
        h *= 0x100000001B3ull;
    }
    return h;
}

static uint64_t key_node(const Node *n)
{
    const Mech *m = &n->p.m;
    const Boss *b = &n->b;
    uint64_t h = 0xCBF29CE484222325ull;
    int i;
    h = mix(h, (uint32_t)(m->x >> 4) | (uint32_t)(m->y >> 4) << 16);
    h = mix(h, (uint32_t)(uint16_t)m->vx | (uint32_t)(uint16_t)m->vy << 16);
    h = mix(h, (uint32_t)m->g | m->ground << 1 | m->airflip << 2 | m->coyote << 3 | n->jheld << 6 |
                   (uint32_t)((n->t >> 2) & 127) << 8);
    /* (shots only tell states apart in a fight: elsewhere they would multiply the
     * search for nothing) */
    if (b->active) {
        h = mix(h, n->p.cd);
        for (i = 0; i < NSHOT; i++)
            if (n->p.shot[i].life) h = mix(h, (uint32_t)n->p.shot[i].x | (uint32_t)n->p.shot[i].vx << 16);
    }
    for (i = 0; i < S.nenemies; i++) h = mix(h, n->hp[i]);
    h = mix(h, (uint32_t)n->items | (uint32_t)n->p.power << 16 | (uint32_t)n->p.bomb << 17);
    if (S.arena.kind != BOSS_NONE) {
        h = mix(h, (uint32_t)b->hp | b->state << 8 | b->active << 16 | b->dead << 17 | b->g << 18);
        h = mix(h, (uint32_t)b->x | (uint32_t)b->y << 16);
        h = mix(h, (uint32_t)b->st | (uint32_t)b->clock << 8 | (uint32_t)b->cycle << 16);
        for (i = 0; i < NBOLT; i++)
            if (b->b[i].life) h = mix(h, (uint32_t)b->b[i].x | (uint32_t)b->b[i].y << 16);
    }
    return h | 1;
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

const uint8_t physbot_acts[9][2] = {
    /* held, pressed on the first frame */
    { 0, 0 }, { IN_L, 0 }, { IN_R, 0 },
    { IN_JUMP, IN_JUMP_NEW }, { IN_L | IN_JUMP, IN_JUMP_NEW }, { IN_R | IN_JUMP, IN_JUMP_NEW },
    { 0, IN_FLIP_NEW }, { IN_L, IN_FLIP_NEW }, { IN_R, IN_FLIP_NEW },
};

/* put the shared stage in the state of node n */
static void load_state(const Node *n)
{
    int i;
    for (i = 0; i < S.nenemies; i++) S.enemy[i].hp = n->hp[i];
    for (i = 0; i < nitems; i++) S.cell[item_at[i]] = (n->items >> i) & 1 ? T_EMPTY : item_t[i];
    arena_reset(&S);
    if (n->b.active || n->b.dead) arena_seal(&S);
    if (n->b.dead) arena_open(&S);
}

/* one move (HOLD frames of action a) from node n; returns the events */
static uint16_t step(Node *n, int a, uint8_t jheld)
{
    uint16_t ev = 0;
    int f, i;
    load_state(n);
    for (f = 0; f < HOLD; f++) {
        uint8_t press = f == 0 ? physbot_acts[a][1] : 0;
        /* a jump only starts on a fresh press, as on the Game Boy */
        if ((press & IN_JUMP_NEW) && jheld) press &= (uint8_t)~IN_JUMP_NEW;
        ev = world_step(&n->p, &S, &n->b, (uint8_t)(physbot_acts[a][0] | press | IN_FIRE), ++n->t);
        if (ev & (EV_DEAD | EV_EXIT)) break;
    }
    for (i = 0; i < S.nenemies; i++) n->hp[i] = S.enemy[i].hp;
    for (i = 0; i < nitems; i++)
        if (S.cell[item_at[i]] == T_EMPTY) n->items |= (uint16_t)(1u << i);
    n->jheld = (uint8_t)((physbot_acts[a][0] & IN_JUMP) != 0);
    return ev;
}

/* A boss fight is searched a move at a time (a beam): every state of this move is
 * expanded, and only the BEAM with the least boss armour left go on. A path found
 * this way never dawdles, and the search can't drown in the fight's states. Returns
 * the node where the boss is down, or none. */
#define BEAM 1200
#define FIGHT_MOVES 700
static Node beam_n[BEAM * 9];
static uint64_t beam_k[BEAM * 9];
static uint32_t beam_ix[BEAM * 9];

static int beam_cmp(const void *a, const void *b)
{
    const Node *x = &beam_n[*(const uint32_t *)a], *y = &beam_n[*(const uint32_t *)b];
    uint64_t kx = beam_k[*(const uint32_t *)a], ky = beam_k[*(const uint32_t *)b];
    if (x->b.hp != y->b.hp) return x->b.hp < y->b.hp ? -1 : 1;
    return kx < ky ? -1 : kx > ky ? 1 : 0;
}

static uint32_t qt_g;

static uint32_t fight(uint32_t start)
{
    static uint32_t cur[BEAM];
    int ncur = 1, move, c, a;
    uint32_t nt, i;
    cur[0] = start;
    for (move = 0; move < FIGHT_MOVES && ncur; move++) {
        nt = 0;
        for (c = 0; c < ncur; c++) {
            for (a = 0; a < 9; a++) {
                Node *n = &beam_n[nt];
                uint16_t ev;
                *n = q[cur[c]];
                ev = step(n, a, q[cur[c]].jheld);
                if (ev & EV_DEAD) continue;
                n->parent = cur[c];
                n->act = (uint8_t)a;
                if (n->b.dead) {
                    if (getenv("PBDBG")) fprintf(stderr, "boss down at move %d, mech x %u y %u\n", move, n->p.m.x >> 4, n->p.m.y >> 4);
                    if (qt_g >= MAXQ) return 0xFFFFFFFFu;
                    q[qt_g] = *n;
                    return qt_g++;
                }
                beam_k[nt] = key_node(n);
                if (!insert(beam_k[nt])) continue;
                beam_ix[nt] = nt;
                nt++;
            }
        }
        qsort(beam_ix, nt, sizeof(uint32_t), beam_cmp);
        if (getenv("PBDBG") && (move % 25 == 0 || nt < 50))
            fprintf(stderr, "fight move %d: %u states, best hp %u\n", move, nt, nt ? beam_n[beam_ix[0]].b.hp : 0);
        if (nt > BEAM) nt = BEAM;
        for (i = 0, ncur = 0; i < nt && qt_g < MAXQ; i++) {
            q[qt_g] = beam_n[beam_ix[i]];
            cur[ncur++] = qt_g++;
        }
    }
    return 0xFFFFFFFFu;
}

int physbot_solve(const Stage *src, unsigned *states, char *path, int pathmax)
{
    uint32_t qh, qt = 0, i;
    uint16_t c;
    int a, f, win = -1, fights = 0;
    if (!seen) {
        seen = calloc(HASH_N, sizeof(uint64_t));
        q = malloc(MAXQ * sizeof(Node));
        bnext = malloc(MAXQ * sizeof(uint32_t));
        if (!seen || !q || !bnext) {
            fprintf(stderr, "physbot: out of memory\n");
            exit(2);
        }
    }
    memset(bhead, 0xFF, sizeof(bhead));
    bmax = -1;
    best_p = -1;
    memset(seen, 0, HASH_N * sizeof(uint64_t));
    memcpy(&S, src, sizeof(Stage));
    /* (energy cells and the checkpoint change nothing for the bot; power-ups do) */
    nitems = 0;
    for (c = 0; c < (uint16_t)S.w * SH; c++) {
        if (S.cell[c] == T_CELL || S.cell[c] == T_CHECK) S.cell[c] = T_EMPTY;
        if (T_IS_ITEM(S.cell[c]) && nitems < MAX_ITEMS) {
            item_at[nitems] = c;
            item_t[nitems++] = S.cell[c];
        }
    }
    memset(&q[0], 0, sizeof(Node));
    player_spawn(&q[0].p, S.start_x, S.start_y);
    q[0].p.strict = 1;
    q[0].p.hull = 1;
    boss_reset(&q[0].b, &S);
    for (i = 0; i < S.nenemies; i++) q[0].hp[i] = S.enemy[i].hp;
    /* settle on the floor first, as the game does (with the clock stopped) */
    for (f = 0; f < 24; f++) mech_step(&q[0].p.m, &S, 0, 0);
    q[0].t = 0;
    q[0].parent = 0xFFFFFFFFu;
    q[0].jheld = 0;
    insert(key_node(&q[0]));
    qt = 1;
    bpush(0);
    while (win < 0 && (qh = bpop()) != 0xFFFFFFFFu) {
        for (a = 0; a < 9 && win < 0; a++) {
            Node n;
            uint16_t ev;
            n = q[qh];
            ev = step(&n, a, q[qh].jheld);
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
            if (n.b.active && fights < 6) {
                /* the fight starts: settle it with the beam, carry on from the win */
                uint32_t w;
                fights++;
                qt_g = qt + 1;
                w = fight(qt);
                qt = qt_g;
                if (w != 0xFFFFFFFFu) {
                    bpush(w);
                    break;      /* (carry on from the win first) */
                }
                continue;
            }
            if (n.b.active) continue;
            bpush(qt);
            qt++;
        }
        if (qt >= MAXQ) break;
    }
    if (states) *states = qt;
    if (win < 0) {
        const Node *n = &q[best_i];
        fprintf(stderr, "physbot: furthest: x %u y %u g %u t %u boss %u/%u %s\n", n->p.m.x >> 4, n->p.m.y >> 4,
                n->p.m.g, n->t, n->b.hp, n->b.hpmax, n->b.active ? "fighting" : n->b.dead ? "down" : "waiting");
        return 0;
    }
    if (path) {
        /* the inputs, newest last: one char per HOLD frames ('0'..'8') */
        int len = 0;
        uint32_t k = (uint32_t)win;
        static char tmp[16384];
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
