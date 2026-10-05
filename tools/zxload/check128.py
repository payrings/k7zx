#!/usr/bin/env python3
"""check128.py expected.banks dump.banks -- differing bytes per 16K bank."""
import sys

a = open(sys.argv[1], "rb").read()
b = open(sys.argv[2], "rb").read()
out = []
for bank in range(8):
    x, y = a[bank * 0x4000:(bank + 1) * 0x4000], b[bank * 0x4000:(bank + 1) * 0x4000]
    d = [i for i in range(0x4000) if x[i] != y[i]]
    if d:
        out.append(f"b{bank}:{len(d)}@{d[0]:04x}-{d[-1]:04x}")
print("banks OK" if not out else " ".join(out))
