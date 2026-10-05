#!/usr/bin/env python3
"""checksna.py file.sna dump.mem -- compare restored RAM with a 48K snapshot.
Reports differing ranges (the top of RAM is expected to hold the loader)."""
import sys

sna = open(sys.argv[1], "rb").read()[27:]
mem = open(sys.argv[2], "rb").read()
diffs = [a for a in range(0x4000, 0x10000) if mem[a] != sna[a - 0x4000]]
ranges, start, prev = [], None, None
for a in diffs:
    if start is None:
        start = prev = a
    elif a == prev + 1:
        prev = a
    else:
        ranges.append((start, prev)); start = prev = a
if start is not None:
    ranges.append((start, prev))
screen = sum(1 for a in diffs if a < 0x5B00)
body = sum(1 for a in diffs if 0x5B00 <= a < 0xFE00)
top = sum(1 for a in diffs if a >= 0xFE00)
print(f"screen={screen} body={body} top(FE00+)={top} ranges={['%04x-%04x' % r for r in ranges[:6]]}")
