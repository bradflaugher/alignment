#ifndef AL_SAVE_H
#define AL_SAVE_H
#include <stdint.h>
#include <gb/gb.h>
#include "../core/gen.h"

typedef struct {
    uint8_t magic[2];       /* 'A','L' */
    uint8_t version;
    uint16_t run_best_room; /* furthest room reached in a RUN */
    uint16_t run_best_opt;  /* OPTIMAL clears in that run */
    uint16_t zen_seed;
    Door zen_door;          /* where the ZEN station walk resumes */
    uint16_t total_opt;     /* lifetime OPTIMAL clears */
    uint16_t total_rooms;   /* lifetime rooms cleared */
    uint8_t checksum;
} SaveData;

extern SaveData save;
void save_load(void) BANKED;   /* loads or initialises */
void save_write(void) BANKED;
uint8_t save_checksum(const SaveData *s) BANKED;
#endif
