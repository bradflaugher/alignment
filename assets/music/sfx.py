# ALIGNMENT - sound effects.  Compiled by tools/gen_music.py.
#
# Each effect is a Timeline of per-frame register writes.  Helpers (from
# gen_music.py):
#   P1(note, duty, env, sweep)  trigger pulse 1     P1F(note) change pitch only
#   P2(note, duty, env)         trigger pulse 2     P2F(note)
#   P1D/P2D(duty)               change duty         NOI(poly, env) trigger noise
#   NOIP(poly)                  change noise poly   OFF1/OFF2/OFF4  channel off
# note may be a name ("e6"), a midi number, hz=... or reg=... (11-bit value).
#
# No pops: OFFx stops a channel with its length counter (the DAC stays on),
# and the compiler rejects NRx2 values that switch a DAC off or NRx2 writes
# that are not immediately followed by a trigger.  Envelopes should have run
# down by the end of the timeline (gen_music.py --stats prints any tail the
# driver would chop).
#
# SFX[name] = (timeline, channel mask, priority)
#   mask bit0 = pulse1, bit1 = pulse2, bit3 = noise  (the wave channel is
#   never used by sfx, so the bass line always keeps playing).
#   A new sfx is rejected while an overlapping sfx of higher priority plays;
#   equal priority retriggers.  The channels are released to the music when
#   the timeline ends.
#
# Priorities: 1 per-turn ticks (STEP, BOT_STEP, SCAN) < 2 small feedback <
# 3-4 actions < 5 confirm / unlock < 6 events < 7 EXIT < 8 death / deck < 9
# OPTIMAL.  Per-turn sounds sit on different channels (STEP noise, BOT_STEP
# pulse 2) so they layer instead of cutting each other.

P1_, P2_, NO_ = 1, 2, 8

def T():
    return Timeline()

def _lcg(seed):
    s = [seed]
    def nxt():
        s[0] = (s[0] * 1103515245 + 12345) & 0x7FFFFFFF
        return s[0] >> 16
    return nxt

def expo(f0, f1, n, i):
    """exponential glide f0 -> f1 over n steps, value at step i"""
    return f0 * (f1 / f0) ** (min(i, n) / float(n))

SFX = {}

# STEP - light servo click: two tiny metallic (7-bit) noise ticks, the second
# one lower and softer.  Plays on every step, so it is soft and short and it
# only borrows the drum channel (the music's lead and arps keep going).
t = (T().at(0, NOI(0x2D, 0x51)).at(1, NOIP(0x3D))
        .at(3, NOI(0x4C, 0x31)).at(4, NOIP(0x5C)).length(6))
SFX["STEP"] = (t, NO_, 1)

# BOT_STEP - tiny drone servo: a soft two-step "bi-dip" on pulse 2 (layers
# with STEP on the noise channel).
t = (T().at(0, P2("a5", duty=1, env=0x31)).at(1, P2F("e6"))
        .at(3, P2("b5", duty=1, env=0x21)).at(4, P2F("f#6")).length(7))
SFX["BOT_STEP"] = (t, P2_, 1)

# SCAN - room-generation tick: a very soft high blip that falls a fourth.
t = T().at(0, P2("e6", duty=2, env=0x31)).at(1, P2F("b5")).length(3)
SFX["SCAN"] = (t, P2_, 1)

# BUMP - blocked: a dull low "thunk" (falling square + a low noise puff)
t = (T().at(0, P2("e3", duty=2, env=0x92) + NOI(0x65, 0x81))
        .at(1, P2F("c3")).at(2, P2F("a2") + NOIP(0x76)).at(4, P2F("g2")).length(9))
SFX["BUMP"] = (t, P2_ | NO_, 2)

# FLIP - the signature sound: gravity inversion.  Pulse 1 dips a little, then
# shoots up three octaves while its duty rolls 50 -> 25 -> 12.5 % (a phasing
# "vwoom"), the noise whooshes from dark to bright under it, and a soft high
# glint closes it.  ~0.33 s.
t = T()
t.at(0, P1(hz=330.0, duty=2, env=0xB2) + NOI(0x67, 0x93))
for f in range(1, 4):
    t.at(f, P1F(hz=expo(330.0, 210.0, 3, f)))
