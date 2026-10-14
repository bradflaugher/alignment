/* sound.c - ALIGNMENT sound driver (derived from the INFINITE BEST driver).
 *
 * Music: 4 channels, each running its own order list of (pattern, transpose)
 * pairs.  Patterns are byte streams (see music_data.h / tools/gen_music.py):
 * notes, rests, holds, length / instrument / arpeggio / glide commands.
 * Timing is in rows; a row lasts speed_a / speed_b frames alternately
 * (groove).  Per-frame effects: arpeggio, glide, vibrato, duty macro, wave
 * level decay, auto-cut, drum NR43 macros, and the global rewind warble.
 *
 * SFX: two voices, each playing a register-write script from snd_sfx[].  An
 * sfx owns the channels in its mask; music keeps updating its state for an
 * owned channel but does not touch the hardware, and when the sfx ends the
 * channel is handed back (sweep cleared, sustained notes re-triggered,
 * decaying ones silenced until the next music note).
 *
 * No pops on real hardware.  A channel whose DAC is on sits at a full DC
 * offset even at volume 0, so anything that moves that offset without a note
 * clicks (PyBoy mixes digital volume and hides all of this; tests/test_sound.c
 * checks the register stream instead):
 *   - every DAC is switched on once in sound_init() and stays on: a note is
 *     cut by loading the length counter with 1 (NRx1 = 0x3F, NR31 = 0xFF) and
 *     enabling it (NRx4 bit 6, no trigger), never by NRx2 = 0 / NR30 = 0;
 *   - NRx2 is only written immediately before a trigger of that channel (a
 *     write to a playing channel is DMG "zombie mode": a volume jump);
 *   - NR51 is written once and never changes; muffle / mute hold channels
 *     (cut, then left alone) instead of re-routing them, and NR50 ramps one
 *     step per frame;
 *   - the wave channel is never retriggered while it runs (DMG wave RAM
 *     corruption + phase reset): a new bass note on a running channel only
 *     changes NR32/NR33/NR34 without the trigger bit; its volume falls at most
 *     one step per frame (100 -> 50 -> 25 -> 0%), then the length counter
 *     stops it; wave RAM is only rewritten while the channel is stopped, and
 *     only when a different wavetable is needed.
 *
 * Performance: SDCC generates very slow code for struct-pointer access on
 * the SM83, so every channel's state lives in its own plain globals and the
 * per-channel functions are stamped out 4 times from the template at the
 * bottom of this file (this file includes itself with SND_CH = 0..3).  All
 * channel state is then accessed at constant addresses, and pulse/wave/noise
 * specialisations are resolved at compile time.  No multiply / divide; the
 * only loops in the hot path are small shift loops.
 */
#ifndef SND_CH
/* ======================================================================= */
#include "sound.h"
#include "music_data.h"
#include "hw_sound.h"

#ifdef HOST_TEST
uint8_t host_snd_regs[0x40];
host_snd_hook_t host_snd_hook;
uint8_t host_snd_src;
void host_snd_write(uint8_t reg, uint8_t val)
{
    if (reg < 0x40)
        host_snd_regs[reg] = val;
    if (host_snd_hook)
        host_snd_hook(reg, val);
}
#endif

/* channel flags */
#define F_GATE   0x01   /* a note is sounding */
#define F_ENDED  0x02   /* order list finished */
#define F_GLIDE  0x04   /* current note is gliding */
#define F_GLNEXT 0x08   /* next note glides */
#define F_PFX    0x10   /* per-frame pitch work needed (arp / glide / vib) */
#define F_DIRTY  0x20   /* rewrite pitch next frame */

#define REQ_NONE 0xFF
#define REQ_STOP 0xFE

static const snd_song_t *song;
static uint8_t music_on, tick_ctr, groove, loops;
static uint8_t owned;                       /* channels owned by sfx */
static uint8_t mfm;                         /* channels held by muffle / mute */
static uint8_t hold;                        /* owned | mfm: music leaves the hardware alone */
static uint8_t n50;                         /* NR50 level 0..7 (ramps one step per frame) */
static uint8_t wave_loaded;
/* wave channel hardware state */
static uint8_t w_run;                       /* CH3 triggered and not stopped */
static uint8_t w_hw;                        /* NR32 code on the hardware: 1=100% 2=50% 3=25% 0=mute */
static uint8_t w_rel;                       /* 0 idle, 1 releasing (a step per frame) */
static uint8_t w_pend;                      /* a note waits for CH3 to stop (new wavetable) */
static uint8_t w_hi;                        /* NR34 frequency bits last written */
static uint8_t fx_rewind, fx_muffle, fx_mute;
static uint8_t rw_ph, rw_skip, rw_t;
/* sfx voices */
static const uint8_t *vo_p[2];
static uint8_t vo_wait[2], vo_mask[2], vo_prio[2];
/* requests from the main thread */
static volatile uint8_t req_song, req_rewind, req_muffle, req_mute;
static volatile uint8_t rq[4];
static volatile uint8_t rq_head, rq_tail;

