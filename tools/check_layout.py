#!/usr/bin/env python3
"""Check that data the game reaches across symbols is laid out as on the GameCube.

Some game code uses one symbol's address to reach the object next to it, for
example a struct that dtk split into several labels (the code reads +3 of
lbl_80478D78, which dtk calls lbl_80478D7B). That only works if the port
places those symbols at the same distance from each other as the retail
binary does.

Input: build/cross_symbol.json from tools/access_widths.py (every access
the original code makes through a neighbouring symbol). For each (via,
target) pair this compares the GameCube distance (symbols.txt) with the
distance in the linked port, build/link_probe/colosseum.elf.

Usage:
    python tools/check_layout.py
"""

import json
import re
import subprocess
import sys
from collections import defaultdict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import compile_check  # noqa: E402
from access_widths import symbol_addresses  # noqa: E402

NM = compile_check.DEVKITARM / "bin" / "arm-none-eabi-nm.exe"


def main():
    cross = json.loads((ROOT / "build" / "cross_symbol.json").read_text(encoding="utf-8"))
    manifest = json.loads((ROOT / "build" / "objects.json").read_text(encoding="utf-8"))
    addrs = symbol_addresses(Path(manifest["decomp"]))
    out = subprocess.run([str(NM), str(ROOT / "build" / "link_probe" / "colosseum.elf")],
                         capture_output=True, text=True).stdout
    elf = {p[2]: int(p[0], 16) for p in (l.split() for l in out.splitlines()) if len(p) == 3}

    pairs = sorted({(c["via"], c["target"]) for c in cross
                    if c["target"] != "?" and not c["via"].startswith(("@", "STACK:"))})
    ok, bad, unused = [], [], 0
    for via, target in pairs:
        if via not in elf or target not in elf:
            unused += 1          # not linked into the port, or never named
            continue
        gc = addrs[target][1] - addrs[via][1]
        port = elf[target] - elf[via]
        (ok if port == gc else bad).append((via, target, gc, port))

    print(f"{len(pairs)} (via, target) pairs, {unused} not both in the port's link")
    print(f"  {len(ok)} at the GameCube distance")
    print(f"  {len(bad)} at another distance")
    for x in bad[:30]:
        print(f"      {x[0]} -> {x[1]}: GameCube {x[2]:+#x}, port {x[3]:+#x}")


if __name__ == "__main__":
    main()
