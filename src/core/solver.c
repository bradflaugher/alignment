/* ALIGNMENT - breadth-first solver.
 *
 * A state packs into 5 bytes plus one byte of search metadata:
 *   [0] mech cell | gravity << 7
 *   [1] crate A   | cell 0 collected << 7      (crates sorted: A <= B)
 *   [2] crate B   | cell 1 collected << 7
 *   [3] drone A   | cell 2 collected << 7      (drones sorted: A <= B)
 *   [4] drone B
 *   [5] first action of the path (2 bits) | flips on the path << 2 (saturating)
 *   [6] [7] next state in the same hash bucket (little endian, 0xFFFF = end)
 * Records are 8 bytes so an index turns into an address with three shifts. The array
 * doubles as the BFS queue; a 512-entry hash table in WRAM heads the bucket chains. */
#ifdef __SDCC
#pragma bank 255
#include <gb/gb.h>
#endif
#include <string.h>
#include "solver.h"

#ifdef __SDCC
#define MEM ((uint8_t *)0xA000)
#define MEM_MAP() do { ENABLE_RAM; SWITCH_RAM(1); } while (0)
#else
uint8_t solver_mem[8192];
#define MEM solver_mem
#define MEM_MAP() do { } while (0)
#endif

#define HASH_N 512
#define REC(i) (MEM + ((uint16_t)(i) << 3))
#define END 0xFFFFu

static uint16_t heads[HASH_N];
uint16_t solve_visited;
uint8_t solve_first, solve_flips, solve_capped;
uint8_t solve_limit = SOLVE_MAX_DEPTH;
void (*solve_yield)(void);
uint8_t solve_epoch;
uint8_t explore_depth[RN];
uint8_t explore_flips[RN];
uint8_t explore_reach[(RN + 7) / 8];

static uint8_t key[5];
static uint16_t n_states;

static void pack(void)
{
    uint8_t a = f_o[SLOT_CRATE], b = f_o[SLOT_CRATE + 1], t;
    if (a > b) { t = a; a = b; b = t; }
    key[0] = (uint8_t)(f_o[0] | (f_g << 7));
    key[1] = (uint8_t)(a | ((f_got & 1) << 7));
    key[2] = (uint8_t)(b | ((f_got & 2) << 6));
    a = f_o[SLOT_BOT];
    b = f_o[SLOT_BOT + 1];
    if (a > b) { t = a; a = b; b = t; }
    key[3] = (uint8_t)(a | ((f_got & 4) << 5));
    key[4] = b;
}

static void unpack(const uint8_t *d)
{
    f_o[0] = (uint8_t)(d[0] & 0x7F);
    f_g = (uint8_t)(d[0] >> 7);
    f_o[SLOT_CRATE] = (uint8_t)(d[1] & 0x7F);
    f_o[SLOT_CRATE + 1] = (uint8_t)(d[2] & 0x7F);
    f_o[SLOT_BOT] = (uint8_t)(d[3] & 0x7F);
    f_o[SLOT_BOT + 1] = d[4];
    f_got = (uint8_t)((d[1] >> 7) | ((d[2] >> 6) & 2) | ((d[3] >> 5) & 4));
}

static uint16_t hash_key(void)
{
    uint16_t h = (uint16_t)((uint16_t)key[0] << 2);
    h ^= key[1];
    h ^= (uint16_t)((uint16_t)key[2] << 1);
    h ^= (uint16_t)((uint16_t)key[3] << 3);
    h ^= (uint16_t)((uint16_t)key[4] << 4);
    h ^= (uint16_t)(h >> 9);
    return (uint16_t)(h & (HASH_N - 1));
}

/* insert key if new; returns 1 if inserted, 0 if seen (or full: solve_capped) */
static uint8_t insert(uint8_t meta)
{
    uint16_t h = hash_key();
    uint16_t i = heads[h];
    uint8_t *d;
    uint8_t k0 = key[0], k1 = key[1];
    while (i != END) {
        d = REC(i);
        if (d[0] == k0 && d[1] == k1 && d[2] == key[2] && d[3] == key[3] && d[4] == key[4])
            return 0;
        i = (uint16_t)(d[6] | ((uint16_t)d[7] << 8));
    }
    if (n_states >= SOLVE_CAP) {
        solve_capped = 1;
        return 0;
    }
    d = REC(n_states);
    d[0] = k0;
    d[1] = k1;
    d[2] = key[2];
    d[3] = key[3];
    d[4] = key[4];
    d[5] = meta;
    i = heads[h];
    d[6] = (uint8_t)i;
    d[7] = (uint8_t)(i >> 8);
    heads[h] = n_states;
    n_states++;
    return 1;
}

