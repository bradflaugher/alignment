# ALIGNMENT - song sources.  Compiled by tools/gen_music.py.
#
# Notation: see the docstring at the top of tools/gen_music.py.
# One row = one 16th note, 16 rows per bar.  speed = frames per row
# (a pair alternates per row: (7, 6) = 6.5 frames = ~138 BPM).
# Channels: p1 = pulse 1 (lead), p2 = pulse 2 (arps / harmony),
#           wv = wave (bass), no = noise (drums).
# Orders: list of pattern names or (name, transpose); "LOOP" marks the loop
# point (no LOOP = one-shot song that stops at the end).
# key / chords: only for `gen_music.py --check` (one chord per bar, "X,Y" =
# two chords in a bar, "X*4" = four bars).
#
# All music is original.  Style homage: Tecmo's Metal Storm (NES) - driving
# minor keys, 16th-note octave bass, fast arpeggios, heroic vibrato leads.
# The gravity flip is written into the themes: motifs come back mirrored
# (inverted around their first note) and sequences rise / fall.
#
# One bass wavetable ("warm") is shared by every song except ZEN, so the
# driver almost never rewrites wave RAM.

# ---------------------------------------------------------------- waves ----
def _wave(fn):
    out = []
    for i in range(32):
        v = fn(i / 32.0)            # -1..1
        out.append(max(0, min(15, int(round(7.5 + 7.5 * v)))))
    return out

WAVES = [
    # warm: a saw rounded off with its own sine fundamental - fat, not buzzy
    ("warm", _wave(lambda t: 0.55 * (1.0 - 2.0 * t) + 0.45 * math.sin(2 * math.pi * t))),
    # sine-ish with a touch of 2nd harmonic: soft sub for ZEN
    ("soft", _wave(lambda t: 0.85 * math.sin(2 * math.pi * t) + 0.15 * math.sin(4 * math.pi * t))),
]

# ---------------------------------------------------------- instruments ----
# pulse: env (NRx2), duty 0..3 (12.5/25/50/75%), alt+rate = duty toggle macro
# wave : wave, level (1=100%, 2=50%, 3=25%), decay = frames per level step
# vshift/vrate/vdelay = vibrato, cut = auto note-off after N frames
INSTRUMENTS = [
    # leads
    ("lead",     dict(env=0xB7, duty=2, vshift=8, vrate=20, vdelay=16)),          # round heroic lead
    ("leadb",    dict(env=0xB6, duty=1, vshift=8, vrate=22, vdelay=12)),          # brighter lead (25%)
    ("leadsoft", dict(env=0x86, duty=2, vshift=8, vrate=18, vdelay=18)),
    ("bell",     dict(env=0x94, duty=2, alt=1, rate=8, vshift=9, vrate=14, vdelay=20)),
    ("bellsoft", dict(env=0x85, duty=2, vshift=9, vrate=12, vdelay=24)),
    ("echo",     dict(env=0x55, duty=2, vshift=8, vrate=20, vdelay=16)),
    # arps / harmony
    ("pluck",    dict(env=0x81, duty=1)),                                          # 16th arps
    ("pluckw",   dict(env=0x71, duty=2)),                                          # warmer, softer arps
    ("glass",    dict(env=0x62, duty=1)),
    ("mbox",     dict(env=0x63, duty=1)),                                          # music box (ZEN)
    ("chord",    dict(env=0x77, duty=2)),                                          # per-frame arp chords
    ("harm",     dict(env=0x86, duty=2, vshift=9, vrate=20, vdelay=16)),           # harmony line
    ("stab",     dict(env=0x92, duty=1)),
    ("pad",      dict(env=0x50, duty=2, vshift=10, vrate=10, vdelay=20)),         # held, soft
    # bass (wave)
    ("bass",     dict(kind="wave", wave="warm", level=1, decay=3)),                # punchy, articulated
    ("bassl",    dict(kind="wave", wave="warm", level=1, decay=12)),               # longer notes
    ("basss",    dict(kind="wave", wave="warm", level=1)),                         # sustained
    ("bassq",    dict(kind="wave", wave="warm", level=2)),                         # quiet sustained
    ("sub",      dict(kind="wave", wave="soft", level=2, decay=48)),
    ("subq",     dict(kind="wave", wave="soft", level=2)),
]

# ---------------------------------------------------------------- drums ----
# (grid char, NR42 envelope, [NR43 per frame, 1..4 values])
DRUMS = [
    ("K", 0xB1, [0x41, 0x55, 0x66, 0x77]),   # kick: click then falling thump
    ("k", 0x81, [0x55, 0x66, 0x77]),         # soft kick
    ("S", 0xA2, [0x32, 0x23, 0x33, 0x34]),   # snare
    ("s", 0x51, [0x33]),                     # ghost snare
    ("h", 0x31, [0x12]),                     # closed hat (soft, not too bright)
    ("H", 0x51, [0x12]),                     # accent hat
    ("o", 0x33, [0x13]),                     # open hat
    ("C", 0x74, [0x03, 0x13, 0x14, 0x15]),   # crash
    ("t", 0x41, [0x3C]),                     # metal tick (7-bit = tonal)
    ("T", 0x61, [0x4D, 0x5D]),               # clank (low 7-bit)
    ("z", 0x21, [0x14]),                     # shaker (very soft)
    ("m", 0xA2, [0x45, 0x56, 0x57, 0x67]),   # low tom
]

# -------------------------------------------------------------- helpers ----
NAMES = ["c", "c#", "d", "d#", "e", "f", "f#", "g", "g#", "a", "a#", "b"]

