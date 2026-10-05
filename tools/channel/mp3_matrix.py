#!/usr/bin/env python3
"""Re-measure the guide's lossy-codec (MP3) table.

Every technique is converted at 44.1 kHz, encoded at each bitrate, decoded again
and loaded on an emulated 48K -- twice, once from its own edges and once through
the "clean line" model of real hardware.  The symbols are:

  clean  loads with no wrong byte at all, both ways
  ok     loads both ways, but with wrong bytes in at least one
  edges  loads only from its own edges
  no     does not load at least one way

    tools/channel/mp3_matrix.py [--markdown]

`--markdown` prints the table in the form GUIDE.md publishes it, so the document
can be regenerated rather than hand-edited.  Needs `lame`.

There is no AAC column because k7zx has no AAC output and never had: the only
lossy format it writes is MP3.  The figure is still worth knowing if you encode
the WAV yourself for streaming or Bluetooth.
"""
import os, subprocess, sys, tempfile
sys.path.insert(0, 'tools/channel')
sys.path.insert(0, 'tools/zxload')
import channel                      # the same model the guide describes
from robustness import wrong_bytes

CLI  = 'build/k7zx-cli'
ZX   = 'build/zxload/zxload'
TAPE = 'tests/data/golden.tap'
TMP  = os.path.join(tempfile.gettempdir(), 'k7zx-mp3v')
os.makedirs(TMP, exist_ok=True)

ROWS = [("FSK 8.00", "fsk", "8.00"), ("FSK 5.00", "fsk", "5.00"),
        ("FSK 4.00", "fsk", "4.00"), ("Fi 6.00", "fi", "6.00"),
        ("Manchester 4.00", "manchester", "4.00"), ("Shavings Slow 4.00", "slow", "4.00"),
        ("Shavings Delta 3.50", "delta", "3.50"), ("Shavings Raudo 2.75", "raudo", "2.75"),
        ("Rayo 2.75", "rayo", "2.75"), ("NPU 1.25", "npu", "1.25")]
MP3 = [("MP3 320", ['lame','--quiet','-b','320','-m','j']), ("MP3 192", ['lame','--quiet','-b','192','-m','j']),
       ("MP3 128", ['lame','--quiet','-b','128','-m','j']), ("MP3 96",  ['lame','--quiet','-b','96','-m','j'])]

def run(cmd, **kw):
    return subprocess.run(cmd, capture_output=True, text=True, **kw)

def load(path):
    """Return (loads with no wrong byte from its own edges, same via the model)."""
    out = []
    for ch in (None, dict(offset=0.05)):
        src = path
        if ch is not None:
            rate, x = channel.load_wav(path)
            t, l0, _, _ = channel.edges(x, rate, **ch)
            e = os.path.join(TMP, 'c.edge'); channel.write_edges(e, t, l0); src = e
        mem = os.path.join(TMP, 'c.mem')
        if os.path.exists(mem): os.remove(mem)
        r = subprocess.run([ZX, src, mem, '--extra-seconds', '2', '--contention'],
                           capture_output=True, text=True)
        out.append(r.stdout.startswith("result=HALT") and wrong_bytes(TAPE, mem) == 0)
    return out

print(f"{'row':22s} {'WAV':5s} " + " ".join(f"{n.split()[1]:>5s}" for n, _ in MP3))
results = {}
for label, method, spb in ROWS:
    wav = os.path.join(TMP, 'a.wav')
    if run([CLI,'-q','-m','hispeed','-t',method,'-s',spb,'-c','one','-r','44100',TAPE,wav]).returncode:
        print(f"{label:22s} CONVERSION FAILED"); continue
    cells = []
    perfect, modelled = load(wav)
    cells.append(('perfect', 'clean', perfect and modelled))
    for name, cmd in MP3:
        enc = os.path.join(TMP, 'a.mp3')
        enc_rc = subprocess.run(cmd + [wav, enc], capture_output=True)
        if enc_rc.returncode or not os.path.exists(enc):
            print(f"{label:22s} {name}: the encoder failed")
            cells.append((name, 'error', None))
            continue
        dec = os.path.join(TMP, 'd.wav')
        if os.path.exists(dec):
            os.remove(dec)
        d = subprocess.run(['lame', '--quiet', '--decode', enc, dec], capture_output=True)
        if d.returncode or not os.path.exists(dec):
            print(f"{label:22s} {name}: could not decode what the encoder wrote")
            cells.append((name, 'error', None))
            continue
        p2, m2 = load(dec)
        cells.append((name,'perfect',p2))
        cells.append((name,'modelled',m2))
    results[label] = cells
    def mark(b): return {True:'OK', False:'--', None:'??'}[b]
    print(f"{label:22s} {mark(cells[0][2] and cells[0][2]):5s} " +
          " ".join(f"{mark(any(c[2] for c in cells[1:] if c[0]==n and c[1]=='perfect')):>5s}"
                   for n,_ in MP3))
import json
json.dump({k: [(a, b, c) for a, b, c in v] for k, v in results.items()},
          open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'mp3v.json'), 'w'), indent=1)

if __name__ == '__main__' and "--markdown" in sys.argv:
    print("\n| Technique | WAV | MP3 320 | MP3 192 | MP3 128 | MP3 96 |")
    print("|---|---|---|---|---|---|")
    def mark(p, m):
        if p and m: return "✅"
        if p or m: return "⚠️"
        return "❌"
    for label, method, spb in ROWS:
        cells = results.get(label)
        if cells is None:
            continue
        row = [mark(cells[0][2], cells[0][2])]
        for col, _ in MP3:
            p = any(x[0] == col and x[1] == 'perfect' and x[2] for x in cells)
            m = any(x[0] == col and x[1] == 'modelled' and x[2] for x in cells)
            row.append(mark(p, m))
        print(f"| **{label}** | " + " | ".join(row) + " |")
