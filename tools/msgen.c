/* msgen - ALIGNMENT stages on the command line.
 *
 * A stage is either a generated one, "<seed> <stage>", or a hand-made sector, "s <n>".
 *
 *   msgen show   STAGE              ASCII map
 *   msgen raw    <seed> <stage>     a generated stage before repairs, with what the checker reached
 *   msgen path   STAGE              the physics bot's button presses (one digit per 4 frames)
 *   msgen replay STAGE <path>       play a path with the real game code, print every move
 *   msgen bot    <seeds> <stages> [v]   can the bot finish generated stages?
 *   msgen sbot   [v]                    can the bot finish every hand-made sector?
 *   msgen stats  <seeds> <stages>       generator statistics
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "stage.h"
#include "physbot.h"
#include "phys.h"
#include "world.h"

static Stage S;
extern const uint8_t physbot_acts[9][2];

static char glyph(const Stage *s, uint8_t x, uint8_t y)
{
    uint8_t i;
    for (i = 0; i < s->nenemies; i++) {
        const Enemy *e = &s->enemy[i];
        if (e->kind == E_CRAWLER && y == e->y0 && x >= e->x0 && x <= e->x1) return x == e->x0 ? (e->y1 ? 'k' : 'c') : '-';
        if (e->kind == E_TURRET && y == e->y0 && x == (e->speed ? e->x1 : e->x0))
            return e->y1 ? (e->speed ? '{' : '}') : (e->speed ? '<' : '>');
        if ((e->kind == E_HOVER || e->kind == E_CRUSHER) && x == e->x0 && y >= e->y0 && y <= e->y1)
            return e->kind == E_HOVER ? 'v' : 'X';
    }
    if (x == s->start_x && y == s->start_y) return 'S';
    if (s->arena.kind && x == s->arena.bx && y == s->arena.by) return 'B';
    switch (STAGE_AT(s, x, y)) {
    case T_EMPTY: return '.';
    case T_WALL: return '#';
    case T_PLASMA: return '~';
    case T_PHASE_P: return 'P';
    case T_PHASE_G: return 'G';
    case T_CELL: return 'o';
    case T_CHECK: return 'C';
    case T_EXIT: return 'E';
    case T_LASER: return 'L';
    case T_GIRDER: return '=';
    case T_ITEM_P: return 'p';
    case T_ITEM_S: return 's';
    case T_ITEM_B: return 'b';
    case T_ITEM_R: return 'r';
    case T_DOOR: return 'D';
    case T_DOOR_OPEN: return 'd';
    case T_DECO_PIPE: return '-';
    case T_DECO_PIPE_V: return '|';
    case T_DECO_WINDOW: return 'O';
    case T_DECO_LIGHT: return '*';
    case T_DECO_GRATE: return '%';
    }
    return '?';
}

/* STAGE at argv[at]: returns the number of arguments used, 0 if missing */
static int load(int argc, char **argv, int at)
{
    if (at + 1 >= argc) return 0;
    if (!strcmp(argv[at], "s")) {
        if (!stage_load(&S, (uint8_t)atoi(argv[at + 1]))) {
            fprintf(stderr, "msgen: no sector %s\n", argv[at + 1]);
            exit(1);
        }
    } else {
        stage_gen(&S, (uint16_t)strtoul(argv[at], 0, 0), (uint16_t)atoi(argv[at + 1]));
    }
    return 2;
}

static void show(void)
{
    uint8_t x, y;
    printf("stage %u  sector %u  width %u  enemies %u  cells %u  fixes %u  boss %u/%u\n", S.index, S.sector,
           S.w, S.nenemies, S.ncells, S.fixes, S.arena.kind, S.arena.level);
    for (y = 0; y < SH; y++) {
        for (x = 0; x < S.w; x++) putchar(glyph(&S, x, y));
        putchar('\n');
    }
}