static const uint8_t pat_empty[1] = { SND_CMD_END };
static const uint8_t rw_tri[8] = { 0, 1, 2, 3, 3, 2, 1, 0 };

static uint16_t rw_xform(uint16_t f)
{
    /* rewind: lower the pitch by ~1..2.5 semitones with a slow warble.
     * period p = 2048 - f; new period = p + (2 + t) * (p / 32), t = 0..3
     * (rw_t, set once per tick).  p / 32 fits a byte: cheap on the SM83. */
    uint16_t p = (uint16_t)(2048u - f);
    uint8_t e = (uint8_t)(((uint8_t)(p >> 8) << 3) | ((uint8_t)p >> 5));
    uint16_t d = (uint16_t)e << 1;
    uint8_t t = rw_t;
    while (t--)
        d += e;
    if (d >= f)
        return 0;
    return (uint16_t)(f - d);
}

static uint8_t drum_poly(uint8_t v)
{
    if (fx_rewind && v < 0xD0)
        v += 0x10;          /* one clock-shift lower while rewinding */
    return v;
}

/* Only while CH3 is stopped (never triggered, or halted by its length
 * counter at least one frame ago).  The DAC stays on. */
static void load_wave(uint8_t w)
{
    const uint8_t *s = snd_waves[w];
    uint8_t i;
    for (i = 0; i < 16; i++)
        SND_W(SND_WAVE + i, s[i]);
    wave_loaded = w;
}

/* ---- stamp out the per-channel code ---- */
#define SND_CH 0
#include "sound.c"
#undef SND_CH
#define SND_CH 1
#include "sound.c"
#undef SND_CH
#define SND_CH 2
#include "sound.c"
#undef SND_CH
#define SND_CH 3
#include "sound.c"
#undef SND_CH

/* ------------------------------------------------------------------ */

/* Wave release: one NR32 step per frame down to mute, then stop the channel
 * with its length counter (gone within 1/256 s, i.e. before the next tick).
 * A note that needs a new wavetable waits for this (w_pend), then triggers. */
static void wave_release(void)
{
    if (w_rel == 2) {                       /* stopped now: play the waiting note */
        w_rel = 0;
        if (w_pend && music_on && (c2_flags & F_GATE) && !(hold & 0x04))
            trigger_2();
        w_pend = 0;
        return;
    }
    if (w_hw) {
        w_hw = (w_hw == 3) ? 0 : (uint8_t)(w_hw + 1);
        SND_W(SND_NR32, (uint8_t)(w_hw << 5));
        if (w_hw)
            return;
    }
    SND_W(SND_NR31, 0xFF);
    SND_W(SND_NR34, (uint8_t)(0x40 | w_hi));
    w_run = 0;
    w_rel = w_pend ? 2 : 0;
}

/* Cut the channels in m (the music's view of them is kept). */
static void cut_mask(uint8_t m)
{
    if (m & 1) silence_0();
    if (m & 2) silence_1();
    if (m & 4) silence_2();
    if (m & 8) silence_3();
}

/* Hand channels in m back to the music (sustained notes re-trigger). */
static void restore_mask(uint8_t m)
{
    if (m & 1) restore_0();
    if (m & 2) restore_1();
    if (m & 4) restore_2();
    if (m & 8) restore_3();
}

/* Muffle / mute: hold channels instead of re-routing NR51 (a re-route pops). */
static void set_hold(uint8_t m)
{
    uint8_t on = (uint8_t)(m & ~mfm & ~owned);
    uint8_t off = (uint8_t)(mfm & ~m & ~owned);
    mfm = m;
    hold = (uint8_t)(owned | m);
    cut_mask(on);
    restore_mask(off);
}

static void silence_music(void)
{
    kill_0();
    kill_1();
    kill_2();
    kill_3();
}

