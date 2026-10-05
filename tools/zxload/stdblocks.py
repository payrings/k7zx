#!/usr/bin/env python3
"""stdblocks.py a.wav [b.wav] -- decode the standard-speed blocks (the header
and the BASIC loader) from k7zx WAVs; with two files, diff them byte by byte."""
import sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from wavcmp import load, runs  # noqa: E402


def decode(path):
    rate, _, _, s = load(path)
    r = runs(s)
    tps = 3500000 / rate
    per = [(r[i] + r[i + 1]) * tps for i in range(0, len(r) - 1, 2)]
    near = lambda v, t: abs(v - t) < t * 0.2
    blocks, i = [], 0
    while i < len(per):
        while i < len(per) and not near(per[i], 4336):
            i += 1
        j = i
        while j < len(per) and near(per[j], 4336):
            j += 1
        if j - i < 200:
            if j >= len(per):
                break
            i = j + 1
            continue
        k = j
        while k < len(per) and not near(per[k], 1402):
            k += 1
        k += 1
        acc, bits, out = 0, 0, []
        while k < len(per):
            if near(per[k], 1710):
                pass
            elif near(per[k], 3420):
                acc |= 0x80 >> bits
            else:
                break
            bits += 1
            if bits == 8:
                out.append(acc)
                acc, bits = 0, 0
            k += 1
        blocks.append(bytes(out))
        i = k
    return blocks


def main():
    a = decode(sys.argv[1])
    if len(sys.argv) == 2:
        for b in a:
            print(len(b), b.hex())
        return
    b = decode(sys.argv[2])
    print(f"blocks: {len(a)} vs {len(b)}")
    for n, (x, y) in enumerate(zip(a, b)):
        if x == y:
            print(f"block {n}: identical ({len(x)} bytes)")
            continue
        print(f"block {n}: {len(x)} vs {len(y)} bytes")
        diffs = [i for i in range(min(len(x), len(y))) if x[i] != y[i]]
        for i in diffs[:40]:
            print(f"   +{i:3d} (line data +{i - 5:3d}): {x[i]:02x} vs {y[i]:02x}")
        if len(diffs) > 40:
            print(f"   ... {len(diffs)} differing bytes")


if __name__ == "__main__":
    main()
