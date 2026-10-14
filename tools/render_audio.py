#!/usr/bin/env python3
"""render_audio.py - build a tiny sound-test ROM and render songs/sfx to WAV
with PyBoy (headless), plus measure sound_tick() cost on the emulated CPU.

    python3 tools/render_audio.py                 # all songs + sfx -> build/audio/*.wav
    python3 tools/render_audio.py --song 1 --secs 60
    python3 tools/render_audio.py --bench         # cycle stats only
    python3 tools/render_audio.py --png           # + spectrogram PNGs and a level report
    python3 tools/render_audio.py --solo          # + one WAV per channel (NR51 solo) per song
    python3 tools/render_audio.py --roll          # piano-roll PNGs straight from the song data

Needs /opt/gbdk and `pip install pyboy numpy pillow`.  Output goes to build/.
Note: PyBoy mixes digital levels, so DAC pops are NOT audible here; the
register-level pop rules are enforced by tests/test_sound.c instead.
"""
import argparse
import os
import subprocess
import sys
import wave

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BUILD = os.path.join(ROOT, "build", "sndtest")
LCC = os.environ.get("LCC", "/opt/gbdk/bin/lcc")

sys.path.insert(0, os.path.join(ROOT, "tools"))
import gen_music  # noqa: E402

SONGS = [n.lower() for n in gen_music.SONG_ORDER]
SFX = [n.lower() for n in gen_music.SFX_ORDER]

MAIN_C = r"""
#include <gb/gb.h>
#include <stdint.h>
#include "sound.h"
/* mailbox at 0xD800: [0]=song req (0xFF none) [1]=sfx req [2]=rewind [3]=muffle
   [4]=NR51 solo mask (0 = leave alone)
   [8..9]=last tick cost [10..11]=max cost [12..15]=sum (units of 16 M-cycles) */
#define MB ((volatile uint8_t *)0xD800)
void main(void)
{
    uint16_t cost, mx = 0; uint32_t sum = 0;
    MB[0] = 0xFF; MB[1] = 0xFF; MB[2] = 0; MB[3] = 0; MB[4] = 0;
    sound_init();
    TAC_REG = 0x06;                    /* 65536 Hz: 1 count = 64 T = 16 M-cycles */
    while (1) {
        wait_vbl_done();
        if (MB[0] != 0xFF) { if (MB[0] == 0xFE) music_stop(); else music_play(MB[0]); MB[0] = 0xFF; }
        if (MB[1] != 0xFF) { sfx_play(MB[1]); MB[1] = 0xFF; }
        music_set_rewind(MB[2]);
        music_set_muffle(MB[3]);
        if (MB[4]) { NR51_REG = MB[4]; MB[4] = 0; }
        disable_interrupts();
        TIMA_REG = 0; IF_REG &= ~TIM_IFLAG;
        sound_tick();
        cost = TIMA_REG;
        if (IF_REG & TIM_IFLAG) cost += 256;
        enable_interrupts();
        if (cost > mx) mx = cost;
        sum += cost;
        MB[8] = cost; MB[9] = cost >> 8; MB[10] = mx; MB[11] = mx >> 8;
        MB[12] = sum; MB[13] = sum >> 8; MB[14] = sum >> 16; MB[15] = sum >> 24;
    }
}
"""


def build_rom():
    os.makedirs(BUILD, exist_ok=True)
    main = os.path.join(BUILD, "main.c")
    with open(main, "w") as f:
        f.write(MAIN_C)
    rom = os.path.join(BUILD, "sndtest.gb")
    src = os.path.join(ROOT, "src", "gb")
    cmd = [LCC, "-Wm-yC", "-Wm-yt0x19", "-autobank", "-I" + src, "-o", rom, main,
           os.path.join(src, "sound.c"), os.path.join(src, "music_data.c")]
    subprocess.check_call(cmd)
    return rom