int main(int argc, char **argv)
{
    static char path[16384];
    unsigned st;
    if (argc >= 3 && !strcmp(argv[1], "show") && load(argc, argv, 2)) {
        show();
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
        /* can the real game code finish these stages? */
        unsigned seeds = (unsigned)atoi(argv[2]), stages = (unsigned)atoi(argv[3]), sd, n, ok = 0, tot = 0;
        for (sd = 1; sd <= seeds; sd++)
            for (n = 1; n <= stages; n++) {
                int r;
                stage_gen(&S, (uint16_t)(sd * 7919u), (uint16_t)n);
                r = physbot_solve(&S, &st, path, sizeof(path));
                tot++;
                ok += r;
                if (!r || argc > 4)
                    printf("seed %u stage %u: %s (%u states, %zu inputs)\n", sd * 7919u, n, r ? "ok" : "STUCK", st, strlen(path));
                fflush(stdout);
            }
        printf("%u / %u stages finished by the physics bot\n", ok, tot);
        return ok == tot ? 0 : 1;
    }
    if (argc >= 2 && !strcmp(argv[1], "sbot")) {
        unsigned n, ok = 0, tot = sector_count();
        for (n = 1; n <= tot; n++) {
            int r;
            stage_load(&S, (uint8_t)n);
            r = physbot_solve(&S, &st, path, sizeof(path));
            ok += r;
            printf("sector %u: %s (%u states, %zu inputs)\n", n, r ? "ok" : "STUCK", st, strlen(path));
            fflush(stdout);
        }
        printf("%u / %u sectors finished by the physics bot\n", ok, tot);
        return ok == tot ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "path") && load(argc, argv, 2)) {
        if (!physbot_solve(&S, &st, path, sizeof(path))) {
            printf("none\n");
            return 1;
        }
        printf("%s\n", path);
        return 0;
    }
    if (argc >= 5 && !strcmp(argv[1], "replay") && load(argc, argv, 2)) {
        /* play a path with the real game code (strict, autofire, as the bot does) */
        static Player p;
        static Boss b;
        const char *s = argv[4];
        int f, i = 0;
        uint8_t jheld = 0;
        uint16_t t = 0, ev = 0;
        memset(&p, 0, sizeof(p));
        player_spawn(&p, S.start_x, S.start_y);
        p.strict = 1;
        boss_reset(&b, &S);
        for (f = 0; f < 24; f++) mech_step(&p.m, &S, 0, 0);
        for (; *s; s++, i++) {
            int a = *s - '0';
            for (f = 0; f < 4; f++) {
                uint8_t press = f == 0 ? physbot_acts[a][1] : 0;
                if ((press & IN_JUMP_NEW) && jheld) press &= (uint8_t)~IN_JUMP_NEW;
                ev = world_step(&p, &S, &b, (uint8_t)(physbot_acts[a][0] | press | IN_FIRE), ++t);
                if (getenv("MSGEN_FRAMES"))
                    printf("  t %u mech (%u, %u) boss hp %u act %u bx %u %u st %u %u shots (%u,%u,%u) (%u,%u,%u) (%u,%u,%u) pow %u\n",
                           t, p.m.x >> 4, p.m.y >> 4, b.hp, b.active, b.x, b.y, b.state, b.st,
                           p.shot[0].x, p.shot[0].y, p.shot[0].life, p.shot[1].x, p.shot[1].y, p.shot[1].life,
                           p.shot[2].x, p.shot[2].y, p.shot[2].life, p.power);
                if (ev & (EV_DEAD | EV_EXIT)) break;
            }
            jheld = (uint8_t)((physbot_acts[a][0] & IN_JUMP) != 0);
            printf("%d %c x%u y%u g%u ev%04x boss %u/%u%s\n", i, *s, p.m.x >> 4, p.m.y >> 4, p.m.g, ev,
                   b.hp, b.hpmax, b.active ? " fighting" : b.dead ? " down" : "");
            if (ev & (EV_DEAD | EV_EXIT)) break;
        }
        if (ev & EV_DEAD) printf("dead: %u\n", p.m.dead);
        return 0;
    }
    if (argc >= 4 && !strcmp(argv[1], "stats")) {
        unsigned seeds = (unsigned)atoi(argv[2]), stages = (unsigned)atoi(argv[3]), sd, n;
        unsigned fixes = 0, fixed_stages = 0, total = 0, fails = 0, unsorted = 0, nocheck = 0;
        for (sd = 1; sd <= seeds; sd++)
            for (n = 1; n <= stages; n++) {
                stage_gen(&S, (uint16_t)(sd * 7919u), (uint16_t)n);
                total++;
                fixes += S.fixes;
                if (S.fixes) { fixed_stages++; if (fixed_stages <= 6) printf("repair: seed %u stage %u\n", sd * 7919u, n); }
                if (!stage_check(&S)) fails++;
                {
                    /* every stage has its halfway checkpoint */
                    uint16_t c, k = 0;
                    for (c = 0; c < (uint16_t)S.w * SH; c++) k += S.cell[c] == T_CHECK;
                    if (k != 1) { nocheck++; if (nocheck <= 3) printf("checkpoints %u: seed %u stage %u\n", k, sd * 7919u, n); }
                }
                {
                    /* the game draws enemies assuming they run left to right */
                    uint8_t k;
                    for (k = 1; k < S.nenemies; k++)
                        if (S.enemy[k].x0 < S.enemy[k - 1].x0) { unsorted++; break; }
                }
            }
        printf("%u stages, %u needed repairs (%u repairs), %u impassable, %u with unsorted enemies, %u without one checkpoint\n",
               total, fixed_stages, fixes, fails, unsorted, nocheck);
        return fails || unsorted || nocheck ? 1 : 0;
    }
    fprintf(stderr, "usage: msgen show|raw|path|replay|bot|sbot|stats ... (see tools/msgen.c)\n");
    return 2;
}
