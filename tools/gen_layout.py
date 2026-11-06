#!/usr/bin/env python3
"""Generate a linker script that lays out game data as on the GameCube.

Game code sometimes reaches an object through the address of its neighbour:
a struct dtk split into several labels, or (on the GameCube) MWCC's reversed
.sbss order that the decomp's sources reproduce. That only works if the
port keeps the retail distances between data symbols.

With every unit compiled with -fdata-sections, each data symbol sits in a
section of its own (.data.NAME, .bss.NAME, .rodata.NAME; aliased
definitions use the real name, their assembler label). This script places
those sections at their retail offsets:

  - one output section per retail data section (.rodata, .data, .bss,
    .sdata, .sbss, .sdata2, .sbss2 of the DOL; each REL module's sections
    separately), starting 32-byte aligned like the retail sections;
  - in it, each symbol the port defines at `. = retail address - section
    start`, from the object that defines it;
  - zero-initialised groups go after __bss_start__, the rest at the start
    of the data segment, inside devkitARM's own 3dsx.ld.

Where a symbol cannot go at its retail offset (the port's object is larger
than the retail gap, or it is initialised data in a retail .bss section) it
is left to the default placement and reported.

Usage (normally run by tools/link_probe.py):
    python tools/gen_layout.py OBJECTS.rsp OUT.ld OUT.specs [ALIASES.txt]

ALIASES.txt lines are `name parent offset`; each becomes `name = parent +
offset;` in the script.
"""

import json
import re
import subprocess
import sys
from collections import defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import compile_check  # noqa: E402

BIN = compile_check.DEVKITARM / "bin"
BASE_LD = compile_check.DEVKITARM / "arm-none-eabi" / "lib" / "3dsx.ld"
SYM = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")
DATA_SECTIONS = (".rodata", ".data", ".bss", ".sdata", ".sbss", ".sdata2", ".sbss2")
ZERO_SECTIONS = (".bss", ".sbss", ".sbss2")
SEC = re.compile(r"^\s*\[\s*\d+\]\s+(\S+)\s+(PROGBITS|NOBITS)\s+\S+\s+\S+\s+([0-9a-f]+)"
                 r"\s+\S+\s+\S*\s+\d+\s+\d+\s+(\d+)\s*$")


def retail_symbols(decomp):
    """[(group, name, address, size)] for data objects; group = (module, section)."""
    out = []
    cfg = decomp / "config" / "GC6E01"
    for path in [cfg / "symbols.txt"] + sorted(cfg.glob("*/symbols.txt")):
        module = "main" if path.parent == cfg else path.parent.name
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = SYM.match(line)
            if not m or m.group(4) != "object" or m.group(2) not in DATA_SECTIONS:
                continue
            out.append(((module, m.group(2)), m.group(1), int(m.group(3), 16), int(m.group(5) or "0", 16)))
    return out


def object_sections(obj):
    """{symbol: (section name, kind, size)} for one object's .data.X etc."""
    out = subprocess.run([str(BIN / "arm-none-eabi-readelf.exe"), "-SW", str(obj)],
                         capture_output=True, text=True).stdout
    res = {}
    for line in out.splitlines():
        m = SEC.match(line)
        if not m:
            continue
        name, kind, size, align = m.group(1), m.group(2), int(m.group(3), 16), int(m.group(4))
        for prefix in (".data.", ".bss.", ".rodata.", ".sdata.", ".sbss."):
            if name.startswith(prefix) and not name.startswith(".rodata.str"):
                res[name[len(prefix):]] = (name, kind, size, max(align, 1))
                break
    return obj, res


def main():
    rsp, out_ld, out_specs = (Path(a) for a in sys.argv[1:4])
    manifest = json.loads((ROOT / "build" / "objects.json").read_text(encoding="utf-8"))
    decomp = Path(manifest["decomp"])
    objs = [Path(l) for l in rsp.read_text(encoding="utf-8").split() if l]

    with ThreadPoolExecutor() as pool:
        tables = list(pool.map(object_sections, objs))
    provider = {}
    dup = set()
    for obj, secs in tables:
        for sym, info in secs.items():
            if sym in provider:
                dup.add(sym)
            provider.setdefault(sym, (obj, *info))

    groups = defaultdict(list)
    for group, name, addr, size in retail_symbols(decomp):
        groups[group].append((addr, name, size))

    data_blocks, bss_blocks = [], []
    placed = skipped = 0
    problems = []
    for (module, section), syms in sorted(groups.items()):
        syms.sort()
        start = syms[0][0]
        zero = section in ZERO_SECTIONS
        lines = []
        pos = 0
        for addr, name, size in syms:
            if name not in provider or name in dup:
                continue
            obj, secname, kind, psize, align = provider[name]
            if zero and kind != "NOBITS":
                problems.append(f"{name}: initialised in the port, {section} on the GameCube")
                skipped += 1
                continue
            off = addr - start
            if off < pos:
                problems.append(f"{name}: at +{off:#x} would overlap the previous object (port size)")
                skipped += 1
                continue
            if off % min(align, 0x20):
                problems.append(f"{name}: at +{off:#x}, but the port aligns it to {align}")
                skipped += 1
                continue
            pattern = "*" + obj.resolve().relative_to(ROOT / "build").as_posix()
            lines.append(f"\t\t. = {off:#x}; KEEP({pattern}({secname}))")
            pos = off + psize
            placed += 1
        if not lines:
            continue
        label = f".gc_{module}{section.replace('.', '_')}"
        block = f"\t{label} ALIGN(0x20) : {{\n" + "\n".join(lines) + "\n\t} : data\n"
        (bss_blocks if zero else data_blocks).append(block)

    base = BASE_LD.read_text(encoding="utf-8")
    k = base.index("=========== DATA section")
    i = base.index(". = ALIGN(0x1000);", k) + len(". = ALIGN(0x1000);")
    base = base[:i] + "\n" + "".join(data_blocks) + base[i:]
    j = base.index("__bss_start__ = .;") + len("__bss_start__ = .;")
    base = base[:j] + "\n" + "".join(bss_blocks) + base[j:]
    # Labels inside a larger object the game defines (see link_probe.py).
    if len(sys.argv) > 4:
        rows = [l.split() for l in Path(sys.argv[4]).read_text(encoding="utf-8").splitlines() if l.strip()]
        base += "\n/* labels inside larger game objects */\n" + "".join(
            f"{n} = {p} + {int(d)};\n" for n, p, d in rows)
    out_ld.write_text(base, encoding="utf-8")
    out_specs.write_text(
        "%include <sync-dmb.specs>\n\n*link:\n"
        f"+ -T {out_ld.resolve().as_posix()} -d --emit-relocs --use-blx --gc-sections\n\n"
        "*startfile:\n3dsx_crt0%O%s crti%O%s crtbegin%O%s\n", encoding="utf-8")
    (out_ld.parent / "layout_problems.txt").write_text("\n".join(problems) + "\n", encoding="utf-8")
    print(f"layout: {placed} data symbols at their GameCube offsets, {skipped} left to the default "
          f"placement ({len(dup)} names defined twice skipped); see {out_ld.parent / 'layout_problems.txt'}")


if __name__ == "__main__":
    main()