class Rig:
    def __init__(self, rom):
        from pyboy import PyBoy
        self.pb = PyBoy(rom, window="null", sound_emulated=True, sound_sample_rate=48000)
        self.m = self.pb.memory
        self.pb.tick(400, False, True)  # past the boot ROM
        self.samples = []

    def frame(self, n=1, keep=True):
        import numpy as np
        for _ in range(n):
            self.pb.tick(1, False, True)
            if keep:
                self.samples.append(np.array(self.pb.sound.ndarray, copy=True))

    def cmd(self, idx, val):
        self.m[0xD800 + idx] = val

    def cost(self):
        m = self.m
        last = m[0xD808] | m[0xD809] << 8
        mx = m[0xD80A] | m[0xD80B] << 8
        s = m[0xD80C] | m[0xD80D] << 8 | m[0xD80E] << 16 | m[0xD80F] << 24
        return last * 16, mx * 16, s * 16

    def take(self):
        import numpy as np
        a = np.concatenate(self.samples) if self.samples else np.zeros((0, 2), dtype=np.int8)
        self.samples = []
        return a


def write_wav(path, a, rate=48000):
    import numpy as np
    pcm = (a.astype(np.int16) * 256).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(2)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(pcm.tobytes())


def stats(a):
    import numpy as np
    if len(a) == 0:
        return "empty"
    x = a.astype(np.float64)
    rms = np.sqrt((x ** 2).mean())
    peak = np.abs(x).max()
    clip = (np.abs(a.astype(np.int16)) >= 127).mean() * 100
    return "rms %5.1f  peak %3d  clip %.2f%%" % (rms, peak, clip)


