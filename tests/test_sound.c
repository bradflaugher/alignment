/* test_sound.c - host-side unit tests for the ALIGNMENT sound driver.
 *
 * Build + run (from the repo root):
 *   mkdir -p build && gcc -std=c99 -O2 -Wall -Wextra -Werror -DHOST_TEST -Isrc/gb \
 *       -o build/test_sound tests/test_sound.c src/gb/sound.c src/gb/music_data.c \
 *       && ./build/test_sound
 *
 * Every register write goes through host_snd_write() (see hw_sound.h); the
 * hook below validates each one against the hardware rules and keeps a tiny
 * model of each channel (running / stopped, envelope) plus per-test stats.
 *
 * Pops: on the hardware a DAC that is on sits at a full DC offset even at
 * volume 0, so a DAC switched off / on, a channel re-routed in NR51, a jump of
 * NR50, a big drop of the CH3 volume code, an NRx2 write to a playing channel
 * ("zombie mode") and a CH3 retrigger while it plays all click (or corrupt
 * wave RAM on DMG).  PyBoy mixes digital volume and hides that, so the hook
 * counts those here.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "sound.h"
#include "music_data.h"
#include "hw_sound.h"

static int n_pass, n_fail;
#define CHECK(cond, ...) do { if (cond) n_pass++; else { n_fail++; \
    printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* ---- write monitor ---- */
static unsigned long w_total, w_bad_reg, w_bad_nrx4, w_bad_wave, w_bad_52;
static unsigned long w_music_on_owned;      /* music wrote to a channel owned by sfx */
static unsigned long w_sfx_outside;         /* sfx wrote outside its declared mask */
static unsigned long w_dac_off, w_nr30, w_nr51, w_nr50_jump, w_nr32_drop, w_env_live, w_ch3_retrig;
static unsigned long trig[4], trig_music[4], wave_loads;
static unsigned long tick_writes, max_tick_writes, sum_tick_writes, n_ticks;
static uint8_t sfx_mask_now;                /* union of masks of sfx being tested */
static int in_init;

/* channel model */
static uint8_t ch_run[4], ch_stopping[4];   /* running; length-stop issued (gone next tick) */
static uint8_t ch_env[4];                   /* envelope at the last trigger */
static unsigned long ch_t0[4];              /* tick of the last trigger */
static uint8_t env_pending[4];              /* NRx2 written, trigger expected in this tick */
static uint8_t prev_regs[0x40];

static int reg_channel(uint8_t r)
{
    if (r >= 0x10 && r <= 0x14) return 0;
    if (r >= 0x16 && r <= 0x19) return 1;
    if ((r >= 0x1A && r <= 0x1E) || (r >= 0x30 && r <= 0x3F)) return 2;
    if (r >= 0x20 && r <= 0x23) return 3;
    return -1;
}

/* loudness of an NR32 code: 0 mute .. 3 full */
static int nr32_loud(uint8_t v)
{
    switch (v & 0x60) {
    case 0x20: return 3;
    case 0x40: return 2;
    case 0x60: return 1;
    default: return 0;
    }
}