for f in range(4, 17):
    t.at(f, P1F(hz=expo(210.0, 1760.0, 12, f - 4)))
t.at(6, P1D(1)).at(10, P1D(0)).at(14, P1D(1))
for f, p in enumerate([0x66, 0x57, 0x56, 0x47, 0x46, 0x45, 0x37, 0x36, 0x35, 0x27, 0x26, 0x25]):
    t.at(f + 1, NOIP(p))
t.at(17, P1("a6", duty=2, env=0x51))
t.length(20)
SFX["FLIP"] = (t, P1_ | NO_, 4)

# LAND - heavy mech thunk: low square with a fast hardware down-sweep and a
# low noise thump that darkens.  Short.
t = (T().at(0, P1("a2", duty=2, env=0xC1, sweep=0x2B) + NOI(0x54, 0xC1))
        .at(1, NOIP(0x65)).at(2, NOIP(0x76)).at(4, NOIP(0x77)).length(10))
SFX["LAND"] = (t, P1_ | NO_, 3)

# PUSH - crate scrape: a rasping 7-bit noise that jitters between two low
# metallic pitches.
t = T().at(0, NOI(0x5D, 0x72))
for f in range(1, 12):
    t.at(f, NOIP([0x5C, 0x4E, 0x5D, 0x4D, 0x5E][f % 5]))
t.length(13)
SFX["PUSH"] = (t, NO_, 2)

# CRATE_LAND - heavy clang: a falling low square (the weight) plus a ringing
# metallic 7-bit noise tone (the steel).
t = T().at(0, P2("d3", duty=1, env=0xC3) + NOI(0x55, 0xD1))
for f, n in enumerate(["c#3", "b2", "a2", "g2", "f2"]):
    t.at(f + 1, P2F(n))
t.at(2, NOI(0x3C, 0x93)).at(6, NOIP(0x3D)).at(10, NOIP(0x3E))
t.length(22)
SFX["CRATE_LAND"] = (t, P2_ | NO_, 3)

# CELL - energy cell pickup: a quick bright E-major arpeggio, then a twinkle
# trill on top that fades.
t = T()
for i, n in enumerate(["b5", "e6", "g#6", "b6"]):
    t.at(i * 2, P1(n, duty=2, env=0xB1))
t.at(8, P1("e7", duty=1, env=0x94))
for f in range(9, 30):
    t.at(f, P1F("e7" if f & 2 else "b6"))
t.at(16, P1D(0))
t.length(32)
SFX["CELL"] = (t, P1_, 4)

# EXIT_OPEN - the doors unlock: a rising power arpeggio (root, fifth, octave:
# no third, so it sits on top of any deck's key), then a fast per-frame
# shimmer of the open chord that rings out.  Pulse 2 only, so it layers with
# CELL (the last cell opens the exit).
t = T()
for i, n in enumerate(["a4", "e5", "a5", "e6"]):
    t.at(i * 3, P2(n, duty=2, env=0xB2))
t.at(12, P2("a6", duty=1, env=0xA5))
for f in range(13, 54):
    t.at(f, P2F(["a6", "e6", "a5"][f % 3]))
t.at(24, P2D(2)).at(36, P2D(1))
t.length(56)
SFX["EXIT_OPEN"] = (t, P2_, 5)

# EXIT - warp through the doorway: pulse 1 glides up four octaves with a fast
# vibrato, pulse 2 runs a rising arpeggio under it, the noise rises from a
# rumble to a hiss and fades.
t = T()
t.at(0, P1(hz=110.0, duty=2, env=0xC4) + P2("a3", duty=1, env=0x93) + NOI(0x77, 0xB4))
for f in range(1, 36):
    hz = expo(110.0, 1760.0, 30, f) * (1.0 + 0.03 * math.sin(f * 2 * math.pi / 4.0))
    t.at(f, P1F(hz=hz))
arp = ["a3", "e4", "a4", "c#5", "e5", "a5", "c#6", "e6", "a6"]
for i, n in enumerate(arp):
    t.at(3 + i * 3, P2(n, duty=1, env=0x92 if i < 6 else 0x73))
