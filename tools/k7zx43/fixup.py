#!/usr/bin/env python3
"""fixup.py SRC_DIR OUT_DIR -- copy k7zx 4.3's engine sources and apply the
purely mechanical edits g++ needs.  No logic is changed:

  * implicit-int functions (`Bloque2ROM (...)`) get an explicit `int`
  * `#include <stdlib.h >` loses its stray space
  * zxfiles.cpp's two VCL calls (ExtractFileExt/ExtractFileName) become the
    plain C helpers in shim_c.h
"""
import os
import re
import shutil
import sys

SOURCES = ["zxwav.cpp", "ZXCODE.CPP", "rutinas.cpp", "zxfiles.cpp",
           "zxwav.h", "ZXCODE.h", "rutinas.h", "zxfiles.h"]
KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "else", "do", "case", "default"}


def main():
    src, out = sys.argv[1], sys.argv[2]
    os.makedirs(out, exist_ok=True)
    for name in SOURCES:
        s = open(os.path.join(src, name), encoding="latin-1").read()
        lines = s.split("\n")
        for i, line in enumerate(lines):
            m = re.match(r"^([A-Za-z_]\w*)\s*\(", line)
            if m and m.group(1) not in KEYWORDS:
                lines[i] = "int " + line
        s = "\n".join(lines)
        s = s.replace("<stdlib.h >", "<stdlib.h>").replace("<string.h >", "<string.h>")
        if name == "zxfiles.cpp":
            s = s.replace("#include <SysUtils.hpp>", '#include "shim_c.h"')
            s = s.replace("AnsiString ext=ExtractFileExt(filename).LowerCase();",
                          "const char *ext=shim_ext_lower(filename);")
            for e in ("hex", "tap", "tzx", "sna", "z80", "sbb"):
                s = s.replace(f'if (ext==".{e}")', f'if (!strcmp(ext,".{e}"))')
            s = s.replace("ExtractFileName (filename).c_str()", "shim_basename(filename)")
            assert "AnsiString" not in s and "Extract" not in s
        dest = "zxcode_impl.cpp" if name == "ZXCODE.CPP" else name
        open(os.path.join(out, dest), "w", encoding="latin-1").write(s)
    # ZXCODE.CPP includes "zxcode.h"; the header is spelled ZXCODE.h.
    shutil.copy(os.path.join(out, "ZXCODE.h"), os.path.join(out, "zxcode.h"))


if __name__ == "__main__":
    main()