static void hook(uint8_t r, uint8_t v)
{
    int c = reg_channel(r);
    w_total++;
    tick_writes++;
    if (!((r >= 0x10 && r <= 0x26 && r != 0x15 && r != 0x1F) || (r >= 0x30 && r <= 0x3F)))
        w_bad_reg++;
    if ((r == 0x14 || r == 0x19 || r == 0x1E || r == 0x23) && (v & 0x38))
        w_bad_nrx4++;                        /* 11-bit frequency overflow / junk bits */
    if (r == 0x23 && (v & 0x3F))
        w_bad_nrx4++;
    if (r >= 0x30 && r <= 0x3F) {
        if (ch_run[2])
            w_bad_wave++;                    /* wave RAM written while CH3 runs */
        if (r == 0x30)
            wave_loads++;
    }
    if (r == 0x26 && v != 0x80)
        w_bad_52++;
    if (!in_init) {
        if ((r == 0x12 || r == 0x17 || r == 0x21) && !(v & 0xF8))
            w_dac_off++;
        if (r == 0x1A) {
            w_nr30++;                        /* DAC on/off after init */
            if (!(v & 0x80))
                w_dac_off++;
        }
        if (r == 0x25)
            w_nr51++;
        if (r == 0x24) {
            int d = (int)(v & 7) - (int)(prev_regs[0x24] & 7);
            if (d > 1 || d < -1 || (v >> 4) != (v & 7))
                w_nr50_jump++;
        }
        if (r == 0x1C && nr32_loud(prev_regs[0x1C]) - nr32_loud(v) > 1)
            w_nr32_drop++;
        if (r == 0x12 || r == 0x17 || r == 0x21)
            env_pending[c] = 1;
    }
    /* channel model */
    if (r == 0x1A && !(v & 0x80))
        ch_run[2] = ch_stopping[2] = 0;
    if (r == 0x14 || r == 0x19 || r == 0x1E || r == 0x23) {
        if (v & 0x80) {
            if (c == 2 && ch_run[2] && !in_init)
                w_ch3_retrig++;
            ch_run[c] = 1;
            ch_stopping[c] = 0;
            ch_t0[c] = n_ticks;
            env_pending[c] = 0;
            if (c != 2)
                ch_env[c] = host_snd_regs[c == 0 ? 0x12 : c == 1 ? 0x17 : 0x21];
        } else if (v & 0x40) {
            uint8_t len = host_snd_regs[c == 0 ? 0x11 : c == 1 ? 0x16 : c == 2 ? 0x1B : 0x20];
            if ((c == 2 && len == 0xFF) || (c != 2 && (len & 0x3F) == 0x3F))
                ch_stopping[c] = 1;
        }
    }
    if (c >= 0) {
        if ((r == 0x14 || r == 0x19 || r == 0x1E || r == 0x23) && (v & 0x80)) {
            trig[c]++;
            if (host_snd_src == SND_SRC_MUSIC)
                trig_music[c]++;
        }
        if (host_snd_src == SND_SRC_MUSIC && (sound_debug_owned() & (1 << c)))
            w_music_on_owned++;
        if (host_snd_src == SND_SRC_SFX && !(sfx_mask_now & (1 << c)))
            w_sfx_outside++;
    }
    if (host_snd_src == SND_SRC_SFX && c < 0)
        w_sfx_outside++;
    if (r < 0x40)
        prev_regs[r] = v;
}

/* volume a pulse / noise channel has now (model) */
static int ch_vol(int c)
{
    int vol, pace;
    if (!ch_run[c] || ch_stopping[c])
        return 0;
    vol = ch_env[c] >> 4;
    pace = ch_env[c] & 7;
    if (pace && !(ch_env[c] & 8)) {
        long steps = (long)((n_ticks - ch_t0[c]) * 64 / 60) / pace;
        vol -= (int)(steps > 15 ? 15 : steps);
        if (vol < 0)
            vol = 0;
    }
    return vol;
}

static int wave_silent(void)
{
    return !ch_run[2] || ch_stopping[2] || nr32_loud(host_snd_regs[0x1C]) == 0;
}

static void reset_stats(void)
{
    w_total = w_bad_reg = w_bad_nrx4 = w_bad_wave = w_bad_52 = 0;
    w_music_on_owned = w_sfx_outside = 0;
    w_dac_off = w_nr30 = w_nr51 = w_nr50_jump = w_nr32_drop = w_env_live = w_ch3_retrig = 0;
    wave_loads = 0;
    memset(trig, 0, sizeof trig);
    memset(trig_music, 0, sizeof trig_music);
}

static void check_clean(const char *what)
{
    CHECK(w_bad_reg == 0, "%s: %lu writes to invalid registers", what, w_bad_reg);
    CHECK(w_bad_nrx4 == 0, "%s: %lu bad NRx4 values (freq > 11 bit)", what, w_bad_nrx4);
    CHECK(w_bad_wave == 0, "%s: %lu wave RAM writes while CH3 runs", what, w_bad_wave);
    CHECK(w_bad_52 == 0, "%s: %lu bad NR52 writes", what, w_bad_52);
    CHECK(w_music_on_owned == 0, "%s: %lu music writes to sfx-owned channels", what, w_music_on_owned);
    CHECK(w_sfx_outside == 0, "%s: %lu sfx writes outside mask", what, w_sfx_outside);
    CHECK(w_dac_off == 0, "%s: %lu DACs switched off (pops)", what, w_dac_off);
    CHECK(w_nr30 == 0, "%s: %lu NR30 writes after init (pops)", what, w_nr30);
    CHECK(w_nr51 == 0, "%s: %lu NR51 re-routings (pops)", what, w_nr51);
    CHECK(w_nr50_jump == 0, "%s: %lu NR50 jumps (pops)", what, w_nr50_jump);
    CHECK(w_nr32_drop == 0, "%s: %lu CH3 volume drops of more than one step (clicks)", what, w_nr32_drop);
    CHECK(w_env_live == 0, "%s: %lu NRx2 writes without a trigger (zombie clicks)", what, w_env_live);
    CHECK(w_ch3_retrig == 0, "%s: %lu CH3 retriggers while it plays", what, w_ch3_retrig);
}

