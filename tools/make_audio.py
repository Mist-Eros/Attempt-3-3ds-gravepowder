#!/usr/bin/env python3
"""Synthesizes the entire Gravepowder sound set into data/*.bin.

Everything here is original synthesis - no samples are imported, not even the
original game's.  Like tools/make_icon.py this is kept in the repo so the
soundtrack can be regenerated and tweaked instead of living as opaque blobs.

    pip install --quiet numpy
    python tools/make_audio.py

Output format: mono PCM16 little-endian.
  * SFX   -> 22050 Hz, peak normalised to -3 dBFS, trimmed, 5 ms fades.
  * music -> 16000 Hz seamless loops, peak -6 dBFS, 10 ms fades at the seams.

The 3DS Makefile turns each data/*.bin into an object with bin2o, which is why
source/audio_3ds.c can include build/<name>_bin.h and point ndsp at the data.

Tone (docs/design.md): dusty Western + rot.  Old guns, zombies, wind, bells.
The two tracks are simple original compositions in A minor (menu) and D minor
(zone) built from drones, plucks, bells, a heartbeat pulse and wind noise.
"""

import math
import os
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)
OUT_DIR = os.path.join(REPO, "data")

SFX_RATE = 22050
MUSIC_RATE = 16000
SFX_PEAK_DB = -3.0
MUSIC_PEAK_DB = -6.0
SFX_FADE_MS = 5.0
MUSIC_FADE_MS = 10.0
BYTES_BUDGET = 2500 * 1024


# ---------------------------------------------------------------------------
# small DSP helpers
# ---------------------------------------------------------------------------
def t_axis(n, sr):
    return np.arange(n, dtype=np.float64) / float(sr)


def sine_sweep(n, sr, f0, f1, phase0=0.0):
    t = t_axis(n, sr)
    freq = np.linspace(f0, f1, n, dtype=np.float64)
    phase = phase0 + 2.0 * np.pi * np.cumsum(freq) / float(sr)
    return np.sin(phase)


def exp_env(n, sr, attack_s, decay_s, power=2.0):
    t = t_axis(n, sr)
    a = max(1, int(attack_s * sr))
    env = np.ones(n, dtype=np.float64)
    env[:a] = np.linspace(0.0, 1.0, a, dtype=np.float64)
    d = np.maximum(t - attack_s, 0.0)
    env *= np.exp(-d / max(decay_s, 1e-4))
    return env ** power if power != 1.0 else env


def exp_decay(n, sr, decay_s, attack_s=0.002):
    t = t_axis(n, sr)
    a = max(1, int(attack_s * sr))
    env = np.exp(-t / max(decay_s, 1e-4))
    env[:a] *= np.linspace(0.0, 1.0, a, dtype=np.float64)
    return env


def fft_band(x, sr, lo=None, hi=None):
    """Zero-phase FFT band filter (cheap and good enough for noise shaping)."""
    n = len(x)
    spec = np.fft.rfft(x)
    freqs = np.fft.rfftfreq(n, d=1.0 / sr)
    mask = np.ones_like(freqs)
    if lo is not None:
        mask *= 1.0 / (1.0 + np.exp(-(freqs - lo) / 20.0))
    if hi is not None:
        mask *= 1.0 / (1.0 + np.exp((freqs - hi) / 20.0))
    return np.fft.irfft(spec * mask, n)


def noise(n, sr, lo=None, hi=None, seed=0):
    rng = np.random.default_rng(seed)
    x = rng.standard_normal(n)
    if lo is not None or hi is not None:
        x = fft_band(x, sr, lo, hi)
    peak = np.max(np.abs(x)) if n else 0.0
    return x / peak if peak > 0 else x


def add(out, sig, start, gain=1.0):
    n = len(out)
    start = int(start)
    if start >= n or start < 0 or len(sig) == 0:
        return
    end = min(n, start + len(sig))
    out[start:end] += sig[: end - start] * gain


