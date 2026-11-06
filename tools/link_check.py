#!/usr/bin/env python3
"""Find out what the port has to supply before the game links.

Reads the objects built by unify_prototypes.py --compile and works out, from
their symbol tables:

  defined     symbols the game and library code provide
  duplicate   global symbols defined in more than one object
  missing     symbols referenced but defined nowhere in the game code

Missing symbols are then split by asking the real linker: everything is
linked against devkitARM's newlib, libgcc and libctru, with unresolved symbols
reported instead of fatal. What the libraries resolve is not our problem;
what is left is the list the platform layer (and the remaining units) has to
provide.

Usage:
    python tools/link_check.py [--no-link]

Output: build/link_check/report.json and a summary on stdout.
"""

import argparse
import json
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import compile_check  # noqa: E402

BIN = compile_check.DEVKITARM / "bin"

# How a missing symbol is filed. First match wins.
CATEGORIES = [
    ("sdk-gx", r"^(GX|__GX|gx[A-Z])"),
    ("sdk-os", r"^(OS|__OS|PPC|__PPC|LC[A-Z]|DB[A-Z]|__DB|BOOT_|EXI|__EXI)"),
    ("sdk-dvd", r"^(DVD|__DVD)"),
    ("sdk-card", r"^(CARD|__CARD)"),
    ("sdk-pad", r"^(PAD|__PAD|SI[A-Z]|__SI)"),
    ("sdk-vi", r"^(VI[A-Z]|__VI)"),
    ("sdk-audio", r"^(AI[A-Z]|__AI|AR[A-Z]|ARQ|__AR|DSP|__DSP)"),
    ("sdk-gba", r"^(GBA|__GBA|JOY)"),
    ("sdk-thp", r"^(THP|__THP)"),
    ("sdk-math", r"^(PS|C_)?(MTX|VEC|QUAT|Mtx|Vec)[A-Z]"),
    ("musyx", r"^(snd|sal|synth|hw|seq|vs|inp|adsr|s3d|macro|voice|aram|stream|dataInsert|dataRemove|sndSeq|_snd)"),
    ("hsd", r"^(HSD_|_HSD|hsd)"),
    ("runtime", r"^(__|_[a-z]|_savegpr|_restgpr)"),
    ("data", r"^(lbl_|gap_|jumptable_|\.L)"),
    ("game-fn", r"^fn_8"),
]


def nm(obj):
    p = subprocess.run([str(BIN / "arm-none-eabi-nm.exe"), str(obj)], capture_output=True, text=True)
    defined, undefined, weak = [], [], []
    for line in p.stdout.splitlines():
        parts = line.split()
        if len(parts) == 2 and parts[0] == "U":
            undefined.append(parts[1])
        elif len(parts) == 3:
            kind, name = parts[1], parts[2]
            if kind in "TDBRSGC":
                defined.append((name, kind))
            elif kind in "WVwv":
                weak.append(name)
    return obj, defined, undefined, weak


def category(name):
    for cat, pat in CATEGORIES:
        if re.search(pat, name):
            return cat
    return "game"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--unified", type=Path, default=ROOT / "build" / "unified")
    ap.add_argument("--no-link", action="store_true")
    ap.add_argument("-o", "--out", type=Path, default=ROOT / "build" / "link_check")
    args = ap.parse_args()

    results = json.loads((args.unified / "compile.json").read_text(encoding="utf-8"))
    objs = [args.unified / "obj" / (r["name"] + ".o") for r in results if r["ok"]]
    failed_units = [r["source"] for r in results if not r["ok"]]

    with ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        tables = list(pool.map(nm, objs))

    where = defaultdict(list)
    needed = defaultdict(set)
    weak = set()
    for obj, defined, undefined, w in tables:
        unit = obj.relative_to(args.unified / "obj").as_posix()[:-2]
        for name, kind in defined:
            if kind != "C":
                where[name].append(unit)
        for name in undefined:
            needed[name].add(unit)
        weak.update(w)

    duplicates = {k: v for k, v in where.items() if len(v) > 1}
    missing = sorted(n for n in needed if n not in where and n not in weak)

    summary = {
        "objects": len(objs),
        "units_not_compiled": failed_units,
        "defined": len(where),
        "duplicates": len(duplicates),
        "missing": len(missing),
    }
    report = {"summary": summary, "duplicates": duplicates,
              "missing": {n: sorted(needed[n])[:5] + ([f"... {len(needed[n])} units"]
                                                      if len(needed[n]) > 5 else [])
                          for n in missing}}

    resolved_by_libs = set()
    if not args.no_link:
        resolved_by_libs = link(objs, missing, args.out)
    left = [n for n in missing if n not in resolved_by_libs]

    # Where does the decomp define each one? Its MWCC objects say, if the
    # decomp has been built.
    owner = decomp_owners()
    by_cat = defaultdict(list)
    for n in left:
        if n in owner:
            by_cat["from:" + owner[n]].append(n)
        else:
            by_cat["nowhere:" + category(n)].append(n)
    summary["resolved_by_newlib_libctru"] = sorted(resolved_by_libs)
    summary["left"] = len(left)
    summary["left_by_category"] = {k: len(v) for k, v in sorted(by_cat.items(), key=lambda kv: -len(kv[1]))}
    report["left_by_category"] = {k: sorted(v) for k, v in by_cat.items()}

    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "report.json").write_text(json.dumps(report, indent=1), encoding="utf-8")

    print(f"{len(objs)} objects, {len(failed_units)} units not compiled")
    print(f"{len(where)} symbols defined, {len(duplicates)} defined more than once")
    print(f"{len(missing)} symbols referenced but not defined")
    if not args.no_link:
        print(f"  {len(resolved_by_libs)} resolved by newlib/libgcc/libctru: "
              + ", ".join(sorted(resolved_by_libs)[:30]) + (" ..." if len(resolved_by_libs) > 30 else ""))
    print(f"  {len(left)} left for the platform layer and the remaining units:")
    for k, v in sorted(by_cat.items(), key=lambda kv: -len(kv[1])):
        print(f"    {k:<10} {len(v):5}   e.g. {', '.join(v[:6])}")
    if duplicates:
        print("duplicates (first 15):")
        for k in sorted(duplicates)[:15]:
            print(f"    {k}: {', '.join(duplicates[k][:3])}")


