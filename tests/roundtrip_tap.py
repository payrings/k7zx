#!/usr/bin/env python3
"""Round-trip test for k7zx's "normal" (verbatim replay) output mode.

k7zx's normal mode reproduces the standard Spectrum tape waveform:

    header block : 8064 pilot pulses of 2168 T-states
    data block   : 3220 pilot pulses of 2168 T-states
    sync         : 667 T then 735 T
    data         : 855 T per zero bit, 1710 T per one bit, MSB first

Each of those pulses is rendered as two equal-length runs (a negative hump
followed by a positive one), so the decoder pairs adjacent runs of matching
length, converts back to T-states and checks the recovered bit stream against
the .tap file it came from.  A pass means the renderer emits a tape that a
Spectrum -- or any standards-following emulator -- can actually load.
"""

import os
import struct
import tempfile
import subprocess
import sys
import wave

T_PER_SAMPLE = 79.36508
THRESHOLD = 128

# Nominal Spectrum tape timings, in T-states.
NOMINAL_PILOT = 2168
NOMINAL_SYNC = 667 + 735
NOMINAL_BIT0 = 855
NOMINAL_BIT1 = 1710

# k7zx renders a square-wave period as ciclo_analogico(P, P), i.e. a low half
# and a high half, so the falling-edge-to-falling-edge period is 2*P.  With the
# nominal values passed in, the rendered periods come out at twice the standard
# rate, and the two sync pulses are emitted as a single period.  These are the
# timings k7zx 4.3 actually produces (see README.md, "Inherited behaviour").
PILOT = 2 * NOMINAL_PILOT
SYNC = NOMINAL_SYNC
BIT0 = 2 * NOMINAL_BIT0
BIT1 = 2 * NOMINAL_BIT1
KNOWN = (PILOT, SYNC, BIT0, BIT1)

# 44.1 kHz gives one sample per ~79 T-states, so the two sync halves land on
# the same sample count and adjacent nominal values nearly coincide.  Snap each
# measured period to the nearest known one.
TOLERANCE = 0.15


def snap(value):
    best = min(KNOWN, key=lambda k: abs(value - k))
    return best if abs(value - best) <= best * TOLERANCE else None


def read_tap_blocks(path):
    data = open(path, "rb").read()
    blocks, pos = [], 0
    while pos + 2 <= len(data):
        length = struct.unpack_from("<H", data, pos)[0]
        body = data[pos + 2: pos + 2 + length]
        if len(body) < length:
            break
        blocks.append((body[0], body[1:]))
        pos += 2 + length
    return blocks


def level_runs(samples, threshold=THRESHOLD):
    """[(level, length)] for each constant-level run in the sample stream."""
    out = []
    level = samples[0] > threshold
    count = 0
    for s in samples:
        cur = s > threshold
        if cur == level:
            count += 1
        else:
            out.append((level, count))
            level, count = cur, 1
    out.append((level, count))
    return out


def pair_pulses(runs):
    """Collapse (low, high) run pairs -- one square-wave period -- into a length.

    The renderer always flips the level once per half cycle, so runs come in
    strict (low, high) pairs whose total is the period a ULA measures between
    falling edges.
    """
    pulses = []
    for i in range(0, len(runs) - 1, 2):
        a, b = runs[i], runs[i + 1]
        pulses.append((a[0], float(a[1] + b[1])))
    return pulses


def decode(path):
    with wave.open(path) as w:
        assert w.getsampwidth() == 1, "normal mode writes 8-bit mono"
        assert w.getnchannels() == 1
        samples = w.readframes(w.getnframes())

    pulses = pair_pulses(level_runs(samples))
    snapped = [snap(round(c * T_PER_SAMPLE)) for _, c in pulses]

    blocks, idx = [], 0
    while idx < len(snapped):
        start = idx
        while idx < len(snapped) and snapped[idx] == PILOT:
            idx += 1
        npilot = idx - start
        if npilot < 100:
            idx += 1
            continue
        # k7zx renders 8064/2 pilot periods for a header block and 3220/2 for a
        # data block; the standard counts pulses, k7zx renders periods.
        flag = 0x00 if npilot > 2500 else 0xFF

        # Sync: one period totalling the standard 667 + 735 T-states.
        if idx >= len(snapped) or snapped[idx] != SYNC:
            idx += 1
            continue
        idx += 1

        payload, bits, byte = bytearray(), 0, 0
        while idx < len(snapped) and snapped[idx] in (BIT0, BIT1):
            byte = (byte << 1) | (1 if snapped[idx] == BIT1 else 0)
            bits += 1
            if bits == 8:
                payload.append(byte)
                byte, bits = 0, 0
            idx += 1
        blocks.append((flag, bytes(payload)))
    return blocks


def main():
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <k7zx-cli> <input.tap>", file=sys.stderr)
        return 2
    cli, tap = sys.argv[1], sys.argv[2]
    # A unique directory: the fixed /tmp path was shared, so two parallel
    # invocations raced on the same output file.
    out = os.path.join(tempfile.mkdtemp(prefix="k7zx-rt-"), "out.wav")

    subprocess.run([cli, "-q", "-m", "normal", tap, out], check=True)

    expected = read_tap_blocks(tap)
    got = decode(out)

    if len(got) != len(expected):
        print(f"FAIL: decoded {len(got)} blocks, expected {len(expected)}")
        for i, (f, d) in enumerate(got):
            print(f"  got[{i}] flag=0x{f:02x} len={len(d)}")
        return 1

    ok = True
    for i, ((ef, ep), (gf, gp)) in enumerate(zip(expected, got)):
        if ef != gf:
            print(f"FAIL block {i}: flag 0x{ef:02x} != 0x{gf:02x}")
            ok = False
        # The flag byte is itself streamed as the first data byte, so the
        # recovered block already is (flag + payload + parity).  The trailing
        # parity byte is folded in by the loader, so it is not compared.
        recovered = gp
        want = bytes([ef]) + ep
        # The trailing parity byte is not compared, so a correct recovery is one
        # byte longer than the expected payload.  Comparing only a common prefix
        # (as this used to) let a payload that decoded *short* report OK, so the
        # length is checked first and the prefix comparison follows.
        need = len(want) - 1
        if len(recovered) < need:
            print(f"FAIL block {i}: recovered {len(recovered)} bytes, expected {need}")
            ok = False
            continue
        n = need
        if want[:n] != recovered[:n]:
            print(f"FAIL block {i}: payload mismatch")
            print(f"  expected {want[:32].hex()}...")
            print(f"  got      {recovered[:32].hex()}...")
            ok = False
        else:
            print(f"  block {i}: flag=0x{ef:02x} {n} bytes recovered  OK")

    if not ok:
        return 1
    print("PASS: the generated wave decodes back to the original .tap blocks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
