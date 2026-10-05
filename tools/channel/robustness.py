#!/usr/bin/env python3
"""robustness.py K7ZX_CLI ZXLOAD TAPE [technique:speed ...]

Put each technique's WAV through a model of a real playback chain
(channel.py: DAC filter, AC coupling, the EAR input's threshold offset, noise,
speed error) and load it in zxload, on 48K timing with contention and on 128K
timing.  Prints, per condition, the number of bytes that came out wrong
(0 = loaded perfectly; '-' = never finished).  The TAPE's CODE blocks must end
in DI; HALT -- tests/data/golden.tap does.
"""
import atexit, os, shutil, subprocess, sys, tempfile
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, '..', 'zxload'))
from channel import load_wav, edges, write_edges

CONDITIONS = [
    ('exact', None),
    ('offset 0.2', dict(offset=0.2)),
    ('offset 0.4', dict(offset=0.4)),
    ('inverted', dict(offset=-0.2)),
    ('16 kHz', dict(fc=16000, offset=0.2)),
    ('12 kHz', dict(fc=12000, offset=0.2)),
    ('2% fast', dict(speed=1.02, offset=0.2)),
    ('2% slow', dict(speed=0.98, offset=0.2)),
    ('noise 5%', dict(noise=0.05, offset=0.2)),
    ('AC 200 Hz', dict(fhp=200, offset=0.2)),
]

def wrong_bytes(tape, mem):
    import struct
    t = open(tape, 'rb').read(); m = open(mem, 'rb').read()
    p, hdr, bad = 0, None, 0
    while p + 2 <= len(t):
        n = struct.unpack_from('<H', t, p)[0]; d = t[p + 2:p + 2 + n]; p += 2 + n
        if d[0] == 0: hdr = d; continue
        if hdr is not None and hdr[1] == 3:
            a = struct.unpack_from('<H', hdr, 14)[0]
            for i, b in enumerate(d[1:-1]):
                if a + i >= len(m):
                    # The header claimed an address the dump does not cover;
                    # count the bytes as wrong rather than raising.
                    bad += 1
                elif m[a + i] != b:
                    bad += 1
        hdr = None
    return bad

def main():
    cli, zx, tape = sys.argv[1:4]
    techs = sys.argv[4:] or ['raudo:2.25', 'rayo:2.25', 'raudo:2.75', 'rayo:2.75']
    tmp = tempfile.mkdtemp()
    atexit.register(shutil.rmtree, tmp, True)   # every run used to leave one behind
    env = dict(os.environ, XDG_CONFIG_HOME=os.path.join(tmp, 'cfg'))
    cols = [(t, m) for t in techs for m in ('48K', '128K')]
    headings = [f'{t}/{m}' for t, m in cols]
    # Size every field to its contents plus a gap.  A fixed width used to be one
    # character narrower than the longest heading, so those columns butted
    # together with no separator: "rayo:2.75/48Krayo:2.75/128Kfsk:8.00/48K".
    # That is unreadable, and it has already been misread as a change of result
    # between runs.  Nothing here is variable, so the same width is used for the
    # header and every row, and a rule is printed under it.
    roww = max(len('condition'), *(len(h) for h in headings)) + 3
    cellw = max(len(h) for h in headings) + 3
    print('condition'.ljust(roww) + ''.join(h.ljust(cellw) for h in headings))
    print('-' * (roww + cellw * len(headings)))
    clean = [0] * len(cols)
    for name, ch in CONDITIONS:
        row = []
        for tech, machine in cols:
            method, spb = tech.split(':')
            wav, edg, mem = (os.path.join(tmp, f) for f in ('t.wav', 't.edge', 't.mem'))
            subprocess.run([cli, '-q', '-m', 'hispeed', '-t', method, '-s', spb, '-c', 'one', '-w', 'square',
                            tape, wav], check=True, env=env)
            src = wav
            if ch is not None:
                rate, x = load_wav(wav)
                edge_times, l0, _, _ = edges(x, rate, **ch)
                write_edges(edg, edge_times, l0)
                src = edg
            # zxload leaves the previous column's .mem behind if it does not
            # halt, which would let a later column read a stale image.  Remove it
            # so a '-' always means "did not finish" and never "read someone
            # else's memory".
            if os.path.exists(mem):
                os.remove(mem)
            cmd = [zx, src, mem, '--extra-seconds', '2', '--contention' if machine == '48K' else '--128']
            r = subprocess.run(cmd, capture_output=True, text=True)
            # zxload puts its banner and register dump on stderr and only the
            # verdict on stdout, so a prefix match on stdout is right; be explicit
            # about it anyway, since the first line of stdout is the contract.
            if r.stdout.startswith('result=HALT'):
                row.append(str(wrong_bytes(tape, mem)))
            else:
                if not r.stdout.startswith('result='):
                    print(f'robustness: unexpected zxload output: {r.stdout.splitlines()[:1]}',
                          file=sys.stderr)
                row.append('-')
        print(name.ljust(roww) + ''.join(v.ljust(cellw) for v in row), flush=True)
        for i, v in enumerate(row):
            clean[i] += (v == '0')
    # A tally per column, because that -- not any single cell -- is what the
    # ratings quote, and reading one cell out of a wide table by eye is how they
    # get misread.
    print()
    print('clean conditions per column (out of %d):' % len(CONDITIONS))
    print('condition'.ljust(roww) + ''.join(h.ljust(cellw) for h in headings))
    print('-' * (roww + cellw * len(headings)))
    print('clean'.ljust(roww) +
          ''.join(('all' if clean[i] == len(CONDITIONS) else str(clean[i])).ljust(cellw)
                  for i in range(len(cols))))

if __name__ == '__main__':
    main()
