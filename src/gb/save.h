#ifndef AL_SAVE_H
#define AL_SAVE_H
#include <stdint.h>
#include <gb/gb.h>

typedef struct {
    uint8_t magic[2];       /* 'A','L' */
    uint8_t version;
    uint16_t best_stage;    /* furthest stage reached in ENDLESS */
    uint32_t best_score;
    uint8_t sector;         /* the furthest campaign sector reached (1..) */
    uint8_t campaign_done;  /* the last sector has been cleared */
    uint16_t total_cells;   /* lifetime energy cells */
    uint16_t total_stages;  /* lifetime stages cleared */
    uint8_t checksum;
} SaveData;

extern SaveData save;
void save_load(void) BANKED;   /* loads or initialises */
void save_write(void) BANKED;
uint8_t save_checksum(const SaveData *s) BANKED;
#endif
