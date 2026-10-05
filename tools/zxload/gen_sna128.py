#!/usr/bin/env python3
"""gen_sna128.py out.sna [7ffd] -- runnable 128K .SNA: random contents in all
eight banks, PC=$8000 (bank 2) holding DI;HALT, recognisable registers."""
import random
import struct
import sys

p7ffd = int(sys.argv[2], 0) if len(sys.argv) > 2 else 0x13
rnd = random.Random(8765)
banks = [bytearray(rnd.randrange(256) for _ in range(0x4000)) for _ in range(8)]
banks[2][0:2] = b"\xf3\x76"
h = bytearray(27)
h[0] = 0x3F
for off, v in ((1, 0x1111), (3, 0x2222), (5, 0x3333), (7, 0x4444), (9, 0x5555),
               (11, 0x6666), (13, 0x7777), (15, 0x8844), (17, 0x7F00), (19, 0x5C3A), (21, 0x9999)):
    struct.pack_into("<H", h, off, v)
h[23], h[24], h[25], h[26] = 0x04, 0x22, 0x01, 2
cur = p7ffd & 7
out = bytes(h) + banks[5] + banks[2] + banks[cur] + struct.pack("<HBB", 0x8000, p7ffd, 0)
for b in range(8):
    if b not in (5, 2, cur):
        out += banks[b]
open(sys.argv[1], "wb").write(out)
open(sys.argv[1] + ".banks", "wb").write(b"".join(banks))