/* Give channels back to the music (or keep them cut while muffled / muted). */
static void release(uint8_t m)
{
    owned &= (uint8_t)~m;
    hold = (uint8_t)(owned | mfm);
    if (m & 1)
        SND_W(SND_NR10, 0x00);              /* sweep off */
    cut_mask((uint8_t)(m & mfm));
    restore_mask((uint8_t)(m & ~mfm));
}

static void voice_stop(uint8_t i, uint8_t keep)
{
    uint8_t m = (uint8_t)(vo_mask[i] & ~keep);
    vo_p[i] = 0;
    release(m);
}

static void sfx_start(uint8_t id)
{
    const snd_sfx_t *s = &snd_sfx[id];
    uint8_t m = s->mask;
    uint8_t pr = s->prio;
    uint8_t i;
    if (fx_mute)
        return;                             /* muted: sfx are dropped */
    for (i = 0; i < 2; i++)
        if (vo_p[i] && (vo_mask[i] & m) && vo_prio[i] > pr)
            return;                         /* busy with something more important */
    for (i = 0; i < 2; i++)
        if (vo_p[i] && (vo_mask[i] & m))
            voice_stop(i, m);
    i = 0;
    if (vo_p[0]) {
        i = 1;
        if (vo_p[1]) {                      /* both busy on other channels: evict lower prio */
            if (vo_prio[0] <= vo_prio[1])
                i = 0;
            voice_stop(i, 0);
        }
    }
    vo_p[i] = s->data;
    vo_wait[i] = 0;
    vo_mask[i] = m;
    vo_prio[i] = pr;
    owned |= m;
    hold |= m;
}

static void voice_run(uint8_t i)
{
    const uint8_t *p = vo_p[i];
    uint8_t b;
    if (!p)
        return;
    if (vo_wait[i]) {
        vo_wait[i]--;
        return;
    }
    SND_SRC(SND_SRC_SFX);
    for (;;) {
        b = *p++;
        if (b == 0) {
            SND_SRC(SND_SRC_MUSIC);
            voice_stop(i, 0);
            return;
        }
        if (b < 0x40) {
            SND_W(b, *p);
            p++;
            continue;
        }
        vo_wait[i] = (uint8_t)(b - 0x40);
        break;
    }
    SND_SRC(SND_SRC_MUSIC);
    vo_p[i] = p;
}

/* ------------------------------------------------------------------ */

void sound_init(void)
{
    SND_SRC(SND_SRC_MUSIC);
    SND_W(SND_NR52, 0x80);
    SND_W(SND_NR50, 0x77);
    SND_W(SND_NR51, 0xFF);                  /* the only NR51 write, ever */
    SND_W(SND_NR10, 0x00);
    /* every DAC on (volume 0) once, here, and never off again; channels stopped */
    SND_W(SND_NR12, 0x08);
    SND_W(SND_NR11, 0x3F);
    SND_W(SND_NR14, 0x40);
    SND_W(SND_NR22, 0x08);
    SND_W(SND_NR21, 0x3F);
    SND_W(SND_NR24, 0x40);
    SND_W(SND_NR42, 0x08);
    SND_W(SND_NR41, 0x3F);
    SND_W(SND_NR44, 0x40);
    SND_W(SND_NR30, 0x00);                  /* stop CH3 so wave RAM may be written */
    load_wave(0);
    SND_W(SND_NR32, 0x00);
    SND_W(SND_NR30, 0x80);
    c0_flags = c1_flags = c2_flags = c3_flags = F_ENDED;
    c0_ip = c1_ip = c2_ip = &snd_insts[0];
    c0_freq = c1_freq = c2_freq = 0;
    c0_ms = c1_ms = 0;
    vo_p[0] = vo_p[1] = 0;
    music_on = 0;
    owned = mfm = hold = 0;
    n50 = 7;
    w_run = w_hw = w_rel = w_pend = w_hi = 0;
    loops = 0;
    fx_rewind = fx_muffle = fx_mute = 0;
    req_rewind = req_muffle = req_mute = 0;
    rw_ph = rw_skip = rw_t = 0;
    rq_head = rq_tail = 0;
    req_song = REQ_NONE;
}

