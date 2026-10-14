/* algen - the ALIGNMENT puzzle core on the command line.
 *
 *   algen show  <seed> <room>        ASCII render of a room (rooms are chained from 1)
 *   algen solve <seed> <room>        the optimal solution, e.g. "12 RRFLLF..."
 *   algen stats <seeds> <rooms>      generator statistics per room band
 *   algen route <seed> <rooms>       the route through the station
 *   algen play  <seed> <room> <moves> replay moves (L R F) and show each turn
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "room.h"
#include "solver.h"
#include "gen.h"

static const char *glyph(uint8_t t)
{
    switch (t) {
    case T_EMPTY: return " .";
    case T_WALL: return "##";
    case T_PLASMA: return "~~";
    case T_PHASE_P: return "pp";
    case T_PHASE_G: return "gg";
    case T_PLATE: return "__";
    case T_DOOR: return "||";
    case T_PICK0: case T_PICK1: case T_PICK2: return "<>";
    case T_ENTRY: return "[]";
    case T_EXIT: return "EX";
    }
    return "??";
}

static void show_state(const Room *R, const State *s)
{
    uint8_t x, y, c, i;
    for (y = 0; y < RH; y++) {
        for (x = 0; x < RW; x++) {
            const char *g;
            c = CELL(x, y);
            g = glyph(R->cell[c]);
            if (IS_PICK(R->cell[c]) && (s->got & (1u << (R->cell[c] - T_PICK0)))) g = " .";
            for (i = 0; i < NOBJ; i++) {
                if (s->o[i] != c || !s->o[i]) continue;
                if (i == 0) g = s->g == G_DOWN ? "@v" : "@^";
                else if (i < SLOT_BOT) g = "[]";
                else g = "&&";
            }
            if (s->o[0] == c && R->cell[c] == T_ENTRY) g = s->g == G_DOWN ? "@v" : "@^";
            printf("%s", g);
        }
        printf("\n");
    }
}

static void gen_chain(uint16_t seed, uint16_t room, Room *R, Door *d)
{
    Door cur, nx;
    gen_first_door(&cur);
    for (;;) {
        gen_room(R, seed, &cur);
        if (cur.index >= room) break;
        gen_next_door(&cur, R, &nx);
        cur = nx;
    }
    *d = cur;
}

static const char act_ch[3] = { 'L', 'R', 'F' };

static int solution(const Room *R, char *out)
{
    State s, t;
    uint8_t par, rem, a, n = 0;
    state_start(R, &s);
    par = solve(R, &s);
    if (!par) { out[0] = 0; return 0; }
    rem = par;
    while (rem) {
        /* pick an action whose successor is one closer */
        for (a = 0; a < NUM_ACT; a++) {
            uint8_t r;
            t = s;
            r = sim_turn(R, &t, a, NULL);
            if (r == TR_WIN && rem == 1) { out[n++] = act_ch[a]; rem = 0; break; }
            if (r != TR_OK) continue;
            if (solve(R, &t) == rem - 1) { out[n++] = act_ch[a]; s = t; rem--; break; }
        }
        if (a == NUM_ACT) { out[n++] = '?'; break; }
    }
    out[n] = 0;
    return par;
}

static const char *mech_names[NUM_MECH] = { "plasma", "cells", "crates", "drones", "phase", "doors" };

static void info(const Room *R, const Door *d)
{
    uint8_t m;
    printf("room %u  (%d,%d)  entry %s  exit %s  g0 %s  par %u  flips %u  cost %u searches %u%s\n",
           R->index, d->rx, d->ry,
           (const char *[]){"L", "R", "T", "B"}[R->entry_side],
           (const char *[]){"L", "R", "T", "B"}[R->exit_side],
           R->g0 ? "up" : "down", R->par, R->flips, gen_cost, gen_searches,
           R->fallback ? "  FALLBACK" : "");
    printf("mechs:");
    for (m = 0; m < NUM_MECH; m++) if (R->mechs & MBIT(m)) printf(" %s", mech_names[m]);
    if (R->featured != 0xFF) printf("  (new: %s)", mech_names[R->featured]);
    printf("\n");
}

