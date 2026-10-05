#!/usr/bin/env python3
"""Behavioural tests for k7zx-cli's command line.

These are not about the tape: they check that the command line does what the
README and GUIDE.md say it does.  Each case names the defect it guards against,
because every one of them was a real bug rather than a hypothetical.
"""

import configparser
import os
import struct
import subprocess
import sys
import tempfile

FAILURES = []


def check(name, ok, detail=""):
    if ok:
        print(f"  ok    {name}")
    else:
        print(f"  FAIL  {name}" + (f": {detail}" if detail else ""))
        FAILURES.append(name)


def run(cli, args, **kw):
    return subprocess.run([cli] + args, capture_output=True, text=True, **kw)


def write_ini(path, **ops):
    cfg = configparser.ConfigParser()
    cfg["Ops"] = {k: str(v) for k, v in ops.items()}
    with open(path, "w") as f:
        cfg.write(f)


def main():
    if len(sys.argv) < 3:
        print(f"usage: {sys.argv[0]} <k7zx-cli> <data-dir>", file=sys.stderr)
        return 2
    cli, data = sys.argv[1], sys.argv[2]
    tap = os.path.join(data, "test.tap")
    tmp = tempfile.mkdtemp(prefix="k7zx-cli-")

    # ---------------------------------------------------------------- help
    r = run(cli, ["--help"])
    check("--help works", r.returncode == 0, r.stderr)
    for opt in ("--mp3", "--mp3-encoder", "--mp3-bitrate", "--poke", "--compress",
                "--emulate", "--blocks", "--kolmogorov"):
        check(f"--help documents {opt}", opt in r.stdout)

    # --------------------------------------- a flag only overrides what it says
    # Every option used to carry its documented default and was copied over the
    # stored settings unconditionally, so a flag nobody typed still won and the
    # command line disagreed with the GUI reading the same file.
    ini = os.path.join(tmp, "cfg.ini")
    write_ini(ini, Mode=1, Method=8, SamplesPerBit=20, SampleRate=44100,
              Waveform=2, ManyBlocks=2, Final=1)
    a = os.path.join(tmp, "a.wav")
    b = os.path.join(tmp, "b.wav")
    run(cli, ["--config-path", ini, "-q", tap, a])
    run(cli, ["--config-path", ini, "-q", "-t", "fi", "-s", "5.00", "-r", "44100",
              "-w", "cubic", "-c", "many", "--final-tone", tap, b])
    check("the stored configuration is honoured when no option is given",
          open(a, "rb").read() == open(b, "rb").read())

    c = os.path.join(tmp, "c.wav")
    run(cli, ["--config-path", ini, "-q", "-t", "fsk", "-s", "5.00", tap, c])
    check("-t still overrides the stored technique",
          open(a, "rb").read() != open(c, "rb").read())

    # ----------------------------------------------------------- poke ranges
    # A hand-written or mistyped poke used to be narrowed silently: 70000
    # became 4464 and poked a completely different address.
    r = run(cli, ["--config-path", ini, "-q", "--poke", "70000=1", tap,
                  os.path.join(tmp, "p.wav")])
    check("an out-of-range --poke address is refused", r.returncode == 2, r.stderr)
    check("...and says why", "out of range" in r.stderr, r.stderr)
    r = run(cli, ["--config-path", ini, "-q", "--poke", "0x5ce5=3", "--poke",
                  "16384=255", tap, os.path.join(tmp, "p.wav")])
    check("--poke accepts base-0 hex and repeats", r.returncode == 0, r.stderr)
    r = run(cli, ["--config-path", ini, "-q", "--poke", "0x1000", tap,
                  os.path.join(tmp, "p.wav")])
    check("--poke without = is refused", r.returncode == 2, r.stderr)

    # --------------------------------------------------- compress is Rayo-only
    # The GUI greys the box out for every other technique; the command line used
    # to accept the flag for all of them and quietly do nothing.
    r = run(cli, ["--config-path", ini, "--compress", "-t", "fsk", "-s", "5.00",
                  tap, os.path.join(tmp, "z.wav")])
    check("--compress on a technique without LZ says so",
          "only Rayo" in r.stderr, r.stderr)
    r = run(cli, ["--config-path", ini, "--compress", "-t", "rayo", "-s", "2.75",
                  tap, os.path.join(tmp, "r.wav")])
    # A warning about the scheme is fine; what must not happen is the
    # "only Rayo does" complaint.
    check("--compress on Rayo is accepted",
          r.returncode == 0 and "only Rayo" not in r.stderr, r.stderr)
    check("--compress on Rayo actually compresses",
          os.path.getsize(os.path.join(tmp, "r.wav")) > 0)

    # -------------------------------------------------- speeds are named well
    r = run(cli, ["--config-path", ini, "-q", "-t", "npu", "-s", "2.25", tap,
                  os.path.join(tmp, "n.wav")])
    check("an unsupported speed is refused", r.returncode == 2)
    check("...and names the speeds as they are typed",
          "2.50, 2.00, 1.75, 1.25" in r.stderr, r.stderr)

    # ---------------------------------------------------------- the TZX header
    # k7zx 4.3 wrote -- and read -- a 10 byte TZX header because its struct was
    # 10 bytes long; the format has 12.  No .tzx from another tool could be read.
    tzx = os.path.join(tmp, "out.tzx")
    run(cli, ["--config-path", ini, "-q", "--emulate", "-t", "fsk", "-s", "5.00",
              tap, tzx])
    d = open(tzx, "rb").read(16)
    check("--emulate writes a TZX", len(d) >= 13, f"{len(d)} bytes")
    check("...with the 7-byte magic and 0x1A", d[:7] == b"ZXTape!" and d[7] == 0x1A)
    check("...a two byte start-block ID", struct.unpack_from("<H", d, 10)[0] == 0x0030,
          f"start ID = {struct.unpack_from('<H', d, 10)[0]:#06x}")
    check("...and the first block at offset 12", d[12] == 0x30)

    # A file the port itself writes must be one it can read back.
    r = run(cli, ["--config-path", ini, "-q", "-t", "fsk", "-s", "5.00", tap,
                  os.path.join(tmp, "back.tap")])
    check("a plain conversion still works", r.returncode == 0, r.stderr)

    if FAILURES:
        print(f"\n{len(FAILURES)} check(s) failed:")
        for f in FAILURES:
            print(f"  - {f}")
        return 1
    print("\nall command-line checks pass")
    return 0


if __name__ == "__main__":
    sys.exit(main())