void sound_tick(void)
{
    uint8_t r;
    SND_BANK_PUSH();                /* all song / sfx data lives in a ROM bank */

    /* ---- requests ---- */
    r = req_song;
    if (r != REQ_NONE) {
        req_song = REQ_NONE;
        music_on = 0;
        silence_music();
        if (r != REQ_STOP) {
            song = &snd_songs[r];
            start_0();
            start_1();
            start_2();
            start_3();
            loops = 0;
            tick_ctr = 1;
            groove = 1;
            music_on = 1;
        }
    }
    r = req_rewind;
    if (r != fx_rewind) {
        fx_rewind = r;
        rw_ph = 0;
        rw_skip = 0;
        rw_t = 0;
        c0_flags |= F_DIRTY;
        c1_flags |= F_DIRTY;
        c2_flags |= F_DIRTY;
    }
    if (req_muffle != fx_muffle || req_mute != fx_mute) {
        fx_muffle = req_muffle;
        fx_mute = req_mute;
        set_hold(fx_mute ? 0x0F : (fx_muffle ? 0x09 : 0x00));
        if (fx_mute) {                      /* drop sfx (their channels stay cut) */
            if (vo_p[0])
                voice_stop(0, 0);
            if (vo_p[1])
                voice_stop(1, 0);
            rq_tail = rq_head;
        }
    }
    r = fx_mute ? 0 : (fx_muffle ? 4 : 7);  /* NR50: one step per frame */
    if (n50 != r) {
        if (n50 < r)
            n50++;
        else
            n50--;
        SND_W(SND_NR50, (uint8_t)(n50 | (n50 << 4)));
    }
    while (rq_tail != rq_head) {
        sfx_start(rq[rq_tail]);
        rq_tail = (uint8_t)((rq_tail + 1) & 3);
    }

    /* ---- music ---- */
    if (music_on) {
        frame_0();
        frame_1();
        frame_2();
        frame_3();
        r = 1;
        if (fx_rewind) {
            rw_ph++;
            if (!(rw_ph & 7)) {             /* warble step: re-pitch held notes */
                rw_t = rw_tri[(rw_ph >> 3) & 7];
                c0_flags |= F_DIRTY;
                c1_flags |= F_DIRTY;
                c2_flags |= F_DIRTY;
            }
            if (++rw_skip == 3) {           /* rewind: 2/3 tempo */
                rw_skip = 0;
                r = 0;
            }
        }
        if (r && --tick_ctr == 0) {
            groove ^= 1;
            tick_ctr = groove ? song->speed_b : song->speed_a;
            if (!(c0_flags & F_ENDED) && --c0_wait == 0) row_0();
            if (!(c1_flags & F_ENDED) && --c1_wait == 0) row_1();
            if (!(c2_flags & F_ENDED) && --c2_wait == 0) row_2();
            if (!(c3_flags & F_ENDED) && --c3_wait == 0) row_3();
            if (c0_flags & c1_flags & c2_flags & c3_flags & F_ENDED)
                music_on = 0;               /* one-shot song finished */
        }
    }

    /* ---- sfx ---- */
    if (vo_p[0])
        voice_run(0);
    if (vo_p[1])
        voice_run(1);
    if (w_rel)
        wave_release();
    SND_BANK_POP();
}

void music_play(uint8_t s)
{
    if (s < NUM_SONGS)
        req_song = s;
}

void music_stop(void)
{
    req_song = REQ_STOP;
}

uint8_t music_playing(void)
{
    uint8_t r = req_song;
    if (r == REQ_STOP)
        return 0;
    if (r != REQ_NONE)
        return 1;
    return music_on;
}

void sfx_play(uint8_t s)
{
    uint8_t h;
    if (s >= NUM_SFX)
        return;
    h = rq_head;
    if (((h + 1) & 3) == rq_tail)
        return;                             /* queue full: drop */
    rq[h] = s;
    rq_head = (uint8_t)((h + 1) & 3);
}

void music_set_rewind(uint8_t on) { req_rewind = on ? 1 : 0; }
void music_set_muffle(uint8_t on) { req_muffle = on ? 1 : 0; }
void sound_mute_all(uint8_t on) { req_mute = on ? 1 : 0; }

uint8_t sound_debug_loops(void) { return loops; }
uint8_t sound_debug_owned(void) { return owned; }
uint8_t sfx_playing(void)
{
    return (uint8_t)((vo_p[0] != 0) | (vo_p[1] != 0) | (rq_head != rq_tail));
}