static void tick(void)
{
    int c;
    for (c = 0; c < 4; c++)
        if (ch_stopping[c])                  /* the length counter ran out (1/256 s) */
            ch_run[c] = ch_stopping[c] = 0;
    tick_writes = 0;
    sound_tick();
    n_ticks++;
    for (c = 0; c < 4; c++)
        if (env_pending[c]) {
            env_pending[c] = 0;
            w_env_live++;
        }
    if (tick_writes > max_tick_writes)
        max_tick_writes = tick_writes;
    sum_tick_writes += tick_writes;
}

static void ticks(int n)
{
    while (n-- > 0)
        tick();
}

static void fresh(void)
{
    memset(host_snd_regs, 0, sizeof host_snd_regs);
    memset(prev_regs, 0, sizeof prev_regs);
    memset(ch_run, 0, sizeof ch_run);
    memset(ch_stopping, 0, sizeof ch_stopping);
    memset(env_pending, 0, sizeof env_pending);
    in_init = 1;
    sound_init();
    in_init = 0;
    reset_stats();
}

static const char *song_names[NUM_SONGS] = { "TITLE", "DECK_A", "DECK_B", "DECK_C", "ZEN", "GAMEOVER",
    "BOSS", "CLEAR", "ENDING", "FINAL" };
static const char *sfx_names[NUM_SFX] = { "STEP", "BUMP", "FLIP", "LAND", "PUSH", "CRATE_LAND",
    "CELL", "EXIT_OPEN", "EXIT", "ZAP", "CRUSH", "BOT_STEP", "BOT_DIE", "DOOR", "REWIND", "MENU",
    "SELECT", "OPTIMAL", "NEWMECH", "HINT", "ERROR", "SCAN", "DECK",
    "SHOOT", "SHOOT_POWER", "HIT", "EXPLODE", "HURT", "ITEM", "BOMB", "BOSS_HIT", "BOSS_DIE",
    "ALARM", "TURRET", "JUMP" };

static void test_init(void)
{
    host_snd_hook = hook;
    fresh();
    CHECK(host_snd_regs[0x26] == 0x80, "NR52 on");
    CHECK(host_snd_regs[0x24] == 0x77, "NR50 = 0x77");
    CHECK(host_snd_regs[0x25] == 0xFF, "NR51 = 0xFF");
    CHECK((host_snd_regs[0x12] & 0xF8) && (host_snd_regs[0x17] & 0xF8) && (host_snd_regs[0x21] & 0xF8)
          && (host_snd_regs[0x1A] & 0x80), "all DACs on after init");
    CHECK(!music_playing(), "nothing playing after init");
    CHECK(!sfx_playing(), "no sfx after init");
    CHECK(sizeof(snd_songs) / sizeof(snd_songs[0]) == NUM_SONGS, "song table matches sound.h");
    CHECK(sizeof(snd_sfx) / sizeof(snd_sfx[0]) == NUM_SFX, "sfx table matches sound.h");
    ticks(10);
    CHECK(w_total == 0, "idle: no writes (%lu)", w_total);
    check_clean("idle");
}

static void test_songs(void)
{
    uint8_t s;
    for (s = 0; s < NUM_SONGS; s++) {
        int f, stopped_at = -1, window;
        unsigned long after;
        fresh();
        music_play(s);
        CHECK(music_playing(), "%s: playing right after music_play", song_names[s]);
        window = snd_song_frames[s] + 900;
        for (f = 0; f < window; f++) {
            tick();
            if (stopped_at < 0 && !music_playing())
                stopped_at = f;
        }
        check_clean(song_names[s]);
        CHECK(trig[0] && trig[1] && trig[2] && trig[3],
              "%s: all 4 channels used (%lu %lu %lu %lu)", song_names[s], trig[0], trig[1], trig[2], trig[3]);
        CHECK(wave_loads <= 4, "%s: wave RAM reloads kept rare (%lu)", song_names[s], wave_loads);
        if (snd_song_loops[s]) {
            CHECK(stopped_at < 0, "%s: looping song stopped at %d", song_names[s], stopped_at);
            CHECK(sound_debug_loops() >= 1, "%s: song looped (%u)", song_names[s], sound_debug_loops());
            if (s >= SONG_DECK_A && s <= SONG_DECK_C)
                CHECK(snd_song_frames[s] >= 60 * 60, "%s: deck loop is long (%u frames)",
                      song_names[s], snd_song_frames[s]);
            if (s == SONG_BOSS || s == SONG_FINAL || s == SONG_ENDING)
                CHECK(snd_song_frames[s] >= 40 * 60, "%s: loop is at least 40 s (%u frames)",
                      song_names[s], snd_song_frames[s]);
        } else {
            CHECK(stopped_at > 0, "%s: one-shot song stopped", song_names[s]);
            CHECK(stopped_at < 600, "%s: one-shot song short (%d frames)", song_names[s], stopped_at);
            if (s == SONG_CLEAR)
                CHECK(stopped_at >= 3 * 60 && stopped_at <= 5 * 60 + 10, "CLEAR: fanfare 3-5 s (%d frames)",
                      stopped_at);
            CHECK(sound_debug_loops() == 0, "%s: one-shot did not loop", song_names[s]);
            ticks(10);                       /* wave release */
            after = w_total;
            ticks(200);
            CHECK(w_total == after, "%s: no writes after end (%lu)", song_names[s], w_total - after);
            CHECK(ch_vol(0) == 0 && ch_vol(1) == 0 && ch_vol(3) == 0 && wave_silent(),
                  "%s: channels silent at end", song_names[s]);
        }
    }
    /* switching songs back and forth stays clean */
    fresh();
    for (s = 0; s < 3 * NUM_SONGS; s++) {
        music_play((uint8_t)((s * 7) % NUM_SONGS));
        ticks(97 + s * 13);
    }
    check_clean("song switching");
    CHECK(wave_loads <= 3 * NUM_SONGS, "song switching: wave reloads (%lu)", wave_loads);
}

