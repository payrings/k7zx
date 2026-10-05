#!/usr/bin/env python3
"""Compare a memory dump from zxload against the CODE blocks of a .tap.

usage: check.py file.tap dump.mem   -> prints 'OK' or a mismatch summary
"""
import struct
import sys


def blocks(tap: bytes):
    p, hdr = 0, None
    while p + 2 <= len(tap):
        n = struct.unpack_from("<H", tap, p)[0]
        d = tap[p + 2:p + 2 + n]
        p += 2 + n
        if d[0] == 0:
            hdr = d
        elif hdr is not None:
            typ = hdr[1]
            start = struct.unpack_from("<H", hdr, 14)[0]
            yield typ, start, d[1:-1]
            hdr = None


def main():
    tap = open(sys.argv[1], "rb").read()
    mem = open(sys.argv[2], "rb").read()
    bad = []
    for typ, start, data in blocks(tap):
        if typ != 3:
            continue
        got = mem[start:start + len(data)]
        diff = sum(1 for a, b in zip(got, data) if a != b)
        if diff:
            first = next(i for i, (a, b) in enumerate(zip(got, data)) if a != b)
            bad.append(f"{start:#06x}+{len(data)}: {diff} bytes differ, first at +{first}")
    print("OK" if not bad else "; ".join(bad))


if __name__ == "__main__":
    main()