def normalize(x, peak_db):
    peak = float(np.max(np.abs(x))) if len(x) else 0.0
    if peak <= 1e-9:
        return x
    return np.clip(x * (10.0 ** (peak_db / 20.0) / peak), -1.0, 1.0)


def fade(x, sr, ms):
    n = len(x)
    k = max(1, min(n // 2, int(sr * ms / 1000.0)))
    if n < 4:
        return x
    ramp = np.linspace(0.0, 1.0, k)
    y = x.copy()
    y[:k] *= ramp
    y[-k:] *= ramp[::-1]
    return y


def trim_silence(x, sr, threshold=0.003, pad_ms=2.0):
    if len(x) == 0:
        return x
    idx = np.nonzero(np.abs(x) > threshold)[0]
    if len(idx) == 0:
        return x
    pad = max(1, int(sr * pad_ms / 1000.0))
    return x[max(0, int(idx[0]) - pad): min(len(x), int(idx[-1]) + 1 + pad)]


def write_pcm16(path, x):
    pcm = np.clip(np.round(np.asarray(x) * 32767.0), -32768, 32767).astype("<i2")
    with open(path, "wb") as f:
        f.write(pcm.tobytes())
    return len(pcm)


# ---------------------------------------------------------------------------
# instruments / one-shot sound design
# ---------------------------------------------------------------------------
def gunshot(sr, seed, boom_f0=95.0, boom_f1=38.0, crack_hi=5200.0,
            crack_lo=1400.0, crack_decay=0.012, boom_decay=0.20,
            hiss_decay=0.35, length=0.9, hiss_gain=0.22, tail_gain=0.10):
    """Crack + low boom + smoke hiss.  Returns normalised float array."""
    n = int(length * sr)
    rng = np.random.default_rng(seed)
    out = np.zeros(n)

    # supersonic crack: short filtered noise burst
    crack_n = int(0.06 * sr)
    crack = rng.standard_normal(crack_n)
    crack = fft_band(crack, sr, crack_lo, crack_hi)
    crack *= exp_decay(crack_n, sr, crack_decay, 0.0005)
    add(out, crack, 0, 1.0)

    # low boom: pitch-swept sine + its octave, slow decay
    boom = sine_sweep(n, sr, boom_f0, boom_f1)
    boom *= exp_decay(n, sr, boom_decay, 0.001)
    add(out, boom, 0, 0.9)
    sub = sine_sweep(n, sr, boom_f0 * 0.55, boom_f1 * 0.6)
    sub *= exp_decay(n, sr, boom_decay * 1.7, 0.002)
    add(out, sub, 0, 0.55)

    # body knock (wood/brass)
    knock_n = int(0.09 * sr)
    knock_t = t_axis(knock_n, sr)
    knock = np.sin(2 * np.pi * 180.0 * knock_t) * np.exp(-knock_t / 0.02)
    knock += 0.5 * np.sin(2 * np.pi * 320.0 * knock_t) * np.exp(-knock_t / 0.012)
    add(out, knock, 0, 0.5)

    # smoke hiss: band noise with a soft attack, long tail
    hiss_n = int(length * sr)
    hiss = rng.standard_normal(hiss_n)
    hiss = fft_band(hiss, sr, 900.0, 7000.0)
    hiss *= exp_decay(hiss_n, sr, hiss_decay, 0.004)
    hiss *= 0.6 + 0.4 * np.sin(2 * np.pi * 7.0 * t_axis(hiss_n, sr))
    add(out, hiss, int(0.03 * sr), hiss_gain)

    # distant tail slap (open air)
    tail_n = int(0.5 * sr)
    tail = rng.standard_normal(tail_n)
    tail = fft_band(tail, sr, 120.0, 900.0)
    tail *= exp_decay(tail_n, sr, 0.22, 0.02)
    delay = int(0.06 * sr)
    echo = np.zeros(tail_n + delay)
    echo[delay:] += tail
    echo[:tail_n] += tail * 0.4
    add(out, echo, int(0.05 * sr), tail_gain)

    return normalize(np.tanh(out * 0.9), SFX_PEAK_DB)


def click(sr, dur, freqs=(2400.0, 3700.0), decay=0.008, seed=1):
    n = int(dur * sr)
    out = np.zeros(n)
    for i, f in enumerate(freqs):
        tt = t_axis(n, sr)
        out += (0.8 / (i + 1)) * np.sin(2 * np.pi * f * tt) * exp_decay(n, sr, decay)
    rng = np.random.default_rng(seed)
    tick = rng.standard_normal(int(0.006 * sr)) * exp_decay(int(0.006 * sr), sr, 0.002)
    add(out, tick, 0, 0.7)
    return normalize(out, SFX_PEAK_DB)


def flintlock(sr):
    return gunshot(sr, seed=11, boom_f0=105.0, boom_f1=42.0, length=0.95,
                   crack_hi=6200.0, crack_lo=1600.0, hiss_gain=0.26,
                   tail_gain=0.12)


def revolver_variant(sr, variant):
    # snappier and brighter than the flintlock, three slightly different barrels
    hi = [5400.0, 6000.0, 5000.0][variant]
    return gunshot(sr, seed=20 + variant, boom_f0=130.0 + 10 * variant,
                   boom_f1=58.0, length=0.46, crack_hi=hi,
                   crack_lo=2000.0, crack_decay=0.008,
                   boom_decay=0.10, hiss_decay=0.14, hiss_gain=0.16,
                   tail_gain=0.05)


def musket(sr):
    return gunshot(sr, seed=31, boom_f0=72.0, boom_f1=30.0, length=1.4,
                   crack_hi=4200.0, crack_lo=900.0, crack_decay=0.018,
                   boom_decay=0.34, hiss_decay=0.55, hiss_gain=0.30,
                   tail_gain=0.18)


def reload_flintlock(sr):
    """Powder measure, frizzen snap, ramrod tap, hammer cock."""
    n = int(0.9 * sr)
    out = np.zeros(n)
    add(out, click(sr, 0.12, (2600.0, 4100.0), 0.010, 41), 0.00, 0.9)   # frizzen
    add(out, click(sr, 0.10, (1800.0, 2900.0), 0.012, 42), 0.28, 0.7)   # powder
    add(out, click(sr, 0.14, (1400.0, 2200.0), 0.016, 43), 0.52, 0.8)   # ramrod
    add(out, click(sr, 0.12, (3000.0, 4700.0), 0.008, 44), 0.78, 0.6)   # cock
    rng = np.random.default_rng(45)
    grains = rng.standard_normal(int(0.35 * sr)) * exp_decay(int(0.35 * sr), sr, 0.12, 0.01)
    grains = fft_band(grains, sr, 2000.0, 8000.0)
    add(out, grains, int(0.28 * sr), 0.10)
    return normalize(np.tanh(out * 1.2), SFX_PEAK_DB)


def reload_revolver(sr):
    """Cylinder spin ratchet (5 clicks), hammer cock, primer seat."""
    n = int(1.2 * sr)
    out = np.zeros(n)
    add(out, click(sr, 0.1, (2200.0, 3400.0), 0.008, 51), 0.0, 0.8)
    for i in range(6):
        t0 = 0.10 + i * 0.085
        add(out, click(sr, 0.07, (3000.0 + 240.0 * i, 4600.0), 0.005, 60 + i),
            t0 * sr, 0.72 - 0.06 * i)
    add(out, click(sr, 0.12, (1500.0, 2500.0), 0.012, 71), 0.72, 0.8)   # hammer
    add(out, click(sr, 0.10, (2600.0, 3900.0), 0.010, 72), 0.98, 0.7)   # primer
    rng = np.random.default_rng(73)
    spin = rng.standard_normal(int(0.5 * sr)) * exp_decay(int(0.5 * sr), sr, 0.15, 0.02)
    spin = fft_band(spin, sr, 1200.0, 5000.0)
    add(out, spin, int(0.1 * sr), 0.08)
    return normalize(np.tanh(out * 1.2), SFX_PEAK_DB)


def reload_musket(sr):
    """Long scrape + ramrod thunk + barrel ring."""
    n = int(1.5 * sr)
    out = np.zeros(n)
    # scrape
    scrape_n = int(0.45 * sr)
    rng = np.random.default_rng(81)
    scrape = rng.standard_normal(scrape_n)
    scrape = fft_band(scrape, sr, 700.0, 3200.0)
    scrape *= 0.5 + 0.5 * np.sin(2 * np.pi * 11.0 * t_axis(scrape_n, sr))
    scrape *= np.minimum(1.0, t_axis(scrape_n, sr) / 0.05)
    scrape *= np.exp(-t_axis(scrape_n, sr) / 0.3)
    add(out, scrape, 0.0, 0.22)
    add(out, click(sr, 0.12, (2200.0, 3400.0), 0.012, 82), 0.55, 0.7)   # powder
    # ramrod thunk
    thunk_n = int(0.25 * sr)
    thunk = sine_sweep(thunk_n, sr, 150.0, 70.0) * exp_decay(thunk_n, sr, 0.06, 0.001)
    add(out, thunk, int(0.85 * sr), 0.75)
    add(out, click(sr, 0.12, (1200.0, 1900.0), 0.02, 83), 0.88, 0.6)
    # barrel ring
    ring_n = int(0.6 * sr)
    tt = t_axis(ring_n, sr)
    ring = (np.sin(2 * np.pi * 620.0 * tt) * 0.5 +
            np.sin(2 * np.pi * 930.0 * tt) * 0.3) * exp_decay(ring_n, sr, 0.18)
    add(out, ring, int(1.0 * sr), 0.18)
    return normalize(np.tanh(out * 1.1), SFX_PEAK_DB)


def empty_click(sr):
    n = int(0.15 * sr)
    out = np.zeros(n)
    add(out, click(sr, 0.1, (3200.0, 5200.0), 0.006, 91), 0.0, 0.9)
    add(out, click(sr, 0.09, (1200.0, 2100.0), 0.012, 92), 0.05, 0.5)
    return normalize(out, SFX_PEAK_DB)


def hit_thud(sr, seed=101):
    n = int(0.2 * sr)
    out = sine_sweep(n, sr, 170.0, 55.0) * exp_decay(n, sr, 0.05, 0.001)
    rng = np.random.default_rng(seed)
    slap = rng.standard_normal(int(0.05 * sr))
    slap = fft_band(slap, sr, 300.0, 2500.0) * exp_decay(len(slap), sr, 0.02)
    add(out, slap, 0, 0.5)
    return normalize(out, SFX_PEAK_DB)


def zombie_death(sr):
    """Wet gurgle (noise-modulated formants) collapsing into a ground thud."""
    n = int(0.62 * sr)
    out = np.zeros(n)
    rng = np.random.default_rng(111)
    gur_n = int(0.42 * sr)
    src = rng.standard_normal(gur_n)
    src = fft_band(src, sr, 300.0, 1600.0)
    # formant-ish AM: wobbling vowel
    tt = t_axis(gur_n, sr)
    vib = 1.0 + 0.45 * np.sin(2 * np.pi * (9.0 + 5.0 * tt) * tt)
    gur = src * vib
    gur *= np.minimum(1.0, tt / 0.02) * np.exp(-tt / 0.22)
    add(out, gur, 0.0, 1.0)
    # final collapse thud
    thud_n = int(0.25 * sr)
    thud = sine_sweep(thud_n, sr, 120.0, 45.0) * exp_decay(thud_n, sr, 0.08, 0.001)
    rng2 = np.random.default_rng(112)
    dirt = rng2.standard_normal(int(0.12 * sr))
    dirt = fft_band(dirt, sr, 400.0, 3000.0) * exp_decay(len(dirt), sr, 0.05)
    add(out, thud, int(0.38 * sr), 0.8)
    add(out, dirt, int(0.40 * sr), 0.35)
    return normalize(np.tanh(out * 1.1), SFX_PEAK_DB)


def zombie_attack(sr):
    """Airy swipe whoosh + short groan."""
    n = int(0.38 * sr)
    out = np.zeros(n)
    rng = np.random.default_rng(121)
    whoosh_n = int(0.2 * sr)
    whoosh = rng.standard_normal(whoosh_n)
    whoosh = fft_band(whoosh, sr, 500.0, 2600.0)
    tt = t_axis(whoosh_n, sr)
    whoosh *= np.sin(np.pi * np.minimum(tt / 0.2, 1.0)) ** 2
    add(out, whoosh, 0.0, 0.8)
    # groan: low saw-ish voice with wobble
    gr_n = int(0.26 * sr)
    gt = t_axis(gr_n, sr)
    base = 95.0 + 12.0 * np.sin(2 * np.pi * 4.0 * gt)
    ph = 2 * np.pi * np.cumsum(base) / sr
    groan = np.sin(ph) + 0.4 * np.sin(2 * ph) + 0.2 * np.sin(3 * ph)
    groan *= np.minimum(1.0, gt / 0.03) * np.exp(-gt / 0.16)
    add(out, groan, int(0.10 * sr), 0.55)
    return normalize(out, SFX_PEAK_DB)


def player_hurt(sr):
    """Short male grunt: voiced pulse + breath noise."""
    n = int(0.3 * sr)
    out = np.zeros(n)
    tt = t_axis(n, sr)
    base = 150.0 - 40.0 * tt / 0.3
    ph = 2 * np.pi * np.cumsum(base) / sr
    voice = np.sin(ph) + 0.5 * np.sin(2 * ph) + 0.3 * np.sin(3 * ph)
    voice *= np.minimum(1.0, tt / 0.015) * np.exp(-tt / 0.12)
    add(out, voice, 0, 0.7)
    rng = np.random.default_rng(131)
    breath = rng.standard_normal(int(0.18 * sr))
    breath = fft_band(breath, sr, 400.0, 2400.0)
    breath *= np.minimum(1.0, t_axis(len(breath), sr) / 0.01) * \
              np.exp(-t_axis(len(breath), sr) / 0.07)
    add(out, breath, 0, 0.4)
    return normalize(np.tanh(out * 1.2), SFX_PEAK_DB)


def coin_pickup(sr):
    """Brass coin chime: two bright inharmonic partials + tiny shimmer."""
    n = int(0.35 * sr)
    out = np.zeros(n)
    tt = t_axis(n, sr)
    for f, a, d in ((1320.0, 0.6, 0.16), (1980.0, 0.35, 0.12),
                    (2640.0, 0.22, 0.09)):
        out += a * np.sin(2 * np.pi * f * tt) * exp_decay(n, sr, d, 0.001)
    out += 0.12 * np.sin(2 * np.pi * 3960.0 * tt) * exp_decay(n, sr, 0.06, 0.001)
    return normalize(out, SFX_PEAK_DB)


def squelch(sr):
    """Wet sinew squelch: band noise with a fast downward 'suck'."""
    n = int(0.25 * sr)
    rng = np.random.default_rng(141)
    src = rng.standard_normal(n)
    src = fft_band(src, sr, 200.0, 1800.0)
    tt = t_axis(n, sr)
    mod = 1.0 + 0.8 * np.sin(2 * np.pi * (40.0 - 25.0 * tt) * tt)
    out = src * mod * np.minimum(1.0, tt / 0.01) * np.exp(-tt / 0.08)
    return normalize(out, SFX_PEAK_DB)


def chime_phrase(sr, duration, root, bells, sparkle=True):
    """Composed short cue: a bell arpeggio over a low brass pad."""
    n = int(duration * sr)
    out = np.zeros(n)
    tt = t_axis(n, sr)
    # low pad
    for f, a in ((root / 2.0, 0.35), (root, 0.25), (root * 1.5, 0.15)):
        out += a * np.sin(2 * np.pi * f * tt) * exp_env(n, sr, 0.05, duration * 0.6)
    # bell notes
    step = duration / (len(bells) + 0.6)
    for i, f in enumerate(bells):
        start = int(i * step * sr)
        bn = int((duration - i * step) * sr)
        if bn <= 0:
            continue
        bt = t_axis(bn, sr)
        tone = (np.sin(2 * np.pi * f * bt) +
                0.45 * np.sin(2 * np.pi * f * 2.76 * bt) +
                0.25 * np.sin(2 * np.pi * f * 5.4 * bt))
        tone *= exp_decay(bn, sr, 0.35, 0.004)
        add(out, tone, start, 0.45)
    if sparkle:
        rng = np.random.default_rng(151)
        sh = rng.standard_normal(int(0.2 * sr))
        sh = fft_band(sh, sr, 3000.0, 8000.0) * exp_decay(len(sh), sr, 0.06)
        add(out, sh, int(duration * 0.05 * sr), 0.10)
    return normalize(np.tanh(out), SFX_PEAK_DB)


# ---------------------------------------------------------------------------
# music: original loops (A minor menu / D minor zone)
# ---------------------------------------------------------------------------
def bell(freq, dur, sr, decay=0.8, gain=1.0):
    n = int(dur * sr)
    tt = t_axis(n, sr)
    sig = (np.sin(2 * np.pi * freq * tt) * 1.0 +
           np.sin(2 * np.pi * freq * 2.76 * tt) * 0.45 +
           np.sin(2 * np.pi * freq * 5.40 * tt) * 0.22 +
           np.sin(2 * np.pi * freq * 8.93 * tt) * 0.10)
    sig *= exp_decay(n, sr, decay, 0.004)
    return sig * gain


def pluck(freq, dur, sr, decay=0.22, gain=1.0, seed=0):
    """Karplus-Strong-lite: decaying harmonics plus a short noise attack."""
    n = int(dur * sr)
    tt = t_axis(n, sr)
    rng = np.random.default_rng(seed)
    sig = np.zeros(n)
    for h in range(1, 6):
        sig += (0.55 ** (h - 1)) * np.sin(2 * np.pi * freq * h * tt +
                                            rng.uniform(0, 2 * np.pi))
    sig *= np.exp(-tt / decay)
    attack_n = int(0.006 * sr)
    attack = rng.standard_normal(attack_n) * np.exp(-t_axis(attack_n, sr) / 0.003)
    sig[:attack_n] += fft_band(attack, sr, 600.0, 5000.0) * 0.5
    return sig * gain


def wind(dur, sr, seed=0):
    """Slow wind bed: band-passed noise with gusts."""
    n = int(dur * sr)
    x = noise(n, sr, 120.0, 900.0, seed=seed)
    tt = t_axis(n, sr)
    gust = (0.55 + 0.25 * np.sin(2 * np.pi * 0.07 * tt + 0.4) +
            0.15 * np.sin(2 * np.pi * 0.031 * tt + 2.1) +
            0.08 * np.sin(2 * np.pi * 0.11 * tt + 4.0))
    return x * gust


def drone(freqs, dur, sr, lfo_hz=0.05):
    n = int(dur * sr)
    tt = t_axis(n, sr)
    out = np.zeros(n)
    for i, f in enumerate(freqs):
        det = 1.0 + (0.0025 if i % 2 else -0.0025)
        trem = 0.65 + 0.35 * np.sin(2 * np.pi * (lfo_hz * (i + 1)) * tt + i)
        out += (0.5 / (i + 1)) * np.sin(2 * np.pi * f * det * tt) * trem
    return out


def synth_menu_theme(sr=MUSIC_RATE, dur=24.0):
    n = int(dur * sr)
    out = np.zeros(n)
    tt = t_axis(n, sr)

    out += drone((55.0, 82.41, 110.0, 164.81), dur, sr) * 0.16
    out += wind(dur, sr, seed=7) * 0.14

    # sparse A-minor arpeggio, one pluck every 0.8 s
    notes = [220.0, 261.63, 329.63, 392.00, 440.0, 392.00, 329.63, 261.63,
             220.0, 196.00]
    k = 0
    t0 = 0.4
    while t0 < dur - 0.5:
        f = notes[k % len(notes)]
        vel = 0.45 + 0.10 * math.sin(k * 1.7)
        add(out, pluck(f, 1.4, sr, decay=0.35, seed=200 + k), t0 * sr, vel)
        # occasional octave shimmer
        if k % 5 == 2:
            add(out, pluck(f * 2.0, 0.9, sr, decay=0.25, seed=260 + k),
                t0 * sr, vel * 0.30)
        k += 1
        t0 += 0.8

    # distant knells
    for start, f in ((4.0, 110.0), (12.0, 146.83), (20.0, 98.0)):
        add(out, bell(f, 3.2, sr, decay=1.1, gain=0.5), start * sr, 0.22)

    # low moaning swell near the end
    swell = np.sin(2 * np.pi * 73.42 * tt) * exp_env(n, sr, 1.0, 6.0)
    add(out, swell, (dur - 7.0) * sr, 0.12)

    return fade(normalize(np.tanh(out * 1.1), MUSIC_PEAK_DB), sr, MUSIC_FADE_MS)


def synth_zone_theme(sr=MUSIC_RATE, dur=32.0):
    n = int(dur * sr)
    out = np.zeros(n)
    tt = t_axis(n, sr)

    out += drone((73.42, 110.0, 146.83, 220.0), dur, sr, lfo_hz=0.04) * 0.18
    out += wind(dur, sr, seed=11) * 0.10

    # heartbeat pulse every 0.8 s (two thumps)
    beat = 0.0
    while beat < dur - 0.3:
        add(out, sine_sweep(int(0.22 * sr), sr, 62.0, 38.0) *
            exp_decay(int(0.22 * sr), sr, 0.05), beat * sr, 0.30)
        add(out, sine_sweep(int(0.16 * sr), sr, 55.0, 34.0) *
            exp_decay(int(0.16 * sr), sr, 0.04), (beat + 0.24) * sr, 0.18)
        beat += 0.8

    # brooding two-note motif every 3.2 s (D minor: D, F, A, C)
    motif = [146.83, 174.61, 220.0, 196.0]
    k = 0
    t0 = 1.6
    while t0 < dur - 1.2:
        f = motif[k % len(motif)]
        add(out, pluck(f, 2.2, sr, decay=0.55, seed=300 + k), t0 * sr, 0.30)
        add(out, pluck(f * 1.5, 1.8, sr, decay=0.45, seed=340 + k),
            (t0 + 0.05) * sr, 0.12)
        k += 1
        t0 += 3.2

    # knells: D and A low bells
    for start, f in ((6.4, 73.42), (14.4, 110.0), (22.4, 73.42), (30.4, 110.0)):
        add(out, bell(f, 4.0, sr, decay=1.4, gain=0.5), start * sr, 0.16)

    # ominous rising fifth at the loop turnaround
    rise = sine_sweep(int(3.0 * sr), sr, 110.0, 165.0) * \
        exp_env(int(3.0 * sr), sr, 1.5, 1.2)
    add(out, rise, (dur - 3.5) * sr, 0.10)

    return fade(normalize(np.tanh(out * 1.15), MUSIC_PEAK_DB), sr, MUSIC_FADE_MS)


# ---------------------------------------------------------------------------
# asset table + main
# ---------------------------------------------------------------------------
def sfx_table(sr=SFX_RATE):
    """name -> generator (callable returning float array at sr)."""
    return [
        ("sfx_flintlock", lambda: flintlock(sr)),
        ("sfx_revolver_1", lambda: revolver_variant(sr, 0)),
        ("sfx_revolver_2", lambda: revolver_variant(sr, 1)),
        ("sfx_revolver_3", lambda: revolver_variant(sr, 2)),
        ("sfx_musket", lambda: musket(sr)),
        ("sfx_reload_flintlock", lambda: reload_flintlock(sr)),
        ("sfx_reload_revolver", lambda: reload_revolver(sr)),
        ("sfx_reload_musket", lambda: reload_musket(sr)),
        ("sfx_empty_click", lambda: empty_click(sr)),
        ("sfx_hit_thud", lambda: hit_thud(sr)),
        ("sfx_zombie_death", lambda: zombie_death(sr)),
        ("sfx_zombie_attack", lambda: zombie_attack(sr)),
        ("sfx_player_hurt", lambda: player_hurt(sr)),
        ("sfx_coin", lambda: coin_pickup(sr)),
        ("sfx_squelch", lambda: squelch(sr)),
        ("sfx_quest_complete", lambda: chime_phrase(
            sr, 1.0, 146.83, [440.0, 523.25, 587.33, 880.0])),
        ("sfx_zone_clear", lambda: chime_phrase(
            sr, 1.3, 110.0, [440.0, 523.25, 659.25, 880.0, 1046.5])),
        ("sfx_game_over", lambda: game_over_sting(sr)),
    ]


def game_over_sting(sr):
    """Falling minor motif with a long dying bell."""
    n = int(1.2 * sr)
    out = np.zeros(n)
    notes = [220.0, 196.0, 174.61, 130.81]
    for i, f in enumerate(notes):
        start = int(i * 0.22 * sr)
        bn = n - start
        tt = t_axis(bn, sr)
        tone = (np.sin(2 * np.pi * f * tt) + 0.4 * np.sin(4 * np.pi * f * tt))
        tone *= exp_decay(bn, sr, 0.28 + 0.1 * i, 0.006)
        add(out, tone, start, 0.5 - 0.08 * i)
    add(out, bell(65.41, 1.15, sr, decay=0.5, gain=0.5), 0.55 * sr, 0.35)
    return normalize(np.tanh(out * 1.1), SFX_PEAK_DB)


def main():
    sounds = []
    total = 0

    for name, gen in sfx_table():
        x = gen()
        x = trim_silence(x, SFX_RATE)
        x = fade(x, SFX_RATE, SFX_FADE_MS)
        if len(x) == 0:
            sys.exit("empty clip: %s" % name)
        path = os.path.join(OUT_DIR, name + ".bin")
        write_pcm16(path, x)
        total += os.path.getsize(path)
        sounds.append((name + ".bin", SFX_RATE, len(x), os.path.getsize(path)))

    for name, gen in (("music_menu", synth_menu_theme),
                      ("music_zone", synth_zone_theme)):
        x = gen()
        if len(x) == 0:
            sys.exit("empty music track: %s" % name)
        path = os.path.join(OUT_DIR, name + ".bin")
        write_pcm16(path, x)
        total += os.path.getsize(path)
        sounds.append((name + ".bin", MUSIC_RATE, len(x), os.path.getsize(path)))

    print("%-24s %-6s %9s %8s %8s" % ("file", "rate", "samples", "seconds", "bytes"))
    print("-" * 60)
    for name, rate, samples, size in sounds:
        print("%-24s %-6d %9d %8.3f %8d"
              % (name, rate, samples, samples / float(rate), size))
    print("-" * 60)
    print("total data/: %d bytes (%.2f MB), budget %d bytes (%.2f MB)"
          % (total, total / (1024.0 * 1024.0), BYTES_BUDGET,
             BYTES_BUDGET / (1024.0 * 1024.0)))
    if total > BYTES_BUDGET:
        sys.exit("ERROR: data/ exceeds the 2.5 MB budget")


if __name__ == "__main__":
    main()