static int run_sfx_until_done(int limit)
{
    int f = 0;
    while (f < limit) {
        tick();
        f++;
        if (!sfx_playing())
            break;
    }
    return f;
}

static void test_sfx(void)
{
    uint8_t i, with_music;
    for (with_music = 0; with_music < 2; with_music++) {
        for (i = 0; i < NUM_SFX; i++) {
            int f;
            char what[64];
            fresh();
            if (with_music) {
                music_play(SONG_DECK_A);
                ticks(37);
            }
            reset_stats();
            sfx_mask_now = snd_sfx[i].mask;
            sfx_play(i);
            CHECK(sfx_playing(), "sfx %s pending", sfx_names[i]);
            f = run_sfx_until_done(600);
            sprintf(what, "sfx %s%s", sfx_names[i], with_music ? " +music" : "");
            CHECK(f < 240, "%s: finished in %d frames", what, f);
            CHECK(f >= snd_sfx_frames[i], "%s: ran full length (%d < %u)", what, f, snd_sfx_frames[i]);
            CHECK(sound_debug_owned() == 0, "%s: channels released", what);
            CHECK(!(snd_sfx[i].mask & 4), "%s: never uses the wave channel", what);
            CHECK(host_snd_regs[0x10] == 0 || !(snd_sfx[i].mask & 1), "%s: sweep cleared", what);
            CHECK(trig[0] + trig[1] + trig[3] > 0, "%s: makes a sound", what);
            check_clean(what);
            sfx_mask_now = 0;
            if (!with_music) {
                /* nothing may keep sounding on the sfx channels */
                tick();
                if (snd_sfx[i].mask & 1) CHECK(ch_vol(0) == 0, "%s: p1 silent after", what);
                if (snd_sfx[i].mask & 2) CHECK(ch_vol(1) == 0, "%s: p2 silent after", what);
                if (snd_sfx[i].mask & 8) CHECK(ch_vol(3) == 0, "%s: noise silent after", what);
            }
        }
    }
    /* the per-move sounds are short and light */
    CHECK(snd_sfx_frames[SFX_STEP] <= 8, "STEP short (%u)", snd_sfx_frames[SFX_STEP]);
    CHECK(snd_sfx_frames[SFX_BOT_STEP] <= 8, "BOT_STEP short (%u)", snd_sfx_frames[SFX_BOT_STEP]);
    CHECK(snd_sfx_frames[SFX_SCAN] <= 6, "SCAN short (%u)", snd_sfx_frames[SFX_SCAN]);
    CHECK(snd_sfx_frames[SFX_FLIP] >= 12 && snd_sfx_frames[SFX_FLIP] <= 30, "FLIP ~0.3 s (%u)",
          snd_sfx_frames[SFX_FLIP]);
    /* combat: the gun fires ~6x a second, so shots are tiny and stay off the lead + bass */
    CHECK(snd_sfx_frames[SFX_SHOOT] >= 2 && snd_sfx_frames[SFX_SHOOT] <= 4, "SHOOT 2-4 frames (%u)",
          snd_sfx_frames[SFX_SHOOT]);
    CHECK(snd_sfx_frames[SFX_SHOOT_POWER] <= 6, "SHOOT_POWER short (%u)", snd_sfx_frames[SFX_SHOOT_POWER]);
    CHECK((snd_sfx[SFX_SHOOT].mask & ~0x0A) == 0 && (snd_sfx[SFX_SHOOT_POWER].mask & ~0x0A) == 0,
          "shots use pulse 2 / noise only (%02x %02x)", snd_sfx[SFX_SHOOT].mask, snd_sfx[SFX_SHOOT_POWER].mask);
    CHECK(snd_sfx_frames[SFX_HIT] <= 5, "HIT tiny (%u)", snd_sfx_frames[SFX_HIT]);
    CHECK(snd_sfx_frames[SFX_BOSS_HIT] <= 12, "BOSS_HIT short (%u)", snd_sfx_frames[SFX_BOSS_HIT]);
    CHECK(snd_sfx_frames[SFX_TURRET] <= 10, "TURRET short (%u)", snd_sfx_frames[SFX_TURRET]);
    CHECK(snd_sfx_frames[SFX_JUMP] <= 12 && snd_sfx_frames[SFX_JUMP] < snd_sfx_frames[SFX_FLIP],
          "JUMP short, shorter than FLIP (%u)", snd_sfx_frames[SFX_JUMP]);
    CHECK(snd_sfx_frames[SFX_EXPLODE] <= 40, "EXPLODE short (%u)", snd_sfx_frames[SFX_EXPLODE]);
    CHECK(snd_sfx_frames[SFX_BOSS_DIE] >= 75 && snd_sfx_frames[SFX_BOSS_DIE] <= 110, "BOSS_DIE ~1.5 s (%u)",
          snd_sfx_frames[SFX_BOSS_DIE]);
    CHECK(snd_sfx_frames[SFX_ALARM] >= 50 && snd_sfx_frames[SFX_ALARM] <= 75, "ALARM ~1 s (%u)",
          snd_sfx_frames[SFX_ALARM]);
    CHECK(snd_sfx[SFX_SHOOT].prio < snd_sfx[SFX_HURT].prio && snd_sfx[SFX_SHOOT].prio < snd_sfx[SFX_BOMB].prio
          && snd_sfx[SFX_BOSS_DIE].prio >= snd_sfx[SFX_BOMB].prio && snd_sfx[SFX_HIT].prio < snd_sfx[SFX_EXPLODE].prio,
          "combat priorities ordered");
}

