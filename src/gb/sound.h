/* sound.h - ALIGNMENT sound driver (music + sfx) for GBDK-2020.
 *
 * Channel use:  pulse 1 = lead, pulse 2 = arps/harmony, wave = bass,
 *               noise = drums.  SFX temporarily steal pulse 1 / pulse 2 /
 *               noise (never the wave channel); music keeps running silently
 *               on a stolen channel and resumes cleanly when the sfx ends.
 *
 * Click-free on real hardware: every DAC is switched on once in sound_init()
 * and never off, NR51 is written once and never changes, NR50 moves one step
 * per frame, notes are cut with the length counter (not NRx2 = 0), the wave
 * channel is never retriggered while it runs and its volume only falls one
 * step per frame.  See the top of sound.c.
 *
 * Threading: every public call only posts a request (single-byte writes /
 * a 4-entry lock-free queue); all register work happens inside sound_tick(),
 * so it is safe to call these from the main loop while sound_tick() runs in
 * the VBL interrupt.  Requests take effect on the next sound_tick().
 */
#ifndef SOUND_H
#define SOUND_H

#include <stdint.h>

enum { SONG_TITLE, SONG_DECK_A, SONG_DECK_B, SONG_DECK_C, SONG_ZEN, SONG_GAMEOVER, NUM_SONGS };
enum { SFX_STEP, SFX_BUMP, SFX_FLIP, SFX_LAND, SFX_PUSH, SFX_CRATE_LAND, SFX_CELL,
       SFX_EXIT_OPEN, SFX_EXIT, SFX_ZAP, SFX_CRUSH, SFX_BOT_STEP, SFX_BOT_DIE, SFX_DOOR,
       SFX_REWIND, SFX_MENU, SFX_SELECT, SFX_OPTIMAL, SFX_NEWMECH, SFX_HINT, SFX_ERROR,
       SFX_SCAN, SFX_DECK, NUM_SFX };

void sound_init(void);            /* enable APU, DACs on, NR51 = 0xFF (once), init state */
void sound_tick(void);            /* call exactly once per frame (60Hz), e.g. from VBL */
void music_play(uint8_t song);    /* start song from the beginning (restarts if same) */
void music_stop(void);
uint8_t music_playing(void);      /* 0 if stopped / non-looping song finished */
void sfx_play(uint8_t sfx);       /* play sfx (steals its channels from music) */
void music_set_rewind(uint8_t on);/* time-rewind effect: detune/warble down + slow down */
void music_set_muffle(uint8_t on);/* pause effect: drop lead+drums, lower master volume */
void sound_mute_all(uint8_t on);  /* global mute: music keeps running silently, sfx are dropped */

/* --- introspection helpers (cheap; used by tests / debug) --- */
uint8_t sound_debug_loops(void);  /* times the current song wrapped to its loop point */
uint8_t sound_debug_owned(void);  /* channel mask currently owned by sfx (bit0=p1,1=p2,3=noise) */
uint8_t sfx_playing(void);        /* nonzero while any sfx is playing / pending */

#endif
