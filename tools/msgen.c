/* msgen - the ALIGNMENT stage generator on the command line.
 *
 *   msgen show  <seed> <stage>        ASCII map of a stage
 *   msgen stats <seeds> <stages>      generator statistics (repairs, widths, mechanics)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stage.h"
#include "physbot.h"
#include "phys.h"

static Stage S;

static char glyph(const Stage *s, uint8_t x, uint8_t y)
{
    uint8_t i;
    for (i = 0; i < s->nenemies; i++) {
        const Enemy *e = &s->enemy[i];
        if (e->kind == E_CRAWLER && y == e->y0 && x >= e->x0 && x <= e->x1) return x == e->x0 ? 'C' : '-';
        if ((e->kind == E_HOVER || e->kind == E_CRUSHER) && x == e->x0 && y >= e->y0 && y <= e->y1)
            return e->kind == E_HOVER ? 'H' : 'K';
    }
    if (x == s->start_x && y == s->start_y) return '@';
    switch (STAGE_AT(s, x, y)) {
    case T_EMPTY: return ' ';
    case T_WALL: return '#';
    case T_PLASMA: return '~';
    case T_PHASE_P: return 'p';
    case T_PHASE_G: return 'g';
    case T_CELL: return '*';
    case T_CHECK: return '!';
    case T_EXIT: return 'E';
    case T_LASER: return '|';
    case T_GIRDER: return '=';
    }
    return '?';
}

int main(int argc, char **argv)
{
    if (argc >= 4 && !strcmp(argv[1], "show")) {
        uint8_t x, y;
        stage_gen(&S, (uint16_t)strtoul(argv[2], 0, 0), (uint16_t)atoi(argv[3]));
        printf("stage %u  width %u  enemies %u  cells %u  fixes %u  mechs %03x\n", S.index, S.w,
               S.nenemies, S.ncells, S.fixes, S.mechs);
        for (y = 0; y < SH; y++) {
            for (x = 0; x < S.w; x++) putchar(glyph(&S, x, y));
            putchar('\n');
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "raw")) {
        /* the unrepaired stage, with what the checker reached: d (down) u (up) b (both) */
        uint8_t x, y;
        extern void stage_gen_raw(Stage *, uint16_t, uint16_t);
        stage_gen_raw(&S, (uint16_t)strtoul(argv[2], 0, 0), (uint16_t)atoi(argv[3]));
        for (y = 0; y < SH; y++) {
            for (x = 0; x < S.w; x++) {
                unsigned i = x * SH + y, a = check_seen[i >> 3] >> (i & 7) & 1,
                         b = check_seen[(i + SW_MAX * SH) >> 3] >> ((i + SW_MAX * SH) & 7) & 1;
                char c = glyph(&S, x, y);
                if (a || b) c = a && b ? 'b' : a ? 'd' : 'u';
                putchar(c);
            }
            putchar('\n');
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "bot")) {
        /* can the real physics finish these stages? */
        unsigned seeds = (unsigned)atoi(argv[2]), stages = (unsigned)atoi(argv[3]), sd, n, st, ok = 0, tot = 0;
        static char path[4096];
        for (sd = 1; sd <= seeds; sd++)
            for (n = 1; n <= stages; n++) {
                int r;
                stage_gen(&S, (uint16_t)(sd * 7919u), (uint16_t)n);
                r = physbot_solve(&S, &st, path, sizeof(path));
                tot++;
                ok += r;
                if (!r || argc > 4) printf("seed %u stage %u: %s (%u states, %zu inputs)\n", sd * 7919u, n, r ? "ok" : "STUCK", st, strlen(path));
            }
        printf("%u / %u stages finished by the physics bot\n", ok, tot);
        return ok == tot ? 0 : 1;
    }
    if (argc >= 4 && !strcmp(argv[1], "path")) {
        /* the bot's input path for one stage: one digit per 4 frames */
        static char path[8192];
        unsigned st;
        stage_gen(&S, (uint16_t)strtoul(argv[2], 0, 0), (uint16_t)atoi(argv[3]));
        if (!physbot_solve(&S, &st, path, sizeof(path))) { printf("none\n"); return 1; }
        printf("%s\n", path);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "replay")) {
        /* play a bot path with the real physics; print the mech every move */
        static const uint8_t acts[9][2] = {
            { 0, 0 }, { IN_L, 0 }, { IN_R, 0 },
            { IN_JUMP, IN_JUMP_NEW }, { IN_L | IN_JUMP, IN_JUMP_NEW }, { IN_R | IN_JUMP, IN_JUMP_NEW },
            { 0, IN_FLIP_NEW }, { IN_L, IN_FLIP_NEW }, { IN_R, IN_FLIP_NEW } };
        Mech m;
        const char *p = argv[4];
        int f, i = 0;
        uint8_t jheld = 0;
        uint16_t t = 0, ev = 0;
        stage_gen(&S, (uint16_t)strtoul(argv[2], 0, 0), (uint16_t)atoi(argv[3]));
        mech_spawn(&m, S.start_x, S.start_y, G_DOWN);
        for (f = 0; f < 24; f++) mech_step(&m, &S, 0, 0);
        for (; *p; p++, i++) {
            int a = *p - '0';
            for (f = 0; f < 4; f++) {
                uint8_t press = f == 0 ? acts[a][1] : 0;
                if ((press & IN_JUMP_NEW) && jheld) press &= (uint8_t)~IN_JUMP_NEW;
                ev = mech_step(&m, &S, (uint8_t)(acts[a][0] | press), ++t);
                if (ev & (EV_DEAD | EV_EXIT)) break;
            }
            jheld = (uint8_t)((acts[a][0] & IN_JUMP) != 0);
            printf("%d %c x%u y%u g%u ev%03x\n", i, *p, m.x >> 4, m.y >> 4, m.g, ev);
            if (ev & (EV_DEAD | EV_EXIT)) break;
        }
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "stats")) {
        unsigned seeds = (unsigned)atoi(argv[2]), stages = (unsigned)atoi(argv[3]), sd, n;
        unsigned fixes = 0, fixed_stages = 0, total = 0, fails = 0, unsorted = 0;
        for (sd = 1; sd <= seeds; sd++)
            for (n = 1; n <= stages; n++) {
                stage_gen(&S, (uint16_t)(sd * 7919u), (uint16_t)n);
                total++;
                fixes += S.fixes;
                if (S.fixes) { fixed_stages++; if (fixed_stages <= 6) printf("repair: seed %u stage %u\n", sd * 7919u, n); }
                if (!stage_check(&S)) fails++;
                {
                    /* the game draws enemies assuming they run left to right */
                    uint8_t k;
                    for (k = 1; k < S.nenemies; k++)
                        if (S.enemy[k].x0 < S.enemy[k - 1].x0) { unsorted++; break; }
                }
            }
        printf("%u stages, %u needed repairs (%u repairs), %u impassable, %u with unsorted enemies\n",
               total, fixed_stages, fixes, fails, unsorted);
        return fails || unsorted ? 1 : 0;
    }
    fprintf(stderr, "usage: msgen show|stats ...\n");
    return 1;
}