static void test_resume(void)
{
    uint8_t s;
    uint8_t fx[3] = { SFX_CELL, SFX_OPTIMAL, SFX_HINT };
    for (s = 0; s < 3; s++) {
        uint8_t m = snd_sfx[fx[s]].mask, c;
        int f;
        fresh();
        music_play(SONG_TITLE);
        ticks(900);                          /* into the loop: all channels busy */
        sfx_mask_now = m;
        sfx_play(fx[s]);
        run_sfx_until_done(600);
        sfx_mask_now = 0;
        reset_stats();
        for (f = 0; f < 300; f++)
            tick();
        for (c = 0; c < 4; c++)
            if (m & (1 << c))
                CHECK(trig_music[c] > 0, "resume: music retriggers ch%u after sfx %s", c, sfx_names[fx[s]]);
        check_clean("resume");
    }
}

/* important sounds are never cut by the per-turn ones */
static void test_priorities(void)
{
    static const uint8_t big[] = { SFX_ZAP, SFX_CRUSH, SFX_EXIT, SFX_OPTIMAL, SFX_DECK, SFX_BOT_DIE,
                                   SFX_HURT, SFX_BOMB, SFX_BOSS_DIE, SFX_ALARM, SFX_ITEM };
    static const uint8_t small[] = { SFX_STEP, SFX_BOT_STEP, SFX_SCAN, SFX_PUSH, SFX_BUMP, SFX_MENU,
                                     SFX_SHOOT, SFX_SHOOT_POWER, SFX_HIT, SFX_TURRET, SFX_BOSS_HIT };
    unsigned b, k;
    for (b = 0; b < sizeof big; b++) {
        for (k = 0; k < sizeof small; k++) {
            int f, lost = 0;
            fresh();
            music_play(SONG_DECK_B);
            ticks(5);
            sfx_mask_now = 0x0B;
            sfx_play(big[b]);
            ticks(2);
            for (f = 0; f < snd_sfx_frames[big[b]] - 4; f++) {
                if ((f & 3) == 0)
                    sfx_play(small[k]);
                tick();
                if ((sound_debug_owned() & snd_sfx[big[b]].mask) != snd_sfx[big[b]].mask)
                    lost++;
            }
            CHECK(lost == 0, "%s keeps its channels against %s (%d frames lost)",
                  sfx_names[big[b]], sfx_names[small[k]], lost);
            run_sfx_until_done(600);
            sfx_mask_now = 0;
            CHECK(sound_debug_owned() == 0, "released");
            check_clean("priorities");
        }
        CHECK(snd_sfx[big[b]].prio > snd_sfx[SFX_STEP].prio && snd_sfx[big[b]].prio > snd_sfx[SFX_BOT_STEP].prio,
              "%s outranks STEP/BOT_STEP", sfx_names[big[b]]);
    }
    /* STEP + BOT_STEP in the same frame (every turn): both heard or layered */
    fresh();
    music_play(SONG_DECK_A);
    ticks(5);
    sfx_mask_now = 0x0B;
    sfx_play(SFX_STEP);
    sfx_play(SFX_BOT_STEP);
    tick();
    CHECK((snd_sfx[SFX_STEP].mask & snd_sfx[SFX_BOT_STEP].mask) == 0 ?
          sound_debug_owned() == (snd_sfx[SFX_STEP].mask | snd_sfx[SFX_BOT_STEP].mask) : 1,
          "STEP + BOT_STEP layer (owned %02x)", sound_debug_owned());
    run_sfx_until_done(100);
    /* CELL + EXIT_OPEN layer (the last cell opens the exit) */
    sfx_play(SFX_CELL);
    sfx_play(SFX_EXIT_OPEN);
    ticks(2);
    CHECK(sound_debug_owned() == (snd_sfx[SFX_CELL].mask | snd_sfx[SFX_EXIT_OPEN].mask),
          "CELL+EXIT_OPEN layered (owned %02x)", sound_debug_owned());
    run_sfx_until_done(600);
    CHECK(sound_debug_owned() == 0, "layered released");
    /* SCAN / STEP spam settles quickly */
    {
        int k2;
        for (k2 = 0; k2 < 60; k2++) { sfx_play(SFX_SCAN); ticks(2); }
        CHECK(run_sfx_until_done(100) < 10, "SCAN spam settles");
        for (k2 = 0; k2 < 60; k2++) { sfx_play(SFX_STEP); sfx_play(SFX_BOT_STEP); ticks(3); }
        CHECK(run_sfx_until_done(100) < 12, "STEP spam settles");
    }
    sfx_mask_now = 0;
    check_clean("priorities");
}