for f, p in enumerate([0x76, 0x67, 0x66, 0x57, 0x56, 0x47, 0x46, 0x37, 0x36, 0x27, 0x26, 0x17, 0x16]):
    t.at(1 + f * 2, NOIP(p))
t.at(15, P1D(1)).at(25, P1D(0))
t.length(40)
SFX["EXIT"] = (t, P1_ | P2_ | NO_, 7)

# ZAP - plasma death: an electric crackle (random noise colours every frame)
# over a buzzing square that jitters and falls, then a fizzling tail.
t = T()
rnd = _lcg(1991)
t.at(0, P1(hz=880.0, duty=3, env=0xD3) + NOI(0x21, 0xD4) + P2(hz=55.0 * 2, duty=0, env=0x84))
pitch = 880.0
for f in range(1, 44):
    r = rnd()
    t.at(f, NOIP([0x21, 0x1A, 0x32, 0x0B, 0x23, 0x29, 0x14, 0x31][r & 7]))
    pitch *= 0.955
    t.at(f, P1F(hz=max(90.0, pitch * (1.18 if (r >> 3) & 1 else 1.0))))
    if f % 3 == 0:
        t.at(f, P1D(((r >> 5) & 1) + 2))
    t.at(f, P2F(hz=110.0 if f & 1 else 104.0))
t.at(22, NOI(0x35, 0x73))
t.length(48)
SFX["ZAP"] = (t, P1_ | P2_ | NO_, 8)

# CRUSH - crushed: a low grinding crunch (dark noise shuffled every frame),
# a square dropping into the sub register, and a second groan under it.
t = T()
rnd = _lcg(4242)
t.at(0, NOI(0x54, 0xF3) + P1("e3", duty=2, env=0xE3, sweep=0x2D) + P2("a2", duty=1, env=0xB4))
for f in range(1, 30):
    r = rnd()
    t.at(f, NOIP([0x54, 0x65, 0x5C, 0x66, 0x6D, 0x75, 0x64, 0x76][r & 7]))
    t.at(f, P2F(hz=max(55.0, 110.0 * (0.97 ** f) * (1.04 if r & 8 else 1.0))))
t.at(12, NOI(0x67, 0xB4))
t.length(40)
SFX["CRUSH"] = (t, P1_ | P2_ | NO_, 8)

# BOT_DIE - drone destroyed: a short crunch, then a falling stuttering bleep
# (the drone's voice running down).
t = T().at(0, NOI(0x43, 0xD2) + P1("e6", duty=1, env=0xB2))
t.at(2, NOIP(0x55)).at(4, NOIP(0x66))
pitch = 1318.5
for k in range(1, 8):
    pitch *= 0.80
    t.at(k * 4, P1(hz=pitch, duty=1 + (k & 1), env=max(0x31, 0xB2 - k * 0x10)))
    t.at(k * 4 + 2, P1F(hz=pitch * 0.94))
t.length(34)
SFX["BOT_DIE"] = (t, P1_ | NO_, 6)

# DOOR - laser door toggle: a quick rising "vwip" and a short buzzy hum.
t = T().at(0, P2("a4", duty=0, env=0x93))
for f in range(1, 5):
    t.at(f, P2F(hz=expo(440.0, 1320.0, 4, f)))
t.at(5, P2("e5", duty=1, env=0x62))
for f in range(6, 14):
    t.at(f, P2F(hz=659.3 * (1.012 if f & 1 else 0.988)))
t.length(16)
SFX["DOOR"] = (t, P2_, 3)

# REWIND - reverse whoosh: swelling envelopes (volume goes UP; written right
# before the trigger, so no zombie-mode write), falling pulse, noise getting
# brighter, then a cut like reversed tape.
t = T().at(0, NOI(0x57, 0x19) + P1("b5", duty=2, env=0x19))
for f in range(1, 14):
    t.at(f, P1F(hz=987.8 * (2.0 ** (-f / 10.0))))
    t.at(f, NOIP([0x57, 0x56, 0x46, 0x45, 0x35, 0x34, 0x24, 0x23, 0x22, 0x12, 0x11, 0x10, 0x10][f - 1]))