static void search_reset(const Room *R, const State *from)
{
    solve_epoch++;
    MEM_MAP();
    memset(heads, 0xFF, sizeof(heads));
    n_states = 0;
    solve_capped = 0;
    sim_load(R);
    memcpy(f_o, from->o, NOBJ);
    f_g = from->g;
    f_got = from->got;
    pack();
    insert(0);
}

#define MODE_SOLVE 0
#define MODE_EXPLORE 1

static uint8_t mark_explore(uint8_t depth, uint8_t full, uint8_t meta)
{
    uint8_t p = f_o[0], x, y, d;
    explore_reach[p >> 3] |= (uint8_t)(1u << (p & 7));
    if (f_got != full)
        return 0;
    x = CX(p);
    y = CY(p);
    if (x == RW - 2 && y > 0 && y < RH - 1) {
        /* stand at (8,y): through a doorway at (9,y) in two more steps */
        d = (uint8_t)(depth + 2);
        p = CELL(RW - 1, y);
    } else if (f_g == G_UP && y == 1 && x > 0 && x < RW - 1) {
        d = depth;
        p = CELL(x, 0);
    } else if (f_g == G_DOWN && y == RH - 2 && x > 0 && x < RW - 1) {
        d = depth;
        p = CELL(x, RH - 1);
    } else {
        return 0;
    }
    if (d && (!explore_depth[p] || d < explore_depth[p])) {
        explore_depth[p] = d;
        explore_flips[p] = (uint8_t)(meta >> 2);
    }
    return 0;
}

/* the search; mode MODE_SOLVE returns par, MODE_EXPLORE returns 0 */
static uint8_t search(const Room *R, const State *from, uint8_t mode, uint8_t bg)
{
    uint16_t head, layer_end;
    uint8_t depth, act, r, meta, m0, epoch, full;
    uint8_t cur[5];
    uint8_t *d;

restart:
    search_reset(R, from);
    epoch = solve_epoch;
    full = room_full(R);
    head = 0;
    layer_end = 1;
    depth = 0;
    if (mode == MODE_EXPLORE) {
        memset(explore_depth, 0, sizeof(explore_depth));
        memset(explore_flips, 0, sizeof(explore_flips));
        memset(explore_reach, 0, sizeof(explore_reach));
        mark_explore(0, full, 0);
    }
    while (head < n_states) {
        if (head == layer_end) {
            depth++;
            layer_end = n_states;
            if (depth >= solve_limit)
                break;
        }
        if (bg && solve_yield) {
            solve_yield();
            if (solve_epoch != epoch)
                goto restart;           /* someone else used the work area */
            MEM_MAP();
            sim_load(R);                /* the game's own turns reload the sim tables */
        }
        d = REC(head);
        cur[0] = d[0];
        cur[1] = d[1];
        cur[2] = d[2];
        cur[3] = d[3];
        cur[4] = d[4];
        m0 = d[5];
        head++;
        for (act = 0; act < NUM_ACT; act++) {
            unpack(cur);
            r = sim_core(act, NULL);
            if (r == TR_BLOCKED || r == TR_DEAD)
                continue;
            meta = depth ? m0 : act;
            if (act == ACT_FLIP && (meta >> 2) < 63)
                meta = (uint8_t)(meta + 4);
            if (r == TR_WIN) {
                solve_visited = n_states;
                solve_first = (uint8_t)(meta & 3);
                solve_flips = (uint8_t)(meta >> 2);
                return (uint8_t)(depth + 1);
            }
            pack();
            if (insert(meta) && mode == MODE_EXPLORE)
                mark_explore((uint8_t)(depth + 1), full, meta);
        }
    }
    solve_visited = n_states;
    return 0;
}

uint8_t solve(const Room *R, const State *from) CORE_BANKED
{
    return search(R, from, MODE_SOLVE, 0);
}

uint8_t solve_bg(const Room *R, const State *from) CORE_BANKED
{
    return search(R, from, MODE_SOLVE, 1);
}

uint16_t explore_bg(const Room *R, const State *from) CORE_BANKED
{
    search(R, from, MODE_EXPLORE, 1);
    return solve_visited;
}