/* combat: the gun fires ~6x a second over the boss theme; the shot only ever
   borrows its own channel, lead / bass / drums keep playing, and the arps get
   their channel back between shots.  Enemy hits / kills layer on the noise. */
static void test_gun(void)
{
    int f, max_owned_frames = 0, run = 0;
    unsigned long p2_music;
    fresh();
    music_play(SONG_BOSS);
    ticks(400);
    reset_stats();
    sfx_mask_now = snd_sfx[SFX_SHOOT].mask | snd_sfx[SFX_SHOOT_POWER].mask;
    for (f = 0; f < 600; f++) {
        if (f % 10 == 0)
            sfx_play((f / 10) % 4 == 3 ? SFX_SHOOT_POWER : SFX_SHOOT);
        tick();
        CHECK((sound_debug_owned() & ~sfx_mask_now) == 0, "gun: only the shot channel owned (%02x)",
              sound_debug_owned());
        run = (sound_debug_owned() & 2) ? run + 1 : 0;
        if (run > max_owned_frames)
            max_owned_frames = run;
    }
    CHECK(max_owned_frames <= 6, "gun: a shot holds pulse 2 at most 6 frames (%d)", max_owned_frames);
    /* (the wave channel is never retriggered while it runs: check it is still sounding) */
    CHECK(trig_music[0] > 20 && trig_music[3] > 20 && !wave_silent(),
          "gun: lead/bass/drums keep playing (%lu %lu)", trig_music[0], trig_music[3]);
    p2_music = trig_music[1];
    CHECK(p2_music > 20, "gun: arps come back between shots (%lu)", p2_music);
    check_clean("gun");
    /* shots + enemy hits + kills + turret fire at once: all layered, all settle */
    reset_stats();
    sfx_mask_now = 0x0B;
    for (f = 0; f < 600; f++) {
        if (f % 10 == 0) sfx_play(SFX_SHOOT);
        if (f % 10 == 3) sfx_play(SFX_HIT);
        if (f % 45 == 7) sfx_play(SFX_EXPLODE);
        if (f % 37 == 11) sfx_play(SFX_TURRET);
        if (f % 23 == 5) sfx_play(SFX_BOSS_HIT);
        tick();
    }
    CHECK(run_sfx_until_done(200) < 60, "combat spam settles");
    CHECK(sound_debug_owned() == 0, "combat spam: channels released");
    sfx_mask_now = 0;
    check_clean("combat spam");
    /* the boss dies while the player keeps shooting: the death is never cut */
    reset_stats();
    sfx_mask_now = 0x0B;
    sfx_play(SFX_BOSS_DIE);
    ticks(2);
    for (f = 0; f < snd_sfx_frames[SFX_BOSS_DIE] - 4; f++) {
        if (f % 10 == 0) sfx_play(SFX_SHOOT);
        if (f % 7 == 0) sfx_play(SFX_BOSS_HIT);
        tick();
        CHECK((sound_debug_owned() & snd_sfx[SFX_BOSS_DIE].mask) == snd_sfx[SFX_BOSS_DIE].mask,
              "BOSS_DIE keeps its channels (frame %d, owned %02x)", f, sound_debug_owned());
    }
    run_sfx_until_done(300);
    sfx_mask_now = 0;
    CHECK(sound_debug_owned() == 0, "boss death released");
    check_clean("boss death");
}