def nm(m):
    return "%s%d" % (NAMES[m % 12], m // 12 - 1)

def tr(note, k):
    return nm(midi_of(note) + k)

def bars(*parts):
    return " | ".join(parts) + " |"

def rest_bars(n):
    return " ".join(["r:16 |"] * n)

def roll(chords, pat, L=1):
    """Arpeggio: for each chord (space separated notes) emit pat (indices
    into the chord, None = hold) with notes of L rows."""
    out = []
    for ch in chords:
        notes = ch.split()
        for i in pat:
            out.append(("%s:%d" % (notes[i], L)) if i is not None else "-:%d" % L)
    return " ".join(out)

BASS = {
    # R = root, O = octave up, F = fifth
    "gal":   "R:2 R:1 R:1 O:2 R:2 R:1 R:1 O:2 R:2 O:2",
    "gal2":  "R:2 R:1 R:1 O:2 R:2 R:1 R:1 O:2 F:2 O:2",
    "drv":   "R:1 R:1 O:1 R:1 R:1 R:1 O:1 R:1 R:1 R:1 O:1 R:1 R:1 O:1 R:1 O:1",
    "oct8":  "R:2 O:2 R:2 O:2 R:2 O:2 R:2 O:2",
    "pump":  "R:2 R:2 R:2 R:2 R:2 R:2 R:2 R:2",
    "s332":  "R:3 O:3 R:2 R:3 O:3 F:2",
    "s332b": "R:3 O:3 R:2 F:3 O:3 R:2",
    "half":  "R:8 F:8",
    "whole": "R:16",
    "dot":   "R:6 R:6 O:4",
    "push":  "R:6 R:6 O:2 F:2",
    "walk":  "R:4 F:4 O:4 F:4",
}

def bass(style, *roots):
    """One bar per root in the given style; root may be 'e2' or 'e2/b1' to
    give the fifth explicitly (default: a perfect fifth up)."""
    out = []
    for r in roots:
        if r == "r":
            out.append("r:16")
            continue
        root = r.split("/")[0]
        five = r.split("/")[1] if "/" in r else tr(root, 7)
        pat = BASS[style].replace("R:", root + ":").replace("O:", tr(root, 12) + ":").replace("F:", five + ":")
        out.append(pat)
    return " | ".join(out) + " |"

def drums(*bars_):
    return " ".join(bars_)

UPDN = [0, 1, 2, 3, 2, 1, 0, 1, 2, 3, 2, 1, 0, 1, 2, 3]
UP4 = [0, 1, 2, 3] * 4
TRI = [0, 1, 2, 1] * 4

# ================================================================= TITLE ===
# "ALIGNMENT" - D minor, ~138 BPM.  An 8-bar build (arps, then the octave
# bass, then the lead teasing the hook low) into the anthem: the hook rises
# D-F-A and answers itself inverted (falls D-Bb-F) - gravity flipping.
# B section lifts to the relative major, the breakdown slows the hook into
# long gliding notes over half-time drums and climbs back in.
T_DM, T_BB, T_F, T_C = "d4 f4 a4 d5", "d4 f4 bb4 d5", "c4 f4 a4 c5", "c4 e4 g4 c5"
T_GM, T_A, T_AM = "d4 g4 bb4 d5", "c#4 e4 a4 c#5", "c4 e4 a4 c5"

title = dict(
    speed=(7, 6),
    key="d e f g a bb c",
    chords="Dm Dm Bb C Dm Dm Bb A "                         # intro
           "Dm Bb F C Dm Bb Gm A "                          # hook
           "Dm Bb F C Dm Bb Gm,A Dm "                       # hook answer
           "Bb C Am Dm Gm C F A "                           # B: lift
           "Dm Dm Bb Bb Gm Gm A A",                         # breakdown
    patterns=dict(
        # ---- lead ----
        intro_l="r:16 | r:16 | r:16 | r:16 | "
                "@leadsoft d4:6 f4:6 a4:4 | g4:8 f4:4 e4:4 | d5:6 bb4:6 f4:4 | @lead e4:4 a4:4 c#5:4 e5:4 |",
        hook=bars("@lead d5:3 f5:3 a5:2 g5:2 f5:2 e5:2 f5:2",
                  "d6:3 bb5:3 f5:2 g5:2 bb5:2 c6:2 d6:2",
                  "c6:3 a5:3 f5:2 a5:2 c6:2 f6:4",
                  "e6:6 d6:2 c6:4 g5:4",
                  "d5:3 f5:3 a5:2 g5:2 f5:2 e5:2 f5:2",
                  "d6:3 bb5:3 f5:2 g5:2 f5:2 bb5:2 d6:2",
                  "g6:3 f6:3 d6:2 bb5:4 g5:4",
                  "a5:3 c#6:3 e6:2 a6:8"),
        hook2=bars("@lead f5:3 a5:3 d6:2 c6:2 a5:2 g5:2 a5:2",
                   "f6:3 d6:3 bb5:2 c6:2 d6:2 f6:2 d6:2",
                   "c6:3 a5:3 f5:2 a5:2 c6:2 a6:4",
                   "g6:6 f6:2 e6:4 c6:4",
                   "d6:3 a5:3 f5:2 e5:2 f5:2 a5:2 d6:2",
                   "f6:3 d6:3 bb5:2 a5:2 bb5:2 d6:2 f6:2",
                   "g6:4 f6:2 e6:2 c#6:4 e6:4",
                   "d6:12 r:4"),
        lift=bars("@leadb f5:4 bb5:4 d6:8",
                  "e6:4 d6:2 c6:2 g5:8",
                  "a5:4 c6:4 e6:6 d6:2",
                  "f6:8 e6:4 d6:4",
                  "d6:4 g6:4 bb6:8",
                  "a6:4 g6:2 e6:2 c6:8",
                  "c6:4 f6:4 a6:4 g6:2 f6:2",
                  "e6:12 c#6:2 e6:2"),
        brk=bars("@lead a5:6 f5:6 d5:4", "~e5:8 f5:8",
                 "d6:6 bb5:6 f5:4", "~g5:8 f5:8",
                 "bb5:6 g5:6 d5:4", "~e5:8 g5:8",
                 "a5:16", "c#6:4 e6:4 a6:4 ~g6:4"),
        # ---- arps ----
        intro_a="@pluckw " + roll([T_DM, T_DM, T_BB, T_C], UPDN) + " | " +
                "@pluck " + roll([T_DM, T_DM, T_BB, T_A], UPDN) + " |",
        hook_a="@pluck " + roll([T_DM, T_BB, T_F, T_C, T_DM, T_BB, T_GM, T_A], UPDN) + " |",
        hook2_a="@pluck " + roll([T_DM, T_BB, T_F, T_C, T_DM, T_BB], UPDN) + " " +
                roll([T_GM], UPDN[:8]) + " " + roll([T_A], UPDN[:8]) + " " + roll([T_DM], UPDN) + " |",
        lift_a="@pluckw " + roll([T_BB, T_C, T_AM, T_DM, T_GM, T_C, T_F, T_A],
                                 [3, 2, 1, 0, 3, 2, 1, 0, 3, 2, 1, 0, 1, 2, 3, 2]) + " |",
        brk_a="@chord A37 d5:16 | d5:16 | A47 bb4:16 | bb4:16 | A37 g4:16 | g4:16 | A47 a4:16 | a4:8 A00 @pluck "
              "a4:1 c#5:1 e5:1 a5:1 c#6:1 e6:1 a6:1 e6:1 |",
        # ---- bass ----
        intro_b="r:16 | r:16 | @bassl " + bass("oct8", "a#1", "c2", "d2", "d2", "a#1", "a1"),
        hook_b="@bass " + bass("gal", "d2", "a#1", "f2", "c2", "d2", "a#1", "g1", "a1"),
        hook2_b="@bass " + bass("gal", "d2", "a#1", "f2", "c2", "d2", "a#1") + " g1:2 g1:1 g1:1 g2:2 g1:2 "
                "a1:2 a1:1 a1:1 a2:2 a1:2 | " + bass("gal2", "d2"),
        lift_b="@bass " + bass("oct8", "a#1", "c2", "a1", "d2", "g1", "c2", "f2", "a1"),
        brk_b="@bassl " + bass("dot", "d2", "d2", "a#1", "a#1", "g1", "g1") + " @bass " + bass("drv", "a1", "a1"),
        # ---- drums ----
        intro_d=drums("K...............", "K.......K.......", "K...h...K...h...", "K...h...K...h.hh",
                      "K.h.h.h.K.h.h.h.", "K.h.S.h.K.h.S.h.", "K.h.S.h.K.h.S.hh", "K.h.S.h.KSKSSSSS"),
        hook_d=drums("C.h.S.h.K.hKS.h.", "K.h.S.h.K.hKS.hh", "K.h.S.h.K.hKS.h.", "K.h.S.h.K.hKS.hs",
                     "K.h.S.h.K.hKS.h.", "K.h.S.h.K.hKS.hh", "K.h.S.h.K.hKS.h.", "K.h.S.hSK.SKS.SS"),
        lift_d=drums("C.o.S.h.K.o.S.h.", "K.o.S.h.K.o.S.hh", "K.o.S.h.K.o.S.h.", "K.o.S.h.K.oKS.hh",
                     "K.o.S.h.K.o.S.h.", "K.o.S.h.K.o.S.hh", "K.o.S.h.K.o.S.h.", "K.S.S.S.SSSSmmmm"),
        brk_d=drums("C.......S.......", "k.......S.......", "k.......S.......", "k.......S...k...",
                    "k.......S.......", "k.......S.......", "K...K...S...K...", "K.K.S.K.SSSSSSSS"),
    ),
    order=dict(
        p1=["intro_l", "LOOP", "hook", "hook2", "lift", "brk"],
        p2=["intro_a", "LOOP", "hook_a", "hook2_a", "lift_a", "brk_a"],
        wv=["intro_b", "LOOP", "hook_b", "hook2_b", "lift_b", "brk_b"],
        no=["intro_d", "LOOP", "hook_d", "hook_d", "lift_d", "brk_d"],
    ),
)

# ================================================================ DECK_A ===
# "Hull Breach" - A minor, 150 BPM.  The driving one: galloping 16th octave
# bass, rolling 16th arps, a riff-like theme built on a 3+1 "turn" figure.
# The theme's second phrase is the first one mirrored (falls where it rose).
# Form: intro 4 | A 16 | B 16 | breakdown 8 | A' 16 (harmony in thirds) |
# tag 8  -> ~102 s loop.
A_AM, A_F, A_G, A_EM, A_E = "a3 c4 e4 a4", "a3 c4 f4 a4", "b3 d4 g4 b4", "b3 e4 g4 b4", "b3 e4 g#4 b4"
A_DM, A_C = "a3 d4 f4 a4", "c4 e4 g4 c5"

deck_a = dict(
    speed=6,
    key="a b c d e f g",
    chords="Am Am Am Am,E "
           "Am F G Em Am F G E Am F G Em F G E Am "
           "Dm Am Dm E F C Dm E Dm Am Dm E F C Dm E "
           "F G Em Am F G E E "
           "Am F G Em Am F G E Am F G Em F G E Am "
           "Am G F E Am G F,G E",
    patterns=dict(
        intro_l="r:16 | r:16 | r:16 | r:8 @leadb e4:2 a4:2 b4:2 c5:2 |",
        a1=bars("@leadb a4:2 c5:2 e5:3 d5:1 c5:2 b4:2 c5:2 e5:2",
                "f5:4 e5:2 c5:2 a4:4 c5:4",
                "b4:2 d5:2 g5:3 f5:1 e5:2 d5:2 b4:2 d5:2",
                "e5:8 r:2 e5:2 g5:2 b5:2",
                "a5:2 g5:2 e5:3 d5:1 c5:2 b4:2 c5:2 e5:2",
                "f5:4 a5:2 c6:2 a5:4 f5:4",
                "g5:2 f5:2 d5:3 b4:1 g4:2 b4:2 d5:2 f5:2",
                "e5:6 g#5:2 b5:4 e5:4"),
        a2=bars("@leadb a4:2 c5:2 e5:3 d5:1 c5:2 b4:2 c5:2 e5:2",
                "f5:4 e5:2 c5:2 a4:4 c5:4",
                "b4:2 d5:2 g5:3 f5:1 e5:2 d5:2 b4:2 d5:2",
                "e5:8 r:2 b4:2 c5:2 d5:2",
                "c5:2 d5:2 f5:3 e5:1 d5:2 c5:2 a4:2 c5:2",
                "d5:2 e5:2 g5:3 f5:1 e5:2 d5:2 b4:2 d5:2",
                "e5:4 g#5:4 b5:4 d6:4",
                "c6:6 b5:2 a5:8"),
        b1=bars("@lead f5:4 e5:4 d5:4 a5:4",
                "e5:4 d5:4 c5:4 a5:4",
                "d5:4 f5:4 a5:4 d6:4",
                "b5:6 g#5:2 e5:8",
                "a5:4 g5:4 f5:4 c6:4",
                "g5:4 f5:4 e5:4 c6:4",
                "a5:4 d6:4 f6:4 e6:2 d6:2",
                "e6:12 r:4"),
        b2=bars("@lead f5:2 e5:1 d5:1 e5:2 f5:2 a5:4 f5:4",
                "e5:2 d5:1 c5:1 d5:2 e5:2 a5:4 e5:4",
                "d5:2 e5:2 f5:2 a5:2 d6:4 c6:2 a5:2",
                "b5:4 g#5:4 b5:4 e6:4",
                "c6:2 a5:2 f5:2 a5:2 c6:4 f6:4",
                "e6:2 c6:2 g5:2 c6:2 e6:4 g6:4",
                "f6:4 d6:4 a5:4 f6:2 e6:2",
                "e6:8 d6:2 c6:2 b5:2 g#5:2"),
        brk=bars("@bell a5:8 f5:4 c5:4", "b5:8 g5:4 d5:4", "g5:8 e5:4 b4:4", "a5:8 e5:4 c5:4",
                 "c6:8 a5:4 f5:4", "d6:8 b5:4 g5:4",
                 "@leadb e5:1 f5:1 g#5:1 a5:1 b5:1 c6:1 d6:1 e6:1 e5:1 f5:1 g#5:1 a5:1 b5:1 c6:1 d6:1 e6:1",
                 "g#5:1 b5:1 e6:1 g#6:1 e6:1 b5:1 g#5:1 e5:1 @lead e6:8"),
        tag=bars("@lead c6:8 b5:4 a5:4", "b5:8 a5:4 g5:4", "a5:8 g5:4 f5:4", "g#5:8 e5:4 b4:4",
                 "c6:6 e6:6 c6:4", "b5:6 d6:6 b5:4", "a5:4 c6:4 b5:4 d6:4", "g#5:4 b5:4 e6:6 r:2"),
        # arps / harmony
        intro_a="@pluckw " + roll([A_AM, A_AM, A_AM, A_AM], UPDN) + " |",
        a_arp="@pluck " + roll([A_AM, A_F, A_G, A_EM, A_AM, A_F, A_G, A_E], UPDN) + " |",
        a_arp2="@pluck " + roll([A_AM, A_F, A_G, A_EM, A_DM, A_G, A_E, A_AM], UPDN) + " |",
        b_arp="@pluckw " + roll([A_DM, A_AM, A_DM, A_E, A_F, A_C, A_DM, A_E], TRI) + " |",
        brk_a="@chord A47 f4:16 | A47 g4:16 | A37 e4:16 | A37 a4:16 | A47 f4:16 | A47 g4:16 | A47 e4:16 | e4:8 A00 r:8 |",
        a_harm=bars("@harm e4:2 a4:2 c5:3 b4:1 a4:2 g4:2 a4:2 c5:2",
                    "c5:4 c5:2 a4:2 f4:4 a4:4",
                    "g4:2 b4:2 d5:3 d5:1 c5:2 b4:2 g4:2 b4:2",
                    "b4:8 r:2 b4:2 e5:2 g5:2",
                    "e5:2 e5:2 c5:3 b4:1 a4:2 g4:2 a4:2 c5:2",
                    "c5:4 f5:2 a5:2 f5:4 c5:4",
                    "d5:2 d5:2 b4:3 g4:1 d4:2 g4:2 b4:2 d5:2",
                    "b4:6 e5:2 g#5:4 b4:4"),
        tag_a="@pluckw " + roll([A_AM, A_G, A_F, A_E, A_AM, A_G], [0, 1, 2, 3] * 4) + " " +
              roll([A_F], [0, 1, 2, 3] * 2) + " " + roll([A_G], [0, 1, 2, 3] * 2) + " " +
              roll([A_E], [0, 1, 2, 3] * 4) + " |",
        # bass
        intro_b="@bass " + bass("drv", "a1", "a1", "a1") + " a1:1 a1:1 a2:1 a1:1 a1:1 a1:1 a2:1 a1:1 e2:2 g2:2 g#2:2 g#1:2 |",
        a_b="@bass " + bass("gal", "a1", "f1", "g1", "e1", "a1", "f1", "g1", "e1"),
        a_b2="@bass " + bass("gal", "a1", "f1", "g1", "e1", "f1", "g1", "e1", "a1"),
        b_b="@bass " + bass("oct8", "d2", "a1", "d2", "e2", "f2", "c2", "d2", "e2"),
        brk_b="@bassl " + bass("whole", "f1", "g1", "e1", "a1", "f1", "g1") + " @bass " + bass("drv", "e1", "e1"),
        tag_b="@bass " + bass("gal2", "a1", "g1", "f1", "e1", "a1", "g1") +
              " f1:2 f1:1 f1:1 f2:2 f1:2 g1:2 g1:1 g1:1 g2:2 g1:2 | " + bass("gal", "e1"),
        # drums
        intro_d=drums("K...h...K...h...", "K...h...K...h.h.", "K.h.h.h.K.h.h.h.", "K.h.S.h.K.SKS.SS"),
        a_d=drums("C.h.S.hKK.h.S.h.", "K.h.S.hKK.h.S.hh", "K.h.S.hKK.h.S.h.", "K.h.S.hKK.hKS.hs",
                  "K.h.S.hKK.h.S.h.", "K.h.S.hKK.h.S.hh", "K.h.S.hKK.h.S.h.", "K.h.S.hKK.SKS.SS"),
        b_d=drums("C.o.S.o.K.o.S.oK", "K.o.S.o.K.o.S.o.", "K.o.S.o.K.o.S.oK", "K.o.S.o.K.oKS.SS",
                  "K.o.S.o.K.o.S.oK", "K.o.S.o.K.o.S.o.", "K.o.S.o.K.o.S.oK", "K.S.S.S.SSSSmmmm"),
        brk_d=drums("C...z...S...z...", "k...z...S...z...", "k...z...S...z...", "k...z...S...z.k.",
                    "k...z...S...z...", "k...z...S...z.k.", "K.h.S.h.K.h.S.h.", "K.S.K.S.SSSSSSSS"),
        tag_d=drums("C.h.S.h.K.h.S.h.", "K.h.S.h.K.h.S.hh", "K.h.S.h.K.h.S.h.", "K.h.S.h.K.hKS.h.",
                    "K.h.S.h.K.h.S.h.", "K.h.S.h.K.h.S.hh", "K.h.S.h.K.hKS.h.", "K.S.S.S.SSSSmmmm"),
    ),
    order=dict(
        p1=["intro_l", "LOOP", "a1", "a2", "b1", "b2", "brk", "a1", "a2", "tag"],
        p2=["intro_a", "LOOP", "a_arp", "a_arp2", "b_arp", "b_arp", "brk_a", "a_harm", "a_arp2", "tag_a"],
        wv=["intro_b", "LOOP", "a_b", "a_b2", "b_b", "b_b", "brk_b", "a_b", "a_b2", "tag_b"],
        no=["intro_d", "LOOP", "a_d", "a_d", "b_d", "b_d", "brk_d", "a_d", "a_d", "tag_d"],
    ),
)

# ================================================================ DECK_B ===
# "Reactor Hum" - E Phrygian, ~120 BPM, mysterious and industrial.  A 3+3+2
# octave bass ostinato that leans on the Phrygian F, metal ticks and clanks
# instead of hats, a glassy 6-step arp that phases against the bar, and a
# bell lead that answers each rising question with its falling mirror.
B_EM, B_F, B_G, B_AM, B_DM, B_C = "e4 g4 b4 e5", "f4 a4 c5 f5", "g4 b4 d5 g5", "e4 a4 c5 e5", "d4 f4 a4 d5", "e4 g4 c5 e5"
B_CYC = [0, 1, 2, 3, 1, 2]

def phase(chords, cycle, steps=16, start=0, L=1):
    out = []
    k = start
    for ch in chords:
        notes = ch.split()
        for _ in range(steps // L):
            out.append("%s:%d" % (notes[cycle[k % len(cycle)]], L))
            k += 1
    return " ".join(out)

deck_b = dict(
    speed=(8, 7),
    key="e f g a b c d",
    chords="Em Em Em Em "
           "Em F Em F Em F G Em Em F Em F Am G F Em "
           "Am G F Em Am G F Em Dm C F Em Dm C F F "
           "Em Em F F Em Em F F "
           "Em F Em F Em F G Em",
    patterns=dict(
        intro_l=rest_bars(4),
        a1=bars("r:4 @bell e5:3 f5:1 e5:4 b4:4", "a4:8 r:4 c5:2 d5:2",
                "r:4 e5:3 f5:1 e5:4 g5:4", "a5:8 r:4 f5:2 e5:2",
                "r:4 b5:3 c6:3 b5:2 g5:4", "a5:8 r:4 c6:2 b5:2",
                "d6:4 b5:4 g5:4 d5:4", "e5:16"),
        a2=bars("r:4 @bell e5:3 f5:1 e5:4 b4:4", "a4:8 r:4 c5:2 d5:2",
                "r:4 e5:3 f5:1 e5:4 g5:4", "a5:8 r:4 f5:2 e5:2",
                "@leadsoft c5:3 e5:3 a5:2 c6:4 b5:2 a5:2", "b5:6 g5:2 d5:8",
                "f5:3 a5:3 c6:2 a5:4 f5:4", "e5:12 r:4"),
        b1=bars("@lead a5:3 c6:3 e6:2 d6:4 c6:2 b5:2", "b5:6 g5:2 d5:8",
                "c6:3 a5:3 f5:2 a5:4 c6:4", "b5:12 r:4",
                "a5:3 c6:3 e6:2 d6:4 c6:2 b5:2", "d6:6 b5:2 g5:8",
                "f5:3 a5:3 c6:2 f6:4 e6:2 c6:2", "e6:12 r:4"),
        b2=bars("@lead d6:3 f6:3 a6:2 g6:4 f6:2 e6:2", "e6:6 c6:2 g5:8",
                "a5:3 c6:3 f6:2 e6:4 c6:2 a5:2", "b5:12 r:4",
                "d6:3 a5:3 f5:2 a5:4 d6:4", "c6:6 g5:2 e5:8",
                "f5:3 a5:3 c6:2 f6:6 e6:2", "f6:4 e6:4 c6:4 a5:4"),
        brk=bars("r:16", "r:8 @echo e6:2 f6:2 e6:4", "r:16", "r:8 c6:2 b5:2 a5:4",
                 "r:16", "r:8 b5:2 c6:2 b5:4", "r:16", "a5:4 c6:4 f6:4 e6:4"),
        # arps
        intro_a="@glass " + phase([B_EM] * 4, B_CYC),
        a_arp="@glass " + phase([B_EM, B_F, B_EM, B_F, B_EM, B_F, B_G, B_EM], B_CYC),
        a_arp2="@glass " + phase([B_EM, B_F, B_EM, B_F, B_AM, B_G, B_F, B_EM], B_CYC, start=3),
        b_arp="@pluckw " + phase([B_AM, B_G, B_F, B_EM, B_AM, B_G, B_F, B_EM], [0, 2, 1, 3, 2, 1, 0, 2], steps=16, L=2),
        b_arp2="@pluckw " + phase([B_DM, B_C, B_F, B_EM, B_DM, B_C, B_F, B_F], [0, 2, 1, 3, 2, 1, 0, 2], steps=16, L=2),
        brk_a="@chord A37 e4:16 | e4:16 | A47 f4:16 | f4:16 | A37 e4:16 | e4:16 | A47 f4:16 | f4:8 A00 @glass f4:8 |",
        # bass
        intro_b="@bass " + bass("s332", "e1/g1", "e1/g1", "e1/g1", "e1/g1"),
        a_b="@bass " + bass("s332", "e1/g1", "f1/c2", "e1/g1", "f1/c2", "e1/g1", "f1/c2", "g1/d2", "e1/b1"),
        a_b2="@bass " + bass("s332", "e1/g1", "f1/c2", "e1/g1", "f1/c2", "a1/e2", "g1/d2", "f1/c2", "e1/b1"),
        b_b="@bass " + bass("s332b", "a1", "g1", "f1", "e1", "a1", "g1", "f1", "e1"),
        b_b2="@bass " + bass("s332b", "d2", "c2", "f1", "e1", "d2", "c2", "f1", "f1"),
        brk_b="@bassl " + bass("dot", "e1", "e1", "f1", "f1", "e1", "e1", "f1") + " @bass " + bass("drv", "f1"),
        # drums: kick + clank + metal ticks, syncopated
        intro_d=drums("k...t...t...t...", "k...t...t...t.t.", "k...t..tk...t...", "k...t..tk.T.T.TT"),
        a_d=drums("K..t..tTS..t..t.", "K..t..tTS..t.Kt.", "K..t..tTS..t..t.", "K..t..tTS..tKKtT",
                  "K..t..tTS..t..t.", "K..t..tTS..t.Kt.", "K..t..tTS..t..t.", "K..t..tTS.TtSSTT"),
        b_d=drums("C..t.TtTS..t.TtK", "K..t.TtTS..t.Tt.", "K..t.TtTS..t.TtK", "K..t.TtTS.KtSSTT",
                  "K..t.TtTS..t.TtK", "K..t.TtTS..t.Tt.", "K..t.TtTS..t.TtK", "K..TK.TKS.SSSSmm"),
        brk_d=drums("C.......t.......", "k.......t.......", "k.......t...t...", "k.......t.t.t.t.",
                    "k.......t.......", "k.......t...t...", "k...t...k...t...", "k.t.k.t.S.S.SSSS"),
    ),
    order=dict(
        p1=["intro_l", "LOOP", "a1", "a2", "b1", "b2", "brk", "a1"],
        p2=["intro_a", "LOOP", "a_arp", "a_arp2", "b_arp", "b_arp2", "brk_a", "a_arp"],
        wv=["intro_b", "LOOP", "a_b", "a_b2", "b_b", "b_b2", "brk_b", "a_b"],
        no=["intro_d", "LOOP", "a_d", "a_d", "b_d", "b_d", "brk_d", "a_d"],
    ),
)

# ================================================================ DECK_C ===
# "Open Sky" - D major / Mixolydian, ~164 BPM.  Hopeful and soaring: the
# bVII (C) gives it the heroic lift, rising sequences carry each phrase
# higher, the breakdown floats on long gliding notes before the climb back.
C_D, C_C, C_G, C_BM, C_A, C_EM, C_FSM = ("d4 f#4 a4 d5", "c4 e4 g4 c5", "d4 g4 b4 d5", "d4 f#4 b4 d5",
                                         "c#4 e4 a4 c#5", "e4 g4 b4 e5", "c#4 f#4 a4 c#5")

deck_c = dict(
    speed=(6, 5),
    key="d e f# g a b c# c",
    chords="D D D A "
           "D C G D Bm G A A D C G Bm G A D D "
           "G A F#m Bm G A D D Em F#m G A Bm G Em A "
           "Bm G D A Bm G Em A "
           "D C G D Bm G A A D C G Bm G A D D",
    patterns=dict(
        intro_l="r:16 | r:16 | r:16 | r:8 @leadb a4:2 b4:2 c#5:2 e5:2 |",
        a1=bars("@leadb f#5:2 d5:2 f#5:2 a5:4 g5:2 f#5:2 e5:2",
                "g5:6 e5:2 c5:4 e5:4",
                "d5:2 g5:2 b5:2 d6:4 c6:2 b5:2 a5:2",
                "a5:8 f#5:4 d5:4",
                "f#5:2 b5:2 d6:2 f#6:4 e6:2 d6:2 c#6:2",
                "d6:6 b5:2 g5:4 b5:4",
                "c#6:4 e6:4 a6:4 g6:2 f#6:2",
                "e6:12 r:4"),
        a2=bars("@leadb f#5:2 d5:2 f#5:2 a5:4 g5:2 f#5:2 e5:2",
                "g5:6 e5:2 c5:4 g5:4",
                "b5:2 d6:2 g6:2 d6:4 b5:2 g5:2 b5:2",
                "d6:8 c#6:4 b5:4",
                "g5:2 a5:2 b5:2 d6:4 b5:2 d6:2 e6:2",
                "e6:4 c#6:4 a5:4 e6:4",
                "f#6:6 e6:2 d6:8",
                "r:8 a5:2 b5:2 d6:2 e6:2"),
        b1=bars("@lead d6:6 b5:2 g5:8", "e6:6 c#6:2 a5:8", "f#6:6 c#6:2 a5:8", "b5:4 d6:4 f#6:8",
                "g6:6 d6:2 b5:8", "a6:6 e6:2 c#6:8", "d6:4 f#6:4 a6:8", "f#6:8 r:4 a5:4"),
        b2=bars("@lead g5:4 b5:4 e6:8", "a5:4 c#6:4 f#6:8", "b5:4 d6:4 g6:8", "a6:12 g6:2 f#6:2",
                "f#6:6 d6:2 b5:8", "g6:6 d6:2 b5:8", "e6:4 g6:4 b6:8", "a6:12 r:4"),
        brk=bars("@leadsoft b5:16", "~d6:8 b5:8", "a5:16", "~c#6:8 e6:8",
                 "f#6:16", "~g6:8 d6:8", "e6:16", "~g6:4 f#6:4 e6:4 c#6:4"),
        # arps
        intro_a="@pluckw " + roll([C_D, C_D, C_D, C_A], UP4) + " |",
        a_arp="@pluck " + roll([C_D, C_C, C_G, C_D, C_BM, C_G, C_A, C_A], UPDN) + " |",
        a_arp2="@pluck " + roll([C_D, C_C, C_G, C_BM, C_G, C_A, C_D, C_D], UPDN) + " |",
        b_arp="@pluckw " + roll([C_G, C_A, C_FSM, C_BM, C_G, C_A, C_D, C_D], [0, 2, 1, 3] * 4) + " |",
        b_arp2="@pluckw " + roll([C_EM, C_FSM, C_G, C_A, C_BM, C_G, C_EM, C_A], [0, 2, 1, 3] * 4) + " |",
        brk_a="@chord A37 b4:16 | A47 g4:16 | A47 d5:16 | A47 a4:16 | A37 b4:16 | A47 g4:16 | A37 e4:16 | A47 a4:8 A00 @pluck a4:2 c#5:2 e5:2 a5:2 |",
        a_harm=bars("@harm d5:2 a4:2 d5:2 f#5:4 e5:2 d5:2 c#5:2",
                    "e5:6 c5:2 g4:4 c5:4",
                    "g4:2 b4:2 d5:2 g5:4 g5:2 g5:2 f#5:2",
                    "f#5:8 d5:4 a4:4",
                    "d5:2 f#5:2 b5:2 d6:4 b5:2 b5:2 a5:2",
                    "b5:6 g5:2 d5:4 g5:4",
                    "a5:4 c#6:4 e6:4 e6:2 d6:2",
                    "c#6:12 r:4"),
        # bass
        intro_b="@bass " + bass("oct8", "d2", "d2", "d2", "a1"),
        a_b="@bass " + bass("gal", "d2", "c2", "g1", "d2", "b1", "g1", "a1", "a1"),
        a_b2="@bass " + bass("gal", "d2", "c2", "g1", "b1", "g1", "a1", "d2", "d2"),
        b_b="@bass " + bass("oct8", "g1", "a1", "f#1", "b1", "g1", "a1", "d2", "d2"),
        b_b2="@bass " + bass("oct8", "e1", "f#1", "g1", "a1", "b1", "g1", "e1", "a1"),
        brk_b="@bassl " + bass("half", "b1", "g1", "d2", "a1", "b1", "g1", "e1") + " @bass " + bass("drv", "a1"),
        # drums
        intro_d=drums("K.h.h.h.K.h.h.h.", "K.h.h.h.K.h.h.h.", "K.h.S.h.K.h.S.h.", "K.h.S.h.KSKSSSSS"),
        a_d=drums("C.h.S.h.K.hKS.h.", "K.h.S.h.KKh.S.h.", "K.h.S.h.K.hKS.h.", "K.h.S.h.KKh.S.hs",
                  "K.h.S.h.K.hKS.h.", "K.h.S.h.KKh.S.h.", "K.h.S.h.K.hKS.h.", "K.h.S.hSK.SKS.SS"),
        b_d=drums("C.o.S.o.K.o.S.o.", "K.o.S.o.K.o.S.oK", "K.o.S.o.K.o.S.o.", "K.o.S.o.K.oKS.SS",
                  "K.o.S.o.K.o.S.o.", "K.o.S.o.K.o.S.oK", "K.o.S.o.K.o.S.o.", "K.S.S.S.SSSSmmmm"),
        brk_d=drums("C...............", "k.......S.......", "k...............", "k.......S.......",
                    "k...............", "k.......S.......", "K...h...S...h...", "K.h.S.h.SSSSSSSS"),
    ),
    order=dict(
        p1=["intro_l", "LOOP", "a1", "a2", "b1", "b2", "brk", "a1", "a2"],
        p2=["intro_a", "LOOP", "a_arp", "a_arp2", "b_arp", "b_arp2", "brk_a", "a_harm", "a_arp2"],
        wv=["intro_b", "LOOP", "a_b", "a_b2", "b_b", "b_b2", "brk_b", "a_b", "a_b2"],
        no=["intro_d", "LOOP", "a_d", "a_d", "b_d", "b_d", "brk_d", "a_d", "a_d"],
    ),
)

# =================================================================== ZEN ===
# "Drift" - C Lydian / A minor pentatonic colours, ~82 BPM, no pressure.
# Music-box arps, a soft sub that breathes, a bell that rarely speaks.
Z_F, Z_G, Z_EM, Z_AM, Z_DM, Z_C = "f4 a4 c5 e5", "g4 b4 d5 e5", "e4 g4 b4 d5", "e4 a4 c5 e5", "d4 f4 a4 c5", "e4 g4 c5 d5"
ZPAT = [0, None, 1, None, 2, None, 3, None, 2, None, 1, None, 3, None, 2, None]
ZPAT2 = [3, None, 2, None, 1, None, 0, None, 1, None, 2, None, 0, None, 1, None]

zen = dict(
    speed=11,
    key="c d e f g a b",
    chords="Fmaj7 G6 Em7 Am7 Dm7 G6 C C "
           "Fmaj7 G6 Em7 Am7 Dm7 G6 C C "
           "Am7 Em7 Fmaj7 C Am7 Em7 Fmaj7 G6",
    patterns=dict(
        l1=bars("@bellsoft a5:8 c6:4 e6:4", "d6:12 b5:4", "g5:8 b5:4 e6:4", "c6:16",
                "f5:8 a5:4 c6:4", "b5:8 d6:4 g6:4", "e6:8 d6:4 g5:4", "c6:16"),
        l2=bars("r:16", "r:8 @bellsoft b6:4 g6:4", "r:16", "r:8 e6:4 c6:4",
                "r:16", "r:8 d6:4 b5:4", "g5:16", "r:16"),
        l3=bars("@bellsoft e6:8 c6:4 a5:4", "b5:12 g5:4", "a5:8 c6:4 e6:4", "g6:16",
                "e6:8 c6:4 a5:4", "d6:12 b5:4", "c6:8 a5:4 e5:4", "d5:16"),
        a1="@mbox " + roll([Z_F, Z_G, Z_EM, Z_AM, Z_DM, Z_G, Z_C, Z_C], ZPAT) + " |",
        a2="@mbox " + roll([Z_F, Z_G, Z_EM, Z_AM, Z_DM, Z_G, Z_C, Z_C], ZPAT2) + " |",
        a3="@mbox " + roll([Z_AM, Z_EM, Z_F, Z_C, Z_AM, Z_EM, Z_F, Z_G], ZPAT) + " |",
        b1="@sub " + bass("whole", "f1", "g1", "e1", "a1", "d1", "g1", "c1", "c1"),
        b2="@sub " + bass("half", "f1", "g1", "e1", "a1", "d1", "g1", "c1/g1", "c1/g1"),
        b3="@sub " + bass("whole", "a1", "e1", "f1", "c1", "a1", "e1", "f1", "g1"),
        d1=drums("z...............", "................", "........t.......", "................",
                 "z...............", "........t.......", "................", "....t.......t..."),
        d2=drums("z.....t.z.......", "z.....t.z.......", "z.....t.z.......", "z.....t.z...t...",
                 "z.....t.z.......", "z.....t.z.......", "z.....t.z.......", "z.....t.z.t.t..."),
    ),
    order=dict(
        p1=["LOOP", "l1", "l2", "l3"],
        p2=["LOOP", "a1", "a2", "a3"],
        wv=["LOOP", "b1", "b2", "b3"],
        no=["LOOP", "d1", "d2", "d2"],
    ),
)

# ============================================================== GAMEOVER ===
# "Shutdown" - ~5 s, D minor, not looping.  A slow falling line over
# Bb - Gm - A(sus4 -> 3), resolving to a quiet D major chord (Picardy third):
# defeat with dignity, and the hint of another try.
gameover = dict(
    speed=6,
    key="d e f g a bb c",
    chords="Bb Gm A",
    patterns=dict(
        l=bars("@lead d6:6 c6:2 bb5:8", "g5:6 a5:2 bb5:8", "@leadsoft a5:4 d6:4 ~c#6:8") + " @lead d6:8",
        h=bars("@harm f5:8 d5:8", "g4:8 d5:8", "@chord A57 a4:8 A47 a4:8") + " A47 d5:8",
        b="@basss bb1:16 | g1:16 | a1:16 | @bassl d2:8",
        d="C...............k...............k.......k.......C.......",
    ),
    order=dict(p1=["l"], p2=["h"], wv=["b"], no=["d"]),
)

SONGS = dict(TITLE=title, DECK_A=deck_a, DECK_B=deck_b, DECK_C=deck_c, ZEN=zen, GAMEOVER=gameover)