int main(int argc, char **argv)
{
    Room R;
    Door d;
    State s;
    char sol[256];
    if (argc < 4) {
        fprintf(stderr, "usage: algen show|solve|stats|route|play ...\n");
        return 1;
    }
    if (!strcmp(argv[1], "show") || !strcmp(argv[1], "solve")) {
        gen_chain((uint16_t)strtoul(argv[2], 0, 0), (uint16_t)atoi(argv[3]), &R, &d);
        info(&R, &d);
        state_start(&R, &s);
        show_state(&R, &s);
        solution(&R, sol);
        printf("%u %s\n", R.par, sol);
        return 0;
    }
    if (!strcmp(argv[1], "dump")) {
        /* machine-readable: par exit g0 entry, then the 90 cells, crates, drones */
        uint8_t i;
        gen_chain((uint16_t)strtoul(argv[2], 0, 0), (uint16_t)atoi(argv[3]), &R, &d);
        printf("%u %u %u %u %u %u", R.par, R.exit, R.g0, R.entry, R.npicks, R.flips);
        for (i = 0; i < RN; i++) printf(" %u", R.cell[i]);
        printf(" %u %u %u %u\n", R.crate0[0], R.crate0[1], R.bot0[0], R.bot0[1]);
        state_start(&R, &s);
        solution(&R, sol);
        printf("%s\n", sol);
        return 0;
    }
    if (!strcmp(argv[1], "route")) {
        Door cur, nx;
        uint16_t i, n = (uint16_t)atoi(argv[3]);
        gen_first_door(&cur);
        for (i = 0; i < n; i++) {
            gen_room(&R, (uint16_t)strtoul(argv[2], 0, 0), &cur);
            info(&R, &cur);
            gen_next_door(&cur, &R, &nx);
            cur = nx;
        }
        return 0;
    }
    if (!strcmp(argv[1], "play") && argc >= 5) {
        const char *mv = argv[4];
        TurnLog lg;
        gen_chain((uint16_t)strtoul(argv[2], 0, 0), (uint16_t)atoi(argv[3]), &R, &d);
        state_start(&R, &s);
        show_state(&R, &s);
        for (; *mv; mv++) {
            uint8_t a = *mv == 'L' ? ACT_L : *mv == 'R' ? ACT_R : ACT_FLIP;
            uint8_t r = sim_turn(&R, &s, a, &lg);
            printf("-- %c -> %s flags %02x\n", *mv, (const char *[]){"blocked", "ok", "WIN", "DEAD"}[r], lg.flags);
            show_state(&R, &s);
            if (r == TR_WIN || r == TR_DEAD) break;
        }
        return 0;
    }
    if (!strcmp(argv[1], "stats")) {
        uint16_t seeds = (uint16_t)atoi(argv[2]), rooms = (uint16_t)atoi(argv[3]);
        uint16_t sd, i;
        enum { NB = 8 };
        static const uint16_t band_hi[NB] = { 4, 8, 12, 16, 24, 40, 80, 65535 };
        double sum_par[NB] = {0}, sum_flips[NB] = {0}, sum_cost[NB] = {0}, n_b[NB] = {0}, in_win[NB] = {0};
        uint16_t max_cost[NB] = {0};
        unsigned fallbacks = 0, total = 0;
        for (sd = 1; sd <= seeds; sd++) {
            Door cur, nx;
            gen_first_door(&cur);
            for (i = 0; i < rooms; i++) {
                GenParams P;
                uint8_t b = 0;
                gen_room(&R, (uint16_t)(sd * 7919u), &cur);
                while (cur.index > band_hi[b]) b++;
                n_b[b]++;
                sum_par[b] += R.par;
                sum_flips[b] += R.flips;
                sum_cost[b] += gen_cost;
                if (gen_cost > max_cost[b]) max_cost[b] = gen_cost;
                (void)P;
                if (R.fallback) fallbacks++;
                if (gen_score == 0) in_win[b]++;
                total++;
                gen_next_door(&cur, &R, &nx);
                cur = nx;
            }
        }
        printf("band      n   par  flips   cost  maxcost  inwin\n");
        for (i = 0; i < NB; i++) {
            if (!n_b[i]) continue;
            printf("<=%-5u %4.0f %5.1f %5.1f %7.0f %7u  %3.0f%%\n", band_hi[i], n_b[i], sum_par[i] / n_b[i],
                   sum_flips[i] / n_b[i], sum_cost[i] / n_b[i], max_cost[i], 100.0 * in_win[i] / n_b[i]);
        }
        
        printf("fallbacks %u / %u\n", fallbacks, total);
        return 0;
    }
    fprintf(stderr, "unknown command\n");
    return 1;
}