static void test_stop(void)
{
    unsigned long before;
    fresh();
    music_play(SONG_DECK_B);
    ticks(500);
    music_stop();
    CHECK(!music_playing(), "music_playing()==0 right after stop");
    ticks(6);
    CHECK(ch_vol(0) == 0 && ch_vol(1) == 0 && ch_vol(3) == 0, "stop: pulses + noise silent");
    CHECK(wave_silent(), "stop: wave released");
    CHECK((host_snd_regs[0x12] & 0xF8) && (host_snd_regs[0x17] & 0xF8) && (host_snd_regs[0x21] & 0xF8)
          && (host_snd_regs[0x1A] & 0x80), "stop: DACs stay on");
    before = w_total;
    ticks(300);
    CHECK(w_total == before, "stop: no writes while stopped (%lu)", w_total - before);
    /* stop while an sfx owns p1: sfx finishes, channel ends silent */
    music_play(SONG_TITLE);
    ticks(300);
    sfx_mask_now = snd_sfx[SFX_FLIP].mask;
    sfx_play(SFX_FLIP);
    ticks(3);
    music_stop();
    run_sfx_until_done(600);
    sfx_mask_now = 0;
    tick();
    CHECK(ch_vol(0) == 0, "stop during sfx: p1 silent after sfx");
    /* restart works */
    music_play(SONG_TITLE);
    ticks(2);
    CHECK(music_playing(), "restart after stop");
    CHECK(trig[0] + trig[1] + trig[2] + trig[3] > 0, "restart triggers notes");
    check_clean("stop");
}

static uint16_t reg_freq(uint8_t lo_reg)
{
    return (uint16_t)(host_snd_regs[lo_reg] | ((host_snd_regs[lo_reg + 1] & 7) << 8));
}

static int in_table(uint16_t f)
{
    int i;
    for (i = 0; i < SND_NUM_NOTES; i++)
        if (snd_freq[i] == f)
            return 1;
    return 0;
}

static void test_effects(void)
{
    int f, lower = 0, total = 0;
    uint8_t s;
    for (s = 0; s < NUM_SONGS; s++) {
        fresh();
        music_play(s);
        ticks(200);
        music_set_rewind(1);
        for (f = 0; f < 400; f++) {
            tick();
            sfx_mask_now = 0x0B;
            if (s != SONG_DECK_A && (f & 31) == 5)
                sfx_play((uint8_t)(f % NUM_SFX));   /* sfx during rewind must not break */
            if (s == SONG_DECK_A && ch_run[2] && !ch_stopping[2]) {
                total++;
                if (!in_table(reg_freq(0x1D)))
                    lower++;
            }
        }
        music_set_rewind(0);
        run_sfx_until_done(600);
        sfx_mask_now = 0;
        check_clean("rewind");
        if (s == SONG_DECK_A)
            CHECK(lower > total / 2, "rewind detunes the bass (%d of %d frames off-table)", lower, total);
    }
    /* after rewind off, the bass is back on the table */
    fresh();
    music_play(SONG_DECK_A);
    ticks(100);
    music_set_rewind(1);
    ticks(300);
    music_set_rewind(0);
    ticks(2);
    CHECK(in_table(reg_freq(0x1D)), "rewind off restores wave pitch (%u)", reg_freq(0x1D));
    {   /* tempo slows while rewinding: count drum triggers */
        unsigned long a, b;
        reset_stats(); ticks(600); a = trig[3] + trig[1];
        music_set_rewind(1); reset_stats(); ticks(600); b = trig[3] + trig[1];
        music_set_rewind(0);
        CHECK(b < a, "rewind slows tempo (%lu vs %lu triggers)", b, a);
    }
    check_clean("rewind off");

    /* muffle: lead + drums held (cut, no notes), NR50 ramps down, NR51 untouched */
    fresh();
    music_play(SONG_TITLE);
    ticks(600);
    music_set_muffle(1);
    ticks(10);
    reset_stats();
    ticks(400);
    CHECK(trig_music[0] == 0 && trig_music[3] == 0, "muffle drops lead+drums (%lu %lu)", trig_music[0], trig_music[3]);
    CHECK(trig_music[1] > 0, "muffle keeps arps (%lu)", trig_music[1]);
    CHECK(ch_vol(0) == 0 && ch_vol(3) == 0, "muffled channels silent");
    CHECK(host_snd_regs[0x24] < 0x77 && host_snd_regs[0x25] == 0xFF, "muffle lowers NR50 only");
    sfx_mask_now = snd_sfx[SFX_MENU].mask;
    sfx_play(SFX_MENU);
    tick();
    CHECK(trig[0] > 0 || trig[1] > 0, "menu sfx audible while muffled");
    run_sfx_until_done(100);
    sfx_mask_now = 0;
    reset_stats();
    ticks(100);
    CHECK(trig_music[0] == 0, "p1 muffled again after sfx");
    music_set_muffle(0);
    ticks(10);
    CHECK(host_snd_regs[0x24] == 0x77, "unmuffle restores NR50");
    reset_stats();
    ticks(400);
    CHECK(trig_music[0] > 0 && trig_music[3] > 0, "unmuffle: lead + drums back");
    check_clean("muffle");

    /* mute: everything held and silent, NR50 ramps to 0, sfx dropped */
    sound_mute_all(1);
    ticks(10);
    CHECK((host_snd_regs[0x24] & 7) == 0, "mute: NR50 ramped down");
    reset_stats();
    sfx_play(SFX_ZAP);
    ticks(50);
    CHECK(trig[0] + trig[1] + trig[2] + trig[3] == 0, "mute: nothing triggers");
    CHECK(ch_vol(0) == 0 && ch_vol(1) == 0 && ch_vol(3) == 0 && wave_silent(), "mute: silent");
    CHECK(music_playing(), "music keeps running while muted");
    sound_mute_all(0);
    ticks(10);
    CHECK(host_snd_regs[0x24] == 0x77 && host_snd_regs[0x25] == 0xFF, "unmute restores mix");
    reset_stats();
    ticks(300);
    CHECK(trig[0] && trig[1] && trig[3], "unmute: music back");
    check_clean("mute");
}