#else
/* ======================================================================= *
 * Per-channel template.  SND_CH = 0 pulse1, 1 pulse2, 2 wave, 3 noise.
 * ======================================================================= */
#define SND_CAT2(a, b) a##b
#define SND_CAT(a, b) SND_CAT2(a, b)
#define CV(x) SND_CAT(SND_CAT(c, SND_CH), SND_CAT(_, x))    /* c0_flags */
#define CF(x) SND_CAT(x, SND_CAT(_, SND_CH))                /* frame_0  */

#if SND_CH == 0
#define CBIT 0x01
#define CR1 SND_NR11
#define CR2 SND_NR12
#define CR3 SND_NR13
#define CR4 SND_NR14
#elif SND_CH == 1
#define CBIT 0x02
#define CR1 SND_NR21
#define CR2 SND_NR22
#define CR3 SND_NR23
#define CR4 SND_NR24
#elif SND_CH == 2
#define CBIT 0x04
#define CR1 SND_NR31
#define CR2 SND_NR32
#define CR3 SND_NR33
#define CR4 SND_NR34
#else
#define CBIT 0x08
#endif

static const uint8_t *CV(ptr);      /* pattern stream */
static const uint8_t *CV(ord);      /* next order entry */
static uint8_t CV(wait);            /* rows until next event */
static uint8_t CV(len);             /* current event length (rows) */
static uint8_t CV(note);            /* note index (transposed) / drum id */
static uint8_t CV(ms);              /* macro state: duty phase / wave level / drum step */
static uint8_t CV(flags);
#if SND_CH != 3
static uint16_t CV(freq);           /* base (glided) frequency */
static const snd_inst_t *CV(ip);    /* current instrument */
static uint8_t CV(trans), CV(arp), CV(arp_ph);
static uint8_t CV(vib_ph), CV(vib_dl), CV(vdepth), CV(vrate);
static uint8_t CV(cut), CV(mt);     /* auto-cut countdown, macro timer */
#endif

/* Silence without touching the DAC: the length counter stops the channel
 * (wave: after a stepwise release, see wave_release()). */
static void CF(silence)(void)
{
#if SND_CH == 2
    w_pend = 0;
    if (w_run)
        w_rel = 1;
#elif SND_CH == 3
    SND_W(SND_NR41, 0x3F);
    SND_W(SND_NR44, 0x40);
#else
    const snd_inst_t *in = CV(ip);
    SND_W(CR1, (uint8_t)(((CV(ms) ? in->alt : in->duty) << 6) | 0x3F));
    SND_W(CR4, (uint8_t)(0x40 | ((uint8_t)(CV(freq) >> 8) & 0x07)));
#endif
}

static void CF(kill)(void)
{
    CV(flags) = F_ENDED;
    if (!(hold & CBIT))
        CF(silence)();
}

static void CF(start)(void)
{
    CV(ptr) = pat_empty;
    CV(ord) = song->ord[SND_CH];
    CV(wait) = 1;
    CV(len) = 1;
    CV(flags) = 0;
#if SND_CH != 3
    CV(ip) = &snd_insts[0];
    CV(freq) = 0;
    CV(trans) = 0;
    CV(arp) = 0;
#endif
}

/* (Re)start the hardware for the channel's current note. */
static void CF(trigger)(void)
{
#if SND_CH == 3
    const snd_drum_t *d = &snd_drums[CV(note)];
    SND_W(SND_NR41, 0x00);
    SND_W(SND_NR42, d->env);
    SND_W(SND_NR43, drum_poly(d->seq[0]));
    SND_W(SND_NR44, 0x80);
#else
    const snd_inst_t *in = CV(ip);
    uint16_t f;
#if SND_CH == 2
    if (w_run && wave_loaded != in->duty) {
        /* different wavetable: release + stop first, the note follows */
        w_pend = 1;
        w_rel = 1;
        return;
    }
    w_pend = 0;
    w_rel = 0;
    f = CV(freq);
    if (fx_rewind)
        f = rw_xform(f);
    w_hi = (uint8_t)((f >> 8) & 0x07);
    if (!w_run && wave_loaded != in->duty)
        load_wave(in->duty);            /* channel stopped: safe on every model */
    if (CV(ms) != w_hw) {
        w_hw = CV(ms);
        SND_W(SND_NR32, (uint8_t)(w_hw << 5));
    }
    SND_W(SND_NR33, (uint8_t)f);
    if (w_run) {
        SND_W(SND_NR34, w_hi);          /* running: new pitch, no retrigger */
    } else {
        SND_W(SND_NR34, (uint8_t)(w_hi | 0x80));
        w_run = 1;
    }
#else
    SND_W(CR1, (uint8_t)((CV(ms) ? in->alt : in->duty) << 6));
    SND_W(CR2, in->env);
    f = CV(freq);
    if (fx_rewind)
        f = rw_xform(f);
    SND_W(CR3, (uint8_t)f);
    SND_W(CR4, (uint8_t)(((f >> 8) & 0x07) | 0x80));
#endif
#endif
}