t.at(14, OFF1() + OFF4()).length(15)
SFX["REWIND"] = (t, P1_ | NO_, 4)

# MENU - tiny cursor tick
t = T().at(0, P1("e6", duty=1, env=0x81)).at(1, P1F("b6")).at(3, OFF1()).length(4)
SFX["MENU"] = (t, P1_, 2)

# SELECT - confirm: a fifth up, the top note rings
t = T().at(0, P1("e6", duty=2, env=0xA1)).at(4, P1("b6", duty=2, env=0xA3)).length(24)
SFX["SELECT"] = (t, P1_, 5)

# OPTIMAL - solved in par: a short brass-like fanfare in D (pickup triplet,
# rise, a big ringing chord with vibrato), drums on the hits, a sparkle on top.
t = T()
hits = [(0, "a5", "f#5"), (5, "a5", "f#5"), (10, "b5", "g5"), (15, "c#6", "a5")]
for f0, a, b in hits:
    t.at(f0, P1(a, duty=2, env=0xC2) + P2(b, duty=1, env=0x92))
t.at(0, NOI(0x31, 0xA1)).at(10, NOI(0x31, 0x81)).at(15, NOI(0x31, 0xA1))
t.at(20, P1("d6", duty=2, env=0xD7) + P2("a5", duty=1, env=0xA7) + NOI(0x03, 0x96))
for g in range(28, 66):
    t.at(g, P1F(hz=1174.7 * (1.0 + 0.010 * math.sin((g - 28) * 2 * math.pi / 7.0))))
t.at(32, P1D(1)).at(40, P2("f#6", duty=0, env=0x63)).at(46, P2("a6", duty=0, env=0x53))
t.at(52, P2("d7", duty=0, env=0x43))
t.length(70)
SFX["OPTIMAL"] = (t, P1_ | P2_ | NO_, 9)

# NEWMECH - something new: a rising Lydian figure with a soft echo below,
# landing on an open sus2 shimmer (D-E-A) that floats.
t = T()
fig = ["d6", "e6", "f#6", "g#6", "a6"]
for i, n in enumerate(fig):
    t.at(i * 5, P1(n, duty=2, env=0x92))
    t.at(i * 5 + 3, P2(midi_of(n) - 12, duty=1, env=0x52))
t.at(26, P1("e7", duty=0, env=0x86) + P2("a6", duty=1, env=0x65))
for g in range(27, 60):
    t.at(g, P1F(["e7", "d7", "a6"][g % 3]))
t.at(40, P2D(2)).at(48, P2D(1)).length(64)
SFX["NEWMECH"] = (t, P1_ | P2_, 6)

# HINT - soft ping on pulse 2 (a fourth, ringing)
t = T().at(0, P2("a6", duty=2, env=0x74)).at(9, P2("d7", duty=0, env=0x43)).length(30)
SFX["HINT"] = (t, P2_, 3)

# ERROR - not allowed: a soft falling fourth ("uh-uh"), not a buzzer
t = (T().at(0, P1("e5", duty=2, env=0x92)).at(7, P1("b4", duty=2, env=0x93)).length(20))
SFX["ERROR"] = (t, P1_, 5)

# DECK - entering a new deck: a short heroic stinger.  Rising fourth + fifth
# in open power intervals (fits any deck key), snare pickup, crash, and a
# held top note with vibrato that rings out.
t = T()
t.at(0, P1("a4", duty=2, env=0xC2) + P2("e4", duty=1, env=0x92) + NOI(0x31, 0x91))
t.at(4, NOI(0x31, 0x71)).at(6, NOI(0x31, 0x81))
t.at(8, P1("d5", duty=2, env=0xC2) + P2("a4", duty=1, env=0x92) + NOI(0x31, 0xA1))
t.at(16, P1("a5", duty=2, env=0xD7) + P2("d5", duty=1, env=0xA7) + NOI(0x02, 0xA6))
for g in range(24, 66):
    t.at(g, P1F(hz=880.0 * (1.0 + 0.010 * math.sin((g - 24) * 2 * math.pi / 7.0))))
t.at(28, P1D(1)).at(34, P2("e5", duty=1, env=0x85)).at(46, P2("a5", duty=0, env=0x53))
t.length(70)
SFX["DECK"] = (t, P1_ | P2_ | NO_, 8)

