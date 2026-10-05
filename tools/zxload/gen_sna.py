#!/usr/bin/env python3
"""gen_sna.py out.sna -- a runnable 48K snapshot: random RAM, PC=$8000 holding
DI;HALT (so the emulator stops when the snapshot has been restored and resumed),
SP=$7F00, recognisable register values."""
import random
import struct
import sys

rnd = random.Random(4321)
mem = bytearray(rnd.randrange(256) for _ in range(0xC000))   # 0x4000..0xFFFF
code = 0x8000 - 0x4000
mem[code:code + 2] = b"\xf3\x76"
sp = 0x7F00
struct.pack_into("<H", mem, sp - 0x4000, 0x8000)              # PC on the stack (RETN)
h = bytearray(27)
h[0] = 0x3F
for off, v in ((1, 0x1111), (3, 0x2222), (5, 0x3333), (7, 0x4444), (9, 0x5555),
               (11, 0x6666), (13, 0x7777), (15, 0x8844), (17, sp), (19, 0x5C3A), (21, 0x9999)):
    struct.pack_into("<H", h, off, v)
h[23] = 0x04        # IFF2
h[24] = 0x22        # R
h[25] = 0x01        # IM 1
h[26] = 2           # border
open(sys.argv[1], "wb").write(bytes(h) + bytes(mem))