static void CF(note_off)(void)
{
    CV(flags) &= (uint8_t)~(F_GATE | F_GLIDE | F_PFX);
    if (!(hold & CBIT))
        CF(silence)();
}

static void CF(note_on)(uint8_t n)
{
#if SND_CH == 3
    CV(note) = (n < SND_NUM_DRUMS) ? n : 0;
    CV(ms) = 1;
    CV(flags) = F_GATE;
    if (!(hold & CBIT))
        CF(trigger)();
#else
    const snd_inst_t *in;
    uint8_t fl = CV(flags);
    n += CV(trans);
    if (n >= SND_NUM_NOTES)
        n = SND_NUM_NOTES - 1;
    CV(note) = n;
    in = CV(ip);
    CV(arp_ph) = 1;
    CV(vib_ph) = 0;
    CV(vib_dl) = in->vdelay;
    CV(vrate) = in->vrate;
    CV(cut) = in->cut;
    /* vibrato depth in register units, fixed for the note (no per-frame shift) */
    CV(vdepth) = in->vshift ? (uint8_t)((uint16_t)(2048u - snd_freq[n]) >> in->vshift) : 0;
    CV(mt) = in->rate;
#if SND_CH == 2
    CV(ms) = in->env;
#else
    CV(ms) = 0;
#endif
    if ((fl & F_GLNEXT) && CV(freq)) {
        /* glide: re-articulate, then slide from the previous pitch */
        fl = F_GATE | F_GLIDE | F_PFX;
    } else {
        CV(freq) = snd_freq[n];
        fl = (CV(arp) || CV(vdepth)) ? (F_GATE | F_PFX) : F_GATE;
    }
    CV(flags) = fl;
    if (!(hold & CBIT))
        CF(trigger)();
#endif
}

static const uint8_t *CF(next_pattern)(void)
{
    const uint8_t *o = CV(ord);
    uint8_t b = o[0];
    if (b == SND_ORD_JUMP) {
#if SND_CH == 0
        if (loops != 0xFF)
            loops++;
#endif
        o = song->ord[SND_CH] + o[1];
        b = o[0];
    }
    if (b >= SND_ORD_END) {
        CF(kill)();
        return 0;
    }
#if SND_CH != 3
    CV(trans) = o[1];
#endif
    CV(ord) = o + 2;
    return snd_patterns[b];
}

static void CF(row)(void)
{
    const uint8_t *p = CV(ptr);
    uint8_t b;
    for (;;) {
        b = *p++;
        if (b < 0x80) {
            CF(note_on)(b);
            break;
        }
        if (b < 0xC0) {
            CV(len) = (uint8_t)((b & 0x3F) + 1);
            continue;
        }
        if (b == SND_CMD_REST) {
            CF(note_off)();
            break;
        }
        if (b == SND_CMD_HOLD)
            break;
#if SND_CH != 3
        if (b == SND_CMD_INST) {
            CV(ip) = &snd_insts[*p++];
            continue;
        }
        if (b == SND_CMD_ARP) {
            CV(arp) = *p++;
            continue;
        }
        if (b == SND_CMD_GLIDE) {
            CV(flags) |= F_GLNEXT;
            continue;
        }
#endif
        /* SND_CMD_END */
        p = CF(next_pattern)();
        if (!p)
            return;
    }
    CV(ptr) = p;
    CV(wait) = CV(len);
}

#if SND_CH != 3
/* Instrument macro step: duty toggle (pulse) or output-level decay (wave). */
static void CF(macro)(void)
{
    const snd_inst_t *in = CV(ip);
    CV(mt) = in->rate;
#if SND_CH == 2
    if (CV(ms) < 3) {
        CV(ms)++;
        if (!(hold & CBIT) && !w_pend && w_hw != CV(ms)) {
            w_hw = CV(ms);
            SND_W(SND_NR32, (uint8_t)(w_hw << 5));
        }
    }
#else
    CV(ms) ^= 1;
    if (!(hold & CBIT))
        SND_W(CR1, (uint8_t)((CV(ms) ? in->alt : in->duty) << 6));
#endif
}

