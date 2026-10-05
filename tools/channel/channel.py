#!/usr/bin/env python3
"""Analog playback model: WAV -> edge times as a real Spectrum's EAR input sees them.

Chain: DAC (band-limited interpolation, x32) -> output low-pass (Butterworth,
order 4, fc) -> AC coupling (1st-order high-pass, fhp) -> optional speed error
-> additive noise -> comparator with threshold offset (fraction of peak) and
hysteresis.  Returns crossing times in T-states (3.5 MHz) and the initial level.
"""
import sys, struct
import numpy as np
from scipy import signal

T_PER_S = 3500000.0

def load_wav(path):
    b = open(path, 'rb').read()
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid = b[pos:pos+4]; n = struct.unpack_from('<I', b, pos+4)[0]
        if cid == b'fmt ': fmt = struct.unpack_from('<HHIIHH', b, pos+8)
        elif cid == b'data': data = b[pos+8:pos+8+n]
        pos += 8 + n + (n & 1)
    ch, rate, bits = fmt[1], fmt[2], fmt[5]
    if bits == 8:
        x = (np.frombuffer(data, np.uint8).astype(np.float64) - 128) / 128
    else:
        x = np.frombuffer(data[:len(data)//2*2], '<i2').astype(np.float64) / 32768
    x = x[::ch]
    return rate, x

def edges(x, rate, fc=20000.0, order=4, fhp=20.0, offset=0.0, hyst=0.0,
          noise=0.0, speed=1.0, up=32, seed=1):
    y = signal.resample_poly(x, up, 1, window=('kaiser', 8.0))
    fs = rate * up
    if fc:
        sos = signal.butter(order, fc, 'low', fs=fs, output='sos')
        y = signal.sosfilt(sos, y)
    if fhp:
        sos = signal.butter(1, fhp, 'high', fs=fs, output='sos')
        y = signal.sosfilt(sos, y)
    if noise:
        y = y + np.random.default_rng(seed).normal(0, noise, len(y))
    peak = np.percentile(np.abs(y), 99.5)
    th_hi = (offset + hyst / 2) * peak
    th_lo = (offset - hyst / 2) * peak
    # comparator with hysteresis, vectorised: the state is the most recent
    # threshold event (above th_hi -> 1, below th_lo -> 0)
    ev_idx = np.flatnonzero((y > th_hi) | (y < th_lo))
    ev_val = (y[ev_idx] > th_hi).astype(np.int8)
    ch = np.flatnonzero(ev_val[1:] != ev_val[:-1]) + 1
    js = ev_idx[ch]
    th = np.where(ev_val[ch] == 1, th_hi, th_lo)
    y0, y1 = y[js - 1], y[js]
    frac = np.where(y1 != y0, (th - y0) / np.where(y1 != y0, y1 - y0, 1), 0.0)
    out = (js - 1 + frac) / fs
    t = np.array(out) * T_PER_S / speed
    lvl0 = int(ev_val[0]) if len(ev_val) else 0
    return t, lvl0, y, fs

def write_edges(path, t, lvl0):
    t = np.round(t).astype(np.uint64)
    with open(path, 'wb') as f:
        f.write(b'EDGE'); f.write(struct.pack('<IQ', lvl0, len(t))); f.write(t.tobytes())

if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('wav'); ap.add_argument('out')
    ap.add_argument('--fc', type=float, default=20000); ap.add_argument('--order', type=int, default=4)
    ap.add_argument('--fhp', type=float, default=20); ap.add_argument('--offset', type=float, default=0)
    ap.add_argument('--hyst', type=float, default=0); ap.add_argument('--noise', type=float, default=0)
    ap.add_argument('--speed', type=float, default=1.0); ap.add_argument('--seed', type=int, default=1)
    a = ap.parse_args()
    rate, x = load_wav(a.wav)
    t, l0, _, _ = edges(x, rate, a.fc, a.order, a.fhp, a.offset, a.hyst, a.noise, a.speed, seed=a.seed)
    write_edges(a.out, t, l0)
    if len(t):
        print(f'{len(t)} edges, {t[-1]/T_PER_S:.2f} s')
    else:
        # A silent or all-zero input yields no edges at all, and indexing the
        # empty array here aborted the run *after* the .edge file was written.
        print('no edges: the input carries no signal')