# ======================================================== action set ======
# Combat sounds.  Only two sfx voices exist and the player's gun fires ~6
# times a second, so the rapid ones are tiny and live on one channel each:
#   pulse 2 : SHOOT / SHOOT_POWER (the arps lose at most a 16th per shot)
#   noise   : HIT / BOSS_HIT / EXPLODE (borrow the drums for a moment)
#   pulse 1 : TURRET / JUMP / ITEM / ALARM (the lead: short, or rare)
# Big events (HURT, BOMB, BOSS_DIE) take pulse 1 + noise (+ pulse 2 for the
# boss death) at high priority so the gun spam cannot cut them.
# Priorities: SHOOT, SHOOT_POWER, HIT, TURRET 2 < JUMP, BOSS_HIT 3 <
# EXPLODE 4 < ITEM 5 < ALARM 6 < HURT 7 < BOMB 8 < BOSS_DIE 9.

# SHOOT - blaster "pew": a thin 25% square diving an octave and a half in
# four frames, quiet enough to fire constantly over the music.
t = T().at(0, P2(hz=1760.0, duty=1, env=0x81))
for f, hz in enumerate([1250.0, 900.0, 660.0], 1):
    t.at(f, P2F(hz=hz))
t.length(4)
SFX["SHOOT"] = (t, P2_, 2)

# SHOOT_POWER - plasma shot: a fat 50% square, lower, that dives with a
# wobble (two pitches per frame step) - "vworp".  Five frames.
t = T().at(0, P2(hz=1040.0, duty=2, env=0x81))
for f, hz in enumerate([1320.0, 700.0, 880.0, 470.0], 1):
    t.at(f, P2F(hz=hz))
t.at(2, P2D(3)).length(5)
SFX["SHOOT_POWER"] = (t, P2_, 2)

# HIT - enemy took damage: a tiny bright metallic tick (7-bit noise).
t = T().at(0, NOI(0x1C, 0x71)).at(1, NOIP(0x2D)).length(3)
SFX["HIT"] = (t, NO_, 2)

# EXPLODE - enemy destroyed: a noise burst that starts crisp and darkens
# (the noise "pitch" drops one clock step every couple of frames).
t = T().at(0, NOI(0x34, 0xF2))
for f, p in enumerate([0x44, 0x45, 0x55, 0x56, 0x66, 0x67, 0x67, 0x77, 0x77, 0x77], 1):
    t.at(f * 2, NOIP(p))
t.length(26)
SFX["EXPLODE"] = (t, NO_, 4)