def decomp_owners():
    """symbol -> group of the decomp unit that defines it, from the decomp's
    own MWCC objects (base_path in objdiff.json)."""
    manifest = json.loads((ROOT / "build" / "objects.json").read_text(encoding="utf-8"))
    decomp = Path(manifest["decomp"])
    group_of = {u["name"]: u["group"] for u in manifest["units"]}
    failed, seen = set(), set()
    unified = ROOT / "build" / "unified" / "compile.json"
    if unified.exists():
        rows = json.loads(unified.read_text(encoding="utf-8"))
        failed = {r["name"] for r in rows if not r["ok"]}
        seen = {r["name"] for r in rows}
    units = json.loads((decomp / "objdiff.json").read_text(encoding="utf-8"))["units"]
    objdump_nm = decomp / "build" / "binutils" / "powerpc-eabi-nm.exe"
    if not objdump_nm.exists():
        return {}

    def one(u):
        obj = decomp / u.get("base_path", "")
        if not u.get("base_path") or not obj.exists():
            return u["name"], []
        p = subprocess.run([str(objdump_nm), "--defined-only", str(obj)], capture_output=True, text=True)
        return u["name"], [l.split()[-1] for l in p.stdout.splitlines()
                           if len(l.split()) == 3 and l.split()[1] in "TDBRSGV"]

    with ThreadPoolExecutor(os.cpu_count() or 4) as pool:
        res = list(pool.map(one, units))
    owner = {}
    for name, syms in res:
        group = group_of.get(name, "?")
        if name in failed:
            group = "not-compiled-yet"
        elif group in ("game", "library") and name not in seen:
            group = "c++-units"
        elif group in ("game", "library"):
            group = "compiled-but-missing"
        for s in syms:
            owner.setdefault(s, group)
    return owner


def link(objs, missing, out):
    """Link against the libraries with unresolved symbols allowed, then see
    which of `missing` the libraries defined."""
    out.mkdir(parents=True, exist_ok=True)
    rsp = out / "objects.rsp"
    # The driver's @file parser treats backslashes as escapes.
    rsp.write_text("\n".join(o.as_posix() for o in objs) + "\n", encoding="utf-8")
    elf = out / "probe.elf"
    mapf = out / "probe.map"
    devkitpro = compile_check.DEVKITARM.parent
    cmd = [str(BIN / "arm-none-eabi-gcc.exe"), "-specs=3dsx.specs", *compile_check.ARCH,
           f"@{rsp.as_posix()}",
           "-L", str(devkitpro / "libctru" / "lib"), "-lctru", "-lm",
           "-Wl,--unresolved-symbols=ignore-all", "-Wl,--allow-multiple-definition",
           f"-Wl,-Map,{mapf.as_posix()}", "-o", str(elf)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    (out / "link.log").write_text(p.stdout + p.stderr, encoding="utf-8")
    if p.returncode != 0:
        print("probe link failed, see build/link_check/link.log")
        print((p.stdout + p.stderr)[-1500:])
        return set()
    syms = subprocess.run([str(BIN / "arm-none-eabi-nm.exe"), str(elf)], capture_output=True, text=True).stdout
    defined = {l.split()[-1] for l in syms.splitlines() if len(l.split()) == 3 and l.split()[1] not in "Uw"}
    return {n for n in missing if n in defined}


if __name__ == "__main__":
    main()
