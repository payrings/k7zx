#!/usr/bin/env python3
"""wavcmp.py a.wav b.wav -- compare two WAVs as sequences of level runs.

Prints IDENTICAL (same samples), SAME-RUNS (same edge timing, different sample
values e.g. amplitude), or the first run index where edge timing differs, with
context."""
import struct
import sys


def load(p):
    b = open(p, "rb").read()
    pos, fmt, data = 12, None, None
    while pos + 8 <= len(b):
        cid, n = b[pos:pos + 4], struct.unpack_from("<I", b, pos + 4)[0]
        if cid == b"fmt ":
            fmt = struct.unpack_from("<HHIIHH", b, pos + 8)
        elif cid == b"data":
            data = b[pos + 8:pos + 8 + n]
        pos += 8 + n + (n & 1)
    ch, rate, bits = fmt[1], fmt[2], fmt[5]
    step = bits // 8 * ch
    if bits == 8:
        s = [data[i] - 128 for i in range(0, len(data) - step + 1, step)]
    else:
        s = [struct.unpack_from("<h", data, i)[0] for i in range(0, len(data) - step + 1, step)]
    return rate, bits, ch, s


def runs(s):
    out, cur, n = [], s[0] > 0, 0
    for v in s:
        lv = v > 0
        if lv == cur:
            n += 1
        else:
            out.append(n)
            cur, n = lv, 1
    out.append(n)
    return out


def main():
    ra, ba, ca, a = load(sys.argv[1])
    rb, bb, cb, b = load(sys.argv[2])
    if (ra, ba, ca) != (rb, bb, cb):
        print(f"FORMAT {ra},{ba},{ca} vs {rb},{bb},{cb}")
    if a == b:
        print("IDENTICAL")
        return
    xa, xb = runs(a), runs(b)
    if xa == xb:
        print(f"SAME-RUNS (samples differ; {len(a)} vs {len(b)} samples)")
        return
    i = next((k for k in range(min(len(xa), len(xb))) if xa[k] != xb[k]), min(len(xa), len(xb)))
    t = sum(xa[:i]) / ra
    print(f"DIFF at run {i} (t={t:.4f}s, {len(xa)} vs {len(xb)} runs)")
    print("  a:", xa[max(0, i - 6):i + 14])
    print("  b:", xb[max(0, i - 6):i + 14])


if __name__ == "__main__":
    main()