# HURT - the mech is hit: a harsh buzzing tritone that jitters and falls,
# duty flicking 12.5 <-> 50 %, over a crunchy noise hit.  Alarming, short.
t = T().at(0, P1(hz=620.0, duty=0, env=0xF2) + NOI(0x22, 0xE2))
for f in range(1, 24):
    hz = (620.0 if (f >> 1) & 1 else 438.0) * (0.975 ** f)
    t.at(f, P1F(hz=hz))
    if f % 3 == 0:
        t.at(f, P1D(2 if (f // 3) & 1 else 0))
t.at(3, NOIP(0x35)).at(6, NOI(0x46, 0xA2)).at(9, NOIP(0x57))
t.length(26)
SFX["HURT"] = (t, P1_ | NO_, 7)

# ITEM - power-up: a per-frame major arpeggio whose root glides up two
# octaves (the "charging" climb), then a sparkling octave trill that fades.
t = T().at(0, P1(hz=293.7, duty=2, env=0xD3))
for f in range(1, 21):
    root = expo(293.7, 1174.7, 20, f)
    t.at(f, P1F(hz=root * [1.0, 1.26, 1.498][f % 3]))
t.at(12, P1D(1))
t.at(21, P1("d7", duty=1, env=0x93))
for f in range(22, 40):
    t.at(f, P1F("a6" if f & 2 else "d7"))
t.at(30, P1D(0)).length(42)
SFX["ITEM"] = (t, P1_, 5)

# BOMB - gravity bomb shockwave: a huge 50% square that falls from 220 Hz
# into the sub with a slow beating wobble, over a crash that rumbles down.
t = T().at(0, P1(hz=220.0, duty=2, env=0xF4) + NOI(0x21, 0xF4))
for f in range(1, 52):
    hz = expo(220.0, 38.0, 48, f) * (1.0 + 0.04 * math.sin(f * 2 * math.pi / 6.0))
    t.at(f, P1F(hz=hz))
for f, p in enumerate([0x32, 0x43, 0x44, 0x55, 0x56, 0x66, 0x67, 0x77, 0x77]):
    t.at(2 + f * 3, NOIP(p))
t.at(16, P1D(3)).at(30, P1D(2))
t.length(54)
SFX["BOMB"] = (t, P1_ | NO_, 8)

# BOSS_HIT - heavier metal clank: a loud low 7-bit (tonal) noise strike that
# rings down a couple of steps.  Noise only, so the boss's music keeps its lead.
t = T().at(0, NOI(0x2C, 0xA1)).at(1, NOIP(0x3D)).at(3, NOIP(0x4D)).at(5, NOIP(0x5E)).length(9)
SFX["BOSS_HIT"] = (t, NO_, 3)

# BOSS_DIE - the boss goes down: a chain of blasts (each darker), a voice
# that stutters down two octaves, a low groan, then one big final boom that
# rumbles away.  ~1.5 s.
t = T()
rnd = _lcg(808)
for f0, env in [(0, 0xF2), (12, 0xD2), (22, 0xE2), (31, 0xC2), (39, 0xD2)]:
    t.at(f0, NOI([0x34, 0x45, 0x35, 0x46, 0x44][(f0 // 9) % 5], env))
for f in range(1, 50, 2):
    r = rnd()
    if f not in (12, 22, 31, 39):
        t.at(f, NOIP([0x34, 0x45, 0x55, 0x46, 0x56, 0x35, 0x57, 0x44][r & 7]))
pitch = 1046.5
for k in range(10):
    t.at(k * 5, P1(hz=pitch, duty=k & 1 and 1 or 2, env=0xB2))
    t.at(k * 5 + 2, P1F(hz=pitch * 0.92))
    pitch *= 0.87
t.at(0, P2(hz=110.0, duty=2, env=0xA3))
for f in range(2, 48, 2):
    t.at(f, P2F(hz=max(41.0, 110.0 * 0.98 ** f) * (1.03 if f & 2 else 1.0)))
t.at(50, NOI(0x56, 0xF3) + P1(hz=82.4, duty=2, env=0xD3, sweep=0x7F) + P2(hz=55.0, duty=2, env=0xA3))
for f, p in enumerate([0x57, 0x66, 0x67, 0x76, 0x77]):
    t.at(54 + f * 6, NOIP(p))
t.length(92)
SFX["BOSS_DIE"] = (t, P1_ | P2_ | NO_, 9)

# ALARM - boss incoming / blast doors sealing: a two-tone siren (a5 / d5,
# each tone sliding up a little), four cycles, the last tone fading.  Pulse 1
# only, so drums, bass and arps keep driving under it.  ~1.1 s.
t = T()
for k in range(8):
    f0 = k * 8
    base = 880.0 if k % 2 == 0 else 587.3
    t.at(f0, P1(hz=base, duty=1, env=0xC2 if k < 7 else 0x91))
    for g in (2, 4, 6):
        t.at(f0 + g, P1F(hz=base * (1.0 + 0.006 * g)))
t.length(66)
SFX["ALARM"] = (t, P1_, 6)

# TURRET - enemy fires: a short low zap (12.5% square, hardware sweep
# diving into the bass).  Eight frames.
t = T().at(0, P1(hz=392.0, duty=0, env=0xA1, sweep=0x1A)).length(8)
SFX["TURRET"] = (t, P1_, 2)

# JUMP - a short rising square blip (not the octave-climbing FLIP whoosh):
# up a sixth in five frames with the duty opening up.
t = T().at(0, P1(hz=330.0, duty=1, env=0xA1))
for f in range(1, 7):
    t.at(f, P1F(hz=expo(330.0, 560.0, 5, f)))
t.at(3, P1D(2)).length(10)
SFX["JUMP"] = (t, P1_, 3)
