#!/usr/bin/env python3
"""Check that every finding this project publishes still agrees with the code.

The guide's ratings, bit rates and counts are conclusions drawn from
measurements. A conclusion is only worth anything while it is still true, and a
document drifts silently: a speed list gains an entry, a bit rate formula
changes, a test is added, and the sentence that was true last month quietly
becomes false. This script is the guard. It re-derives the published facts from
the source and fails if any of them has moved.

    tools/check_findings.py [repo-root]

Exits 0 if every finding still holds, 1 otherwise.
"""
import json
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else
                       os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

failures = []


def check(what, ok, detail=""):
    if not ok:
        failures.append(f"{what}{(': ' + detail) if detail else ''}")
    print(f"  {'ok  ' if ok else 'FAIL'}  {what}" + (f"  [{detail}]" if detail and not ok else ""))


def read(rel):
    with open(os.path.join(ROOT, rel), encoding="utf-8") as f:
        return f.read()


def main():
    guide = read("GUIDE.md")
    readme = read("README.md")
    settings_h = read("src/core/settings.h")
    settings_cpp = read("src/core/settings.cpp")

    # ---- the four techniques' ratings: mark and notes come from the stored
    #      measurement, so re-derive them rather than trusting the table ----
    snapshot = os.path.join(ROOT, "tools", "channel", "ratings.json")
    rates = {}
    if os.path.exists(snapshot):
        try:
            rates = json.load(open(snapshot))
        except ValueError as e:
            print(f"  FAIL  ratings.json is not valid JSON: {e}")
            failures.append("tools/channel/ratings.json is not valid JSON")
        if not rates:
            failures.append("tools/channel/ratings.json is empty")
    if rates:
        conds = list(next(iter(rates.values())).keys())
        per = {}
        for col, d in rates.items():
            name, mach = col.rsplit("/", 1)
            per.setdefault(name, {})[mach] = d

        def derived(key):
            c48 = [c for c in conds if per[key]["48K"][c] == "0"]
            c128 = [c for c in conds if per[key]["128K"][c] == "0"]
            f48 = [c for c in conds if per[key]["48K"][c] == "-"]
            f128 = [c for c in conds if per[key]["128K"][c] == "-"]
            e48 = [c for c in conds if per[key]["48K"][c] not in ("0", "-")]
            e128 = [c for c in conds if per[key]["128K"][c] not in ("0", "-")]
            emu = "🟢" if "exact" in c48 and "exact" in c128 else "🔴"
            n = min(len(c48), len(c128))
            real = "🟢" if n == 10 else ("🟠" if n >= 8 else "🔴")
            bits = []
            both, o48, o128 = sorted(set(f48) & set(f128)), sorted(set(f48) - set(f128)), sorted(set(f128) - set(f48))
            if both:
                bits.append("fails " + ", ".join(both))
            if o48:
                bits.append("48K only: " + ", ".join(o48))
            if o128:
                bits.append("128K only: " + ", ".join(o128))
            if e48 or e128:
                bits.append("loads with wrong bytes at " + ", ".join(sorted(set(e48) | set(e128))))
            return emu, real, "; ".join(bits)

        sections = {}
        cur = None
        for line in guide.splitlines():
            m = re.match(r"^### (.+?)\s*$", line)
            if m:
                cur = m.group(1).strip()
            m = re.match(r"^\|\s*(\d+\.\d+)\s*\|[^|]*\|[^|]*\|\s*(\S+)\s*\|\s*(\S+)\s*\|\s*(.*?)\s*\|$", line)
            if m and cur:
                sections[(cur, m.group(1))] = (m.group(2), m.group(3), m.group(4))
        sec_to_key = {
            "ROM": "rom", "Milks": "milks", "FSK": "fsk", "Shavings Slow": "slow",
            "Shavings Delta": "delta", "Shavings Raudo": "raudo", "Ultra": "ultra",
            "NPU": "npu", "Fi": "fi", "Fi Quadruple": "fiq", "Manchester": "manchester",
            "Man. diferencial": "manchester-dif", "Escurrido": "escurrido",
            "Rayo (added by this port)": "rayo",
        }
        checked = mismatched = 0
        for (sec, sp), (emu, real, note) in sections.items():
            key = f"{sec_to_key.get(sec, '?')}:{sp}"
            if key not in per:
                failures.append(f"GUIDE rating section {sec!r} does not map to a "
                                f"measured technique ({key})")
                continue
            checked += 1
            e, r, n = derived(key)
            if (e, r, n) != (emu, real, note):
                mismatched += 1
                failures.append(f"GUIDE rating row {key}: published "
                                f"{emu}/{real}/{note!r}, measured {e}/{r}/{n!r}")
        check(f"all {checked} of {len(per)} published rating rows match the measurement",
              mismatched == 0 and checked == len(per),
              f"{mismatched} row(s) differ; {checked} of {len(per)} rows were compared")
        clean = [k for k in per
                 if min(sum(1 for c in conds if per[k][m][c] == "0") for m in ("48K", "128K")) == 10]
        check(f"the guide's 'six of the sixty-one' clean set ({len(clean)} found)",
              len(clean) == 6 and len(per) == 61
              and re.search(r"six of the sixty-?one", readme)
              and re.search(r"six of the sixty-?one", guide),
              f"{len(clean)} clean of {len(per)} combinations")
    else:
        print("  skip  rating rows (no tools/channel/ratings.json snapshot)")

    # ---- the MP3/AAC table -------------------------------------------------
    mp3snap = os.path.join(ROOT, "tools", "channel", "mp3v.json")
    if os.path.exists(mp3snap):
        data = json.load(open(mp3snap))
        hdr = [i for i, l in enumerate(guide.splitlines())
               if l.startswith("| Technique | bps @ 44.1 kHz | WAV |")]
        if not hdr:
            failures.append("GUIDE.md: the MP3 table is gone but its snapshot remains")
        else:
            rows = {}
            for line in guide.splitlines()[hdr[0] + 2:]:
                if not line.startswith("|"):
                    break
                c = [x.strip() for x in line.strip().strip("|").split("|")]
                rows[re.sub(r"[^A-Za-z0-9.]", "", c[0])] = c[2:]
            cols = ["WAV", "MP3 320", "MP3 192", "MP3 128", "MP3 96"]
            bad, n = [], 0
            for label, cells in data.items():
                key = re.sub(r"[^A-Za-z0-9.]", "", label)
                if key not in rows:
                    bad.append(f"{label}: not in the guide")
                    continue
                got = rows[key]
                for i, col in enumerate(cols[:len(got)]):
                    # Only the AAC column may be "—", and only because faac could
                    # not decode here.  Any other unrecognised mark means the row
                    # has been emptied or corrupted, and must not be skipped.
                    if got[i] not in ("✅", "⚠️", "❌"):
                        if i == len(cols) - 1 and got[i] in ("—", "-"):
                            continue
                        bad.append(f"{label} {col}: unrecognised mark {got[i]!r}")
                        continue
                    p = cells[0][2] if i == 0 else any(
                        x[0] == col and x[1] == "perfect" and x[2] for x in cells)
                    m = cells[0][2] if i == 0 else any(
                        x[0] == col and x[1] == "modelled" and x[2] for x in cells)
                    want = "✅" if (p and m) else ("⚠️" if (p or m) else "❌")
                    n += 1
                    if got[i] != want:
                        bad.append(f"{label} {col}: guide {got[i]}, measured {want}")
            check(f"all {n} of {len(data) * len(cols)} MP3 table cells match the "
                  f"measurement", not bad and n == len(data) * len(cols),
                  "; ".join(bad[:3]) or f"only {n} cells compared")
    else:
        print("  skip  MP3 table (no tools/channel/mp3v.json snapshot)")

    # ---- bit rates: every bps figure in the guide must equal the formula ----
    # samplesToBps(): 1.33 is (rate*3)/4, everything else (rate*4)/mpb.
    defs = read("src/core/defs.h")
    # zero-pad ("07" -> "7.00") and make the trailing comma optional: the last
    # enumerator has none, and losing it silently dropped a speed.
    enum = {f"{int(a)}.{int(b):02d}": int(c)
            for a, b, c in re.findall(r"kS(\d)_(\d\d) = (\d+),?", defs)}
    if not enum:
        # A check that parsed nothing must fail, or it silently passes forever.
        check("the SamplesPerBit enum could be read from defs.h", False,
              "the regex matched no entries, so every bps check would vacuously pass")
    else:
        bps = {sp: (48000 * 3) // 4 if v == 4 else (48000 * 4) // v
               for sp, v in enum.items()}
        # Only the per-technique rating tables carry a pure number in the bps
        # column; Rayo's two say "before compression".  Compare every numeric
        # one, and require the count to equal the number of rating rows so the
        # check can never quietly stop looking at anything.
        hdr = "| Speed | bps @ 48 kHz |"   # prefix of the table header
        numeric = total = 0
        bad = []
        glines = guide.splitlines()
        for i, line in enumerate(glines):
            if not line.strip().startswith(hdr):
                continue
            j = i + 2
            while j < len(glines) and glines[j].startswith("|"):
                cells = [c.strip() for c in glines[j].strip().strip("|").split("|")]
                total += 1
                if cells[0] in bps and re.match(r"^[\d,]+$", cells[1]):
                    numeric += 1
                    want = f"{bps[cells[0]]:,}"
                    if cells[1] != want:
                        bad.append(f"{cells[0]}: guide says {cells[1]}, "
                                   f"SamplesPerBit says {want}")
                j += 1
        check(f"all {numeric} of {total} bps figures match samplesToBps()",
              not bad and total > 0 and numeric == total - 2,
              "; ".join(bad[:3]) or f"only {numeric}/{total} rows were found")

    # ---- the technique/speed lists must match the code ----
    speeds = {}
    for m in re.finditer(r"case (k\w+):\s*return \{([^}]*)\}", settings_cpp):
        speeds[m.group(1)] = [int(x.replace("kS", "")) for x in m.group(2).split(",") if x.strip()]
    # Manchester and Manchester-Dif share one `case`, so 13 arms cover 14 techniques.
    check(f"{len(speeds)} technique arms cover at least 14 techniques", len(speeds) >= 13)

    # ---- the mp3 bit rate is fixed, and the label must say so ---------------
    # The constant moved to the core in step 17, when the mp3 plumbing became
    # shared between the GUI and k7zx-cli instead of living in the GUI alone.
    src = read("src/core/mp3.h")
    m = re.search(r"kMp3Bitrate = (\d+);", src)
    if not m:
        failures.append("could not find kMp3Bitrate in src/core/mp3.h")
    else:
        rate = m.group(1)
        # The visible label must be *derived* from the constant, not typed out,
        # or it will quietly go stale the next time the rate changes.
        label_from_constant = ('kMp3Bitrate) + " kbps, fixed"' in read("src/gui/mainwindow.cpp"))
        check(f"the mp3 bit rate is stated as {rate} everywhere it appears",
              all(f"{rate} kbps" in d for d in (readme, guide))
              and label_from_constant,
              "README/GUIDE/label disagree with the code"
              + ("" if label_from_constant else "; the label is a literal"))
        check(f"{rate} kbps is a rate the MP3 table actually measures",
              rate in {"320", "192", "128", "96"},
              f"kMp3Bitrate={rate} is not one of the measured rates")

    # ---- counts quoted in the documents ----
    ntests = 0
    for f in sorted(os.listdir(os.path.join(ROOT, "tests"))):
        if f.startswith("test_") and f.endswith(".cpp"):
            ntests += len(re.findall(r"^TEST\(", read("tests/" + f), re.M))
    check(f"README's test count matches the suite ({ntests} tests)",
          f"**{ntests} unit tests**" in readme and f"{ntests}/{ntests} tests" in readme,
          f"expected {ntests}")
    rows = sum(1 for l in read("tests/golden_k7zx43.inc").splitlines()
               if l.startswith("{") and "low-rate flag" not in l)
    check(f"the golden table has the row count README quotes ({rows})",
          f"{rows} rows" in readme or f"{rows} of the 354" in readme,
          f"actual {rows}, README quotes something else")
    sweep = os.path.join(ROOT, "tools", "zxload", "sweep-step15.txt")
    if os.path.exists(sweep):
        s = open(sweep).read().splitlines()
        p = sum(1 for l in s if l.strip().endswith("PASS"))
        f_ = sum(1 for l in s if l.strip().endswith("FAIL"))
        check(f"the recorded sweep is {len(s)} cases, {p} pass and {f_} fail",
              f"{len(s)}" in readme and p + f_ == len(s) and p == 338 and f_ == 24,
              f"actual {len(s)} cases, {p} pass, {f_} fail")

    # ---- no option or key that no longer exists ----
    binpath = (sys.argv[2] if len(sys.argv) > 2
               else os.path.join(ROOT, "build", "k7zx-cli"))
    if os.path.exists(binpath):
        help_txt = subprocess.run([binpath, "--help"],
                                  capture_output=True, text=True).stdout
        gui = read("src/gui/main.cpp")
        badopt = []
        for opt in sorted(set(re.findall(r"`(--[a-z][a-z-]+)`", guide + readme))):
            # --resample is the encoder hint k7zx appends to its own command
            # line; the user never types it.
            if opt in ("--help", "--version", "--resample"):
                continue
            if opt not in help_txt and opt not in gui:
                badopt.append(opt)
                failures.append(f"{opt} is documented but no program accepts it")
        check("every documented option exists", not badopt,
              "; ".join(badopt[:3]))
    else:
        print("  skip  option check (build/k7zx-cli not built)")

    # ---- invariants introduced by the step 17 review ----
    # Each of these was a real bug; they are cheap to re-check and expensive to
    # regress, so they are asserted here rather than left to a code reading.

    # The published TZX header is 12 bytes.  k7zx wrote 10 (its struct was 10
    # bytes long) and read 10, so no .tzx from another tool could be read.
    wav = read("src/core/zxwav.cpp")
    m = re.search(r'h\.raw\("ZXTape!", 7\);.*?h\.le16\(0x[0-9a-fA-F]{4}\);', wav, re.S)
    check("the TZX writer emits a 12 byte header", m is not None,
          "no start-block-id field after the minor version byte")
    tst = read("tests/test_zxwav.cpp")
    check("the TZX header length is asserted in the tests",
          "r.le16(10), 0x0030" in tst and "kBlock = 64" in tst,
          "test_zxwav.cpp still assumes a 10 byte header")

    # The page-swap hook table holds one 5-byte entry per slot from 3 + 20.
    hdr = read("src/core/zxfiles.h")
    m = re.search(r"kMulti128Size = (\d+);", hdr)
    slots = re.search(r"kMulti128Slots = (\d+);", hdr)
    check("the 128K page-slot count fits the hook table",
          bool(m) and bool(slots)
          and 3 + 21 + (int(slots.group(1)) - 1) * 5 < int(m.group(1)),
          "kMulti128Slots does not fit kMulti128Size")

    # The lookup table had one entry too many, shifting every index from 26 up.
    inv = read("src/core/zxwav.cpp")
    m = re.search(r"kInvSqrt\[\] = \{(.*?)\};", inv, re.S)
    n = len(re.findall(r"\d+\.\d+", m.group(1))) if m else 0
    check(f"the inverse-root table holds the original's 70 entries (found {n})", n == 70,
          "one entry too many or too few shifts every index from 26 up")

    # k7zx-cli must not overwrite the stored settings with defaults for options
    # the user never gave.
    cli = read("src/cli/main.cpp")
    guarded, unguarded = [], []
    for field, target in (("mode", "conversionMode"), ("rate", "sampleRate"),
                          ("waveform", "waveform"), ("scheme", "scheme"),
                          ("stereo", "stereo"), ("invert", "invert"),
                          ("invertRight", "invertRight"), ("finalTone", "finalTone"),
                          ("accelerate", "accelerateBasic"),
                          ("checksum", "controlChecksum"), ("loader", "generateLoader"),
                          ("kolmogorov", "antiKolmogorov"), ("compress", "compress"),
                          ("spb", "samplesPerBit"), ("method", "method")):
        if ("if (o.%s) settings.%s = *o.%s;" % (field, target, field)) in cli:
            guarded.append(field)
        elif re.search(r"^\s*settings\.%s = o\.%s\b" % (target, field), cli, re.M):
            unguarded.append(field)
    check(f"k7zx-cli leaves the stored settings alone for the {len(guarded)} options "
          "nobody passed", not unguarded,
          "these still overwrite k7zx.ini even when not given: " + ", ".join(unguarded))

    # ---- removed things must not still be documented as present ----
    for gone, why in (("--divisor", "the 1/2 Frequency option was removed"),
                      ("1/2 Frequency", "the 1/2 Frequency option was removed")):
        live = re.findall(r"^\s*[|*-].*%s.*$" % re.escape(gone), guide + readme, re.M)
        live = [l for l in live
                if "not supported" not in l and "was removed" not in l
                and "removed here too" not in l and "held only" not in l
                and "was removed here" not in l and "removed too" not in l
                and "since been removed" not in l]
        check(f"no live documentation of {gone}", not live, why + ("" if not live else f" {live[:1]}"))

    # ---- cross references ----
    broken = []
    for doc, name in ((guide, "GUIDE.md"), (readme, "README.md")):
        for path in set(re.findall(r"`((?:src|tests|tools|original)/[A-Za-z0-9_./-]+)`", doc)):
            if not os.path.exists(os.path.join(ROOT, path)):
                broken.append(f"{name}: {path}")
        # Compare each in-document link against the headings that exist.  Testing
        # `anchor in doc` could never fail: the link itself put the string there.
        heads = set()
        for h in re.findall(r"^#{1,6}\s+(.*?)\s*$", doc, re.M):
            slug = re.sub(r"[^a-z0-9 -]", "", h.lower()).strip().replace(" ", "-")
            heads.add(slug)
            heads.add(re.sub(r"-+", "-", slug))
        for anchor in set(re.findall(r"\]\(#([a-z0-9-]+)\)", doc)):
            if anchor not in heads:
                broken.append(f"{name}: anchor #{anchor}")
    check("every file reference and anchor resolves", not broken, "; ".join(broken[:3]))

    print()
    if failures:
        print(f"{len(failures)} finding(s) no longer hold:")
        for f in failures:
            print("  - " + f)
        return 1
    print("every published finding still agrees with the code")
    return 0


if __name__ == "__main__":
    sys.exit(main())