/* Pitch effects: arpeggio / glide / vibrato / rewind -> frequency regs. */
static void CF(pitch)(void)
{
    uint16_t f;
    uint8_t fl = CV(flags);
    if (CV(arp)) {
        uint8_t n = CV(note);
        uint8_t ph = CV(arp_ph);
        uint8_t a = CV(arp);
        if (ph == 1) {
            n += a >> 4;
            ph = (a & 0x0F) ? 2 : 0;
        } else if (ph == 2) {
            n += a & 0x0F;
            ph = 0;
        } else {
            ph = 1;
        }
        CV(arp_ph) = ph;
        if (n >= SND_NUM_NOTES)
            n = SND_NUM_NOTES - 1;
        f = snd_freq[n];
    } else if (fl & F_GLIDE) {
        uint16_t t = snd_freq[CV(note)];
        uint16_t d;
        f = CV(freq);
        if (t > f) {
            d = (uint16_t)(t - f) >> 2;
            f += d ? d : 1;
        } else if (t < f) {
            d = (uint16_t)(f - t) >> 2;
            f -= d ? d : 1;
        } else {
            fl &= (uint8_t)~F_GLIDE;
        }
        CV(freq) = f;
    } else {
        f = CV(freq);
    }
    if (CV(vdepth)) {
        if (CV(vib_dl)) {
            CV(vib_dl)--;
        } else {
            /* triangle LFO, 8 steps: 0 +1 +2 +1 0 -1 -2 -1 (x vdepth) */
            uint8_t d = CV(vdepth);
            uint8_t p = (uint8_t)((CV(vib_ph) += CV(vrate)) >> 5);
            if (p & 3) {
                if ((p & 3) == 2)
                    d <<= 1;
                if (p & 4)
                    f -= d;
                else
                    f += d;
            }
        }
    }
    if (fx_rewind)
        f = rw_xform(f);
    if (f > 2047)
        f = (f & 0x8000) ? 0 : 2047;
    CV(flags) = fl & (uint8_t)~F_DIRTY;
#if SND_CH == 2
    if (!(hold & CBIT) && w_run && !w_pend) {
        w_hi = (uint8_t)(f >> 8);
        SND_W(CR3, (uint8_t)f);
        SND_W(CR4, w_hi);
    }
#else
    if (!(hold & CBIT)) {
        SND_W(CR3, (uint8_t)f);
        SND_W(CR4, (uint8_t)(f >> 8));
    }
#endif
}
#endif

/* Per-frame effects (kept tiny: the common case is a few flag tests). */
static void CF(frame)(void)
{
#if SND_CH == 3
    const snd_drum_t *d;
    if (!(CV(flags) & F_GATE))
        return;
    d = &snd_drums[CV(note)];
    if (CV(ms) < d->len) {
        if (!(hold & CBIT))
            SND_W(SND_NR43, drum_poly(d->seq[CV(ms)]));
        CV(ms)++;
    } else {
        CV(flags) = 0;          /* macro done: nothing more to do per frame */
    }
#else
    if (!(CV(flags) & F_GATE))
        return;
    if (CV(cut) && --CV(cut) == 0) {
        CF(note_off)();
        return;
    }
    if (CV(mt) && --CV(mt) == 0)
        CF(macro)();
    if (CV(flags) & (F_PFX | F_DIRTY))
        CF(pitch)();
#endif
}

/* An sfx released this channel: re-trigger sustained / slowly decaying
 * notes, silence plucks and drums until their next note. */
static void CF(restore)(void)
{
#if SND_CH != 3
    if (music_on && (CV(flags) & F_GATE)) {
#if SND_CH != 2
        uint8_t env = CV(ip)->env;
        if ((env & 0x08) || (env & 0x07) == 0 || (env & 0x07) >= 5)
#endif
        {
            CF(trigger)();
            CV(flags) |= F_DIRTY;
            return;
        }
    }
#endif
    CF(silence)();
}

#undef CBIT
#undef CR1
#undef CR2
#undef CR3
#undef CR4
#undef CV
#undef CF
#undef SND_CAT
#undef SND_CAT2
#endif