# ---------------------------------------------------------------------------
# analysis helpers
# ---------------------------------------------------------------------------
def spectrogram_png(a, path, rate=48000, title=""):
    """Log-frequency spectrogram (mono mix), 50-12000 Hz, 1 px = 1/30 s,
    plus a level strip (RMS per column; red where |x| >= 120)."""
    import numpy as np
    from PIL import Image, ImageDraw
    x = a.astype(np.float32).mean(axis=1) if a.ndim == 2 else a.astype(np.float32)
    hop = rate // 30
    n = 4096
    win = np.hanning(n).astype(np.float32)
    cols = max(1, (len(x) - n) // hop)
    H = 300
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    edges = np.geomspace(50, 12000, H + 1)
    idx = np.searchsorted(freqs, edges)
    img = np.zeros((H + 40, cols), dtype=np.uint8)
    rgb = np.zeros((H + 40, cols, 3), dtype=np.uint8)
    for c in range(cols):
        seg = x[c * hop:c * hop + n]
        if len(seg) < n:
            break
        sp = np.abs(np.fft.rfft(seg * win))
        col = np.array([sp[idx[i]:max(idx[i] + 1, idx[i + 1])].max() for i in range(H)])
        db = 20 * np.log10(col + 1e-3)
        v = np.clip((db - 20) * 3.2, 0, 255)
        img[:H, c] = v[::-1]
        raw = a[c * hop:(c + 1) * hop]
        rms = float(np.sqrt((raw.astype(np.float32) ** 2).mean()))
        hgt = min(38, int(rms))
        clip = (np.abs(raw.astype(np.int16)) >= 120).any()
        rgb[H + 40 - hgt:, c] = (255, 60, 60) if clip else (80, 220, 120)
    rgb[:H, :, 0] = img[:H]
    rgb[:H, :, 1] = (img[:H].astype(np.int32) * 6 // 10).astype(np.uint8)
    rgb[:H, :, 2] = np.clip(img[:H].astype(np.int32) * 12 // 10, 0, 255).astype(np.uint8)
    im = Image.fromarray(rgb, "RGB")
    d = ImageDraw.Draw(im)
    for hz in (100, 250, 500, 1000, 2000, 4000, 8000):
        yy = H - int(np.searchsorted(edges, hz) * H / len(edges))
        d.line([(0, yy), (6, yy)], fill=(255, 255, 255))
        d.text((8, yy - 6), str(hz), fill=(200, 200, 200))
    for sec in range(0, cols // 30 + 1, 5):
        d.line([(sec * 30, H), (sec * 30, H + 4)], fill=(255, 255, 255))
    d.text((40, 2), title, fill=(255, 255, 0))
    im.save(path)


def band_report(a, rate=48000):
    """fraction of energy above 4 kHz / 8 kHz, and DC-free RMS"""
    import numpy as np
    x = a.astype(np.float32).mean(axis=1) if a.ndim == 2 else a.astype(np.float32)
    x = x - x.mean()
    sp = np.abs(np.fft.rfft(x)) ** 2
    f = np.fft.rfftfreq(len(x), 1.0 / rate)
    tot = sp.sum() + 1e-9
    return sp[f > 4000].sum() / tot, sp[f > 8000].sum() / tot, float(np.sqrt((x ** 2).mean()))


def piano_rolls(out):
    """One PNG per song from the song data: p1 red, p2 cyan, wave green,
    drums at the bottom; bar lines every 16 rows, loop point in yellow."""
    import runpy
    from PIL import Image, ImageDraw
    helpers = dict(flatten=gen_music.flatten, NOTE_BASE=gen_music.NOTE_BASE, midi_of=gen_music.midi_of,
                   hz_of_midi=gen_music.hz_of_midi, reg_of_hz=gen_music.reg_of_hz, math=__import__("math"))
    src = runpy.run_path(os.path.join(ROOT, "assets", "music", "songs.py"), init_globals=helpers)
    names = [n for n, _ in src["INSTRUMENTS"]]
    for sname in gen_music.SONG_ORDER:
        song = src["SONGS"][sname]
        fl = [gen_music.flatten(song, names, c) for c in range(4)]
        total = fl[0][1]
        loop = fl[0][2]
        X = 3
        W, Hh = total * X + 1, 72 * 5 + 40
        im = Image.new("RGB", (W, Hh), (8, 8, 16))
        d = ImageDraw.Draw(im)
        for b in range(0, total + 1, 16):
            d.line([(b * X, 0), (b * X, Hh)], fill=(40, 40, 60) if b % 64 else (90, 90, 120))
        if loop is not None:
            d.line([(loop * X, 0), (loop * X, Hh)], fill=(220, 220, 0))
        for m in range(24, 96, 12):
            yy = (96 - m) * 5
            d.line([(0, yy), (W, yy)], fill=(30, 30, 40))
            d.text((2, yy - 10), "C%d" % (m // 12 - 1), fill=(120, 120, 120))
        cols = [(255, 80, 80), (80, 220, 255), (80, 255, 120)]
        for ch in range(3):
            for (r, n, kind, m, inst, arp, gl) in fl[ch][0]:
                if kind != "note":
                    continue
                yy = (96 - m) * 5
                d.rectangle([r * X, yy, (r + n) * X - 1, yy + 3], fill=cols[ch])
                if arp:
                    for k in (arp >> 4, arp & 15):
                        if k:
                            d.rectangle([r * X, yy - k * 5, (r + n) * X - 1, yy - k * 5 + 1], fill=(160, 160, 255))
        dy = {"K": 0, "k": 0, "m": 4, "S": 8, "s": 8, "T": 12, "t": 16, "h": 20, "H": 20, "o": 24, "C": 28, "z": 32}
        for (r, n, kind, m, *_ ) in fl[3][0]:
            yy = 72 * 5 + 2 + dy.get(m, 36) // 1
            d.rectangle([r * X, yy, r * X + X - 1, yy + 3], fill=(255, 200, 80))
        d.text((4, 2), sname, fill=(255, 255, 0))
        im.save(os.path.join(out, "roll_%s.png" % sname.lower()))
        print("piano roll:", os.path.join(out, "roll_%s.png" % sname.lower()))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--song", type=int, default=None)
    ap.add_argument("--secs", type=float, default=None)
    ap.add_argument("--bench", action="store_true")
    ap.add_argument("--png", action="store_true")
    ap.add_argument("--solo", action="store_true")
    ap.add_argument("--roll", action="store_true")
    ap.add_argument("--nosfx", action="store_true")
    args = ap.parse_args()
    out = os.path.join(ROOT, "build", "audio")
    os.makedirs(out, exist_ok=True)
    if args.roll:
        piano_rolls(out)
        return
    rom = build_rom()
    rig = Rig(rom)

    songs = [args.song] if args.song is not None else range(len(SONGS))
    worst = 0
    import numpy as np
    for s in songs:
        frames = 0
        try:
            import re
            txt = open(os.path.join(ROOT, "src", "gb", "music_data.c")).read()
            frames = int(re.search(r"snd_song_frames\[\d+\] = \{([^}]*)\}", txt).group(1).split(",")[s])
        except Exception:
            pass
        secs = args.secs or (frames / 59.73 + (1 if SONGS[s] == "gameover" else 8))
        rig.cost()
        rig.cmd(0, s)
        rig.frame(2, keep=False)
        base = rig.cost()[2]
        # reset max by rebuilding not possible; track via deltas
        n = int(secs * 60)
        peak = 0
        costs = []
        for i in range(n):
            rig.frame(1, keep=not args.bench)
            costs.append(rig.cost()[0])
            peak = max(peak, costs[-1])
        costs.sort()
        total = rig.cost()[2] - base
        worst = max(worst, peak)
        a = rig.take()
        if not args.bench:
            wpath = os.path.join(out, "song_%d_%s.wav" % (s, SONGS[s]))
            write_wav(wpath, a)
            if args.png:
                spectrogram_png(a, wpath[:-4] + ".png", title=SONGS[s])
                h4, h8, rms = band_report(a)
                print("     >4k %.1f%%  >8k %.2f%%  rms %.1f" % (h4 * 100, h8 * 100, rms))
        print("song %-9s %5.1fs  tick avg %4d median %4d p90 %4d max %4d M-cycles  %s" %
              (SONGS[s], secs, total // n, costs[n // 2], costs[n * 9 // 10], peak,
               "" if args.bench else stats(a)))
        rig.cmd(0, 0xFE)
        rig.frame(10, keep=False)
        if args.solo and not args.bench:
            levels = []
            for ch, mask in enumerate((0x11, 0x22, 0x44, 0x88)):
                rig.cmd(4, mask)
                rig.frame(2, keep=False)
                rig.cmd(0, s)
                rig.frame(int(min(secs, 40) * 60))
                a = rig.take()
                rig.cmd(0, 0xFE)
                rig.frame(10, keep=False)
                write_wav(os.path.join(out, "solo_%s_ch%d.wav" % (SONGS[s], ch + 1)), a)
                x = a.astype(np.float32).mean(axis=1)
                levels.append(float(np.sqrt(((x - x.mean()) ** 2).mean())))
            rig.cmd(4, 0xFF)
            rig.frame(2, keep=False)
            print("     solo rms: p1 %.1f  p2 %.1f  wave %.1f  noise %.1f" % tuple(levels))
    if args.song is None and not args.nosfx:
        # sfx: over silence, then over music
        for i, name in enumerate(SFX):
            rig.cmd(1, i)
            peak = 0
            for _ in range(90):
                rig.frame(1)
                peak = max(peak, rig.cost()[0])
            worst = max(worst, peak)
            a = rig.take()
            if not args.bench:
                wpath = os.path.join(out, "sfx_%02d_%s.wav" % (i, name))
                write_wav(wpath, a)
                if args.png:
                    spectrogram_png(a, wpath[:-4] + ".png", title=name)
            print("sfx  %-10s tick max %4d M-cycles  %s" % (name, peak, "" if args.bench else stats(a)))
        # rewind + muffle demo on DECK_A
        rig.cmd(0, 1)
        rig.frame(240)
        rig.cmd(2, 1)
        rig.cmd(1, SFX.index("rewind"))
        peak = 0
        for _ in range(180):
            rig.frame(1)
            peak = max(peak, rig.cost()[0])
        rig.cmd(2, 0)
        rig.frame(180)
        rig.cmd(3, 1)
        rig.frame(180)
        rig.cmd(1, SFX.index("menu"))
        rig.frame(60)
        rig.cmd(3, 0)
        rig.frame(120)
        a = rig.take()
        worst = max(worst, peak)
        if not args.bench:
            write_wav(os.path.join(out, "fx_rewind_muffle.wav"), a)
        print("fx rewind/muffle demo: rewind tick max %d M-cycles  %s" % (peak, stats(a)))
    print("worst sound_tick: %d M-cycles (%.1f%% of a 17556 M-cycle frame)" % (worst, worst * 100.0 / 17556))


if __name__ == "__main__":
    main()
