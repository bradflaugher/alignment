/* ALIGNMENT - the hand-made sectors: decoding the data tools/gen_levels.py compiles
 * from assets/levels/ (header, enemies, then the cells run-length coded, column-major).
 * The data sits in this file's ROM bank, so only this code ever reads it. */
#ifdef __SDCC
#pragma bank 255
#endif
#include <string.h>
#include "stage.h"
#include "level_data.h"

uint8_t stage_load(Stage *S, uint8_t sector) CORE_BANKED
{
    const uint8_t *d;
    uint8_t i, *c, n, t;
    uint16_t left;
    Enemy *e;
    if (sector < 1 || sector > NUM_SECTORS) return 0;
    d = &lv_data[lv_off[sector - 1]];
    S->w = d[0];
    S->start_x = d[1];
    S->start_y = d[2];
    S->check_x = d[3];
    S->check_y = d[4];
    S->exit_x = d[5];
    S->exit_y = d[6];
    S->deck = d[7];
    S->arena.kind = d[8];
    S->arena.level = d[9];
    S->arena.x0 = d[10];
    S->arena.x1 = d[11];
    S->arena.top = d[12];
    S->arena.bot = d[13];
    S->arena.bx = d[14];
    S->arena.by = d[15];
    S->arena.m0 = (uint16_t)(d[16] | (d[17] << 8));
    S->arena.m1 = (uint16_t)(d[18] | (d[19] << 8));
    S->nenemies = d[20];
    d += 21;
    for (i = 0, e = S->enemy; i < S->nenemies; i++, e++, d += 7) {
        e->kind = d[0];
        e->x0 = d[1];
        e->x1 = d[2];
        e->y0 = d[3];
        e->y1 = d[4];
        e->speed = d[5];
        e->phase = d[6];
    }
    stage_rearm(S);
    /* the cells */
    c = S->cell;
    left = (uint16_t)S->w * SH;
    S->ncells = 0;
    while (left) {
        n = d[0];
        t = d[1];
        d += 2;
        if (t == T_CELL) S->ncells = (uint8_t)(S->ncells + n);
        memset(c, t, n);
        c += n;
        left -= n;
    }
    memset(c, T_WALL, (uint16_t)(SW_MAX - S->w) * SH);
    S->sector = sector;
    S->index = sector;
    S->fixes = 0;
    S->mechs = 0;
    S->featured = 0xFF;
    return 1;
}

uint8_t sector_count(void) CORE_BANKED
{
    return NUM_SECTORS;
}

void sector_name(uint8_t sector, char *out) CORE_BANKED
{
    const char *s = "";
    if (sector >= 1 && sector <= NUM_SECTORS) s = lv_name[sector - 1];
    strcpy(out, s);
}