static void test_stress(void)
{
    int f;
    unsigned int seed = 12345;
    fresh();
    for (f = 0; f < 40000; f++) {
        unsigned int r;
        seed = seed * 1103515245u + 12345u;
        r = (seed >> 16) & 0x7FFF;
        if (r % 97 == 0) music_play((uint8_t)(r % (NUM_SONGS + 2)));  /* includes invalid ids */
        if (r % 13 == 0) sfx_play((uint8_t)(r % (NUM_SFX + 3)));
        if (r % 211 == 0) music_set_rewind((uint8_t)(r & 1));
        if (r % 307 == 0) music_set_muffle((uint8_t)((r >> 1) & 1));
        if (r % 509 == 0) sound_mute_all((uint8_t)((r >> 2) & 1));
        if (r % 1009 == 0) music_stop();
        sfx_mask_now = 0x0B;               /* any sfx channel (never wave) */
        tick();
    }
    sfx_mask_now = 0;
    CHECK(w_bad_reg == 0 && w_bad_nrx4 == 0 && w_bad_wave == 0 && w_ch3_retrig == 0, "stress: register rules hold");
    CHECK(w_dac_off == 0 && w_nr30 == 0 && w_nr51 == 0 && w_nr50_jump == 0 && w_nr32_drop == 0 && w_env_live == 0,
          "stress: no pops (%lu %lu %lu %lu %lu %lu)", w_dac_off, w_nr30, w_nr51, w_nr50_jump, w_nr32_drop, w_env_live);
    CHECK(w_music_on_owned == 0, "stress: music never writes owned channels");
    CHECK(w_sfx_outside == 0, "stress: sfx never touch wave channel");
    music_set_rewind(0);
    music_set_muffle(0);
    sound_mute_all(0);
    CHECK(run_sfx_until_done(600) < 600, "stress: sfx drain");
}

static void test_cost(void)
{
    /* cycle proxy: register writes per tick (the real cost is measured on the
       emulated CPU by tools/render_audio.py --bench) */
    uint8_t s;
    max_tick_writes = sum_tick_writes = 0;
    {
        unsigned long t0 = n_ticks;
        for (s = 0; s < NUM_SONGS; s++) {
            fresh();
            music_play(s);
            ticks(3000);
        }
        printf("cost proxy: %.2f register writes/tick avg, %lu max\n",
               (double)sum_tick_writes / (double)(n_ticks - t0), max_tick_writes);
        CHECK(max_tick_writes <= 48, "at most 48 writes in one tick (%lu)", max_tick_writes);
        CHECK(sum_tick_writes < (n_ticks - t0) * 6, "avg < 6 writes per tick");
    }
}

int main(void)
{
    host_snd_hook = hook;
    test_init();
    test_songs();
    test_sfx();
    test_resume();
    test_priorities();
    test_gun();
    test_stop();
    test_effects();
    test_stress();
    test_cost();
    printf("test_sound: %d passed, %d failed\n", n_pass, n_fail);
    return n_fail ? 1 : 0;
}
