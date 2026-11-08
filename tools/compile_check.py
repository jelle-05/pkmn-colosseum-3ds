#!/usr/bin/env python3
"""Compile every decomp unit for the 3DS and report what breaks.

This is a survey, not the real build. Each unit from build/objects.json is
compiled on its own with devkitARM, and the errors and portability warnings
are collected into a report. Nothing is linked.

Usage:
    python tools/compile_check.py [--groups game,library] [--jobs N]
                                  [--no-compat] [--only SUBSTRING]

Output:
    build/compile_check/report.json   summary and per-unit results
    build/compile_check/logs/...      full compiler output per unit

The flags downgrade the errors GCC 14 introduced for old C (implicit
declarations, int/pointer conversions) to warnings, so one problem does not
hide the rest. Those warnings are still counted, because on ARM an implicit
declaration passes float arguments in the wrong registers.
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

DEVKITARM = Path(os.environ.get("DEVKITARM_WIN", r"C:\devkitPro\devkitARM"))

# The game and the platform layer run in the ARM11's big-endian data mode
# (BE8: instructions stay little-endian, data accesses are big-endian), so
# GameCube data, compiled in or read from the disc, is used as it is. Only
# platform/host, which talks to libctru and citro3d, is little-endian
# (docs/BIG_ENDIAN.md).
BIG_ENDIAN = True
ARCH_LE = ["-march=armv6k", "-mtune=mpcore", "-mfloat-abi=hard", "-mtp=soft"]
ARCH = ARCH_LE + (["-mbig-endian"] if BIG_ENDIAN else [])

# Optimisation. The game is built at -O2 without GCC's own inlining of
# functions not declared inline. The game polls state that interrupts and
# other threads change, through small accessor functions:
#     while (fn_801808B4(entry)) {}       /* ARAM copy done? */
# Inlined, such a loop has no call left and GCC reads the field once: at
# plain -O2 the game hung in its first ARAM copy (fn_80180450). MWCC did not
# hoist those loads. Also off: paths that dereference null become traps
# (the decomp passes null on purpose), interprocedural register allocation
# (the coroutine switch) and merging identical functions (callbacks are
# compared by address).
OPT = ("-O2 -fno-inline-small-functions -fno-inline-functions -fno-isolate-erroneous-paths-dereference"
       " -fno-ipa-ra -fno-ipa-icf")
OPT_PLATFORM = "-O2"

# Language semantics that must match MWCC's C89-era behaviour, for every
# compile of decomp code:
#   -fgnu89-inline       a plain `inline` definition still emits an external
#                        symbol (C99 would drop it and other units would fail
#                        to link)
#   -fno-strict-aliasing the decomp reads data through mismatched pointer types
#   -fwrapv              signed overflow wraps, as on the GameCube
#   -funsigned-char      plain char is unsigned on both, but say so
#   -fno-toplevel-reorder  keep globals in source order, so objects the game
#                        reaches through a neighbouring label stay at the
#                        GameCube distance (tools/check_layout.py)
#   -fno-common          tentative definitions get real storage in order
#   -fdata-sections      one section per data symbol, so tools/gen_layout.py
#                        can place each at its retail offset
SEMANTICS = ["-fgnu89-inline", "-fno-strict-aliasing", "-fwrapv", "-funsigned-char",
             "-fno-toplevel-reorder", "-fno-common", "-fdata-sections"]

# Warnings that are portability bugs on ARM, not style. Counted separately.
PORTABILITY_WARNINGS = {
    "implicit-function-declaration",
    "implicit-int",
    "int-conversion",
    "incompatible-pointer-types",
    "return-mismatch",
    "int-to-pointer-cast",
    "pointer-to-int-cast",
    "overflow",
    "multichar",
    "double-promotion",
}

PERMISSIVE = [
    "-Wno-error=implicit-function-declaration",
    "-Wno-error=implicit-int",
    "-Wno-error=int-conversion",
    "-Wno-error=incompatible-pointer-types",
    "-Wno-error=return-mismatch",
    "-Wno-error=declaration-missing-parameter-type",
]

DIAG = re.compile(r"^(?P<file>.*?):(?P<line>\d+):(?P<col>\d+): (?P<kind>error|warning): (?P<msg>.*)$")
FLAG = re.compile(r"\[-W(?:error=)?([\w-]+)\]$")
QUOTED = re.compile(r"[â€˜'\"`][^â€™'\"`]*[â€™'\"`]")
NUMBER = re.compile(r"\b\d+\b")


def normalise(msg):
    msg = FLAG.sub("", msg).strip()
    msg = QUOTED.sub("'X'", msg)
    return NUMBER.sub("N", msg)


def rel(path, decomp):
    p = str(path).replace("\\", "/")
    d = str(decomp).replace("\\", "/").rstrip("/") + "/"
    return p[len(d):] if p.startswith(d) else p


def compile_unit(unit, decomp, out_dir, use_compat):
    src = decomp / unit["source"]
    obj = out_dir / "obj" / (unit["name"] + ".o")
    log = out_dir / "logs" / (unit["name"] + ".log")
    obj.parent.mkdir(parents=True, exist_ok=True)
    log.parent.mkdir(parents=True, exist_ok=True)

    cc = DEVKITARM / "bin" / ("arm-none-eabi-g++.exe" if unit["lang"] == "c++" else "arm-none-eabi-gcc.exe")
    cmd = [str(cc), *ARCH, "-O1", *SEMANTICS,
           "-fdiagnostics-plain-output", "-fno-diagnostics-show-caret",
           "-fmax-errors=50", *PERMISSIVE,
           "-Wdouble-promotion",
           "-DBUILD_VERSION=0", "-DVERSION_GC6E01", "-DNDEBUG=1", "-DPORT=1", "-D__3DS__",
           *("-D" + d for d in unit.get("defines", []))]
    if use_compat:
        # Platform overrides must win over the decomp's headers.
        cmd += ["-I", str(ROOT / "platform" / "include"),
                "-include", str(ROOT / "compat" / "mwcc_compat.h"),
                "-include", str(ROOT / "platform" / "include" / "port" / "gc_mem.h")]
    cmd += ["-I", str(decomp), "-I", str(decomp / "include")]
    if unit["lang"] == "c":
        cmd.append("-std=gnu99")
    cmd += ["-c", str(src), "-o", str(obj)]

    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
    text = proc.stdout + proc.stderr
    log.write_text(text, encoding="utf-8")

    errors, warnings = [], []
    for line in text.splitlines():
        m = DIAG.match(line)
        if not m:
            continue
        flag = FLAG.search(m["msg"])
        entry = {
            "file": rel(m["file"], decomp),
            "line": int(m["line"]),
            "msg": m["msg"],
            "norm": normalise(m["msg"]),
            "flag": flag.group(1) if flag else None,
        }
        (errors if m["kind"] == "error" else warnings).append(entry)
    return {
        "name": unit["name"],
        "source": unit["source"],
        "group": unit["group"],
        "ok": proc.returncode == 0,
        "errors": errors,
        "portability": [w for w in warnings if w["flag"] in PORTABILITY_WARNINGS],
        "warnings": len(warnings),
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--objects", type=Path, default=ROOT / "build" / "objects.json")
    ap.add_argument("--groups", default="game,library,sdk")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--no-compat", action="store_true")
    ap.add_argument("--only", default=None)
    ap.add_argument("-o", "--out", type=Path, default=ROOT / "build" / "compile_check")
    args = ap.parse_args()

    manifest = json.loads(args.objects.read_text(encoding="utf-8"))
    decomp = Path(manifest["decomp"]).resolve()
    groups = set(args.groups.split(","))
    units = [u for u in manifest["units"] if u["group"] in groups]
    if args.only:
        units = [u for u in units if args.only in u["source"]]

    with ThreadPoolExecutor(args.jobs) as pool:
        results = list(pool.map(lambda u: compile_unit(u, decomp, args.out, not args.no_compat), units))

    ok = sum(r["ok"] for r in results)
    by_msg = Counter()
    by_msg_units = defaultdict(set)
    by_file = Counter()
    first_error = Counter()
    port = Counter()
    port_units = defaultdict(set)
    for r in results:
        seen = set()
        for e in r["errors"]:
            key = e["norm"]
            if key not in seen:
                by_msg[key] += 1
                seen.add(key)
            by_msg_units[key].add(r["name"])
            by_file[e["file"]] += 1
        if r["errors"]:
            first_error[r["errors"][0]["norm"]] += 1
        for w in r["portability"]:
            port[w["flag"]] += 1
            port_units[w["flag"]].add(r["name"])

    header_errors = Counter()
    for r in results:
        for f in {e["file"] for e in r["errors"]}:
            if f.endswith(".h"):
                header_errors[f] += 1

    summary = {
        "units": len(results),
        "ok": ok,
        "failed": len(results) - ok,
        "by_group": {g: [sum(r["ok"] for r in results if r["group"] == g),
                         sum(1 for r in results if r["group"] == g)] for g in sorted(groups)},
        "errors_by_message_units": by_msg.most_common(60),
        "first_error": first_error.most_common(30),
        "headers_breaking_units": header_errors.most_common(30),
        "error_lines_by_file": by_file.most_common(30),
        "portability_warnings": {k: [v, len(port_units[k])] for k, v in port.most_common()},
        "compat": not args.no_compat,
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "report.json").write_text(json.dumps({"summary": summary, "units": results}, indent=1),
                                          encoding="utf-8")

    print(f"{ok}/{len(results)} units compile")
    for g, (good, total) in summary["by_group"].items():
        print(f"  {g:<8} {good}/{total}")
    print("\nmost common errors (units affected):")
    for msg, n in summary["errors_by_message_units"][:25]:
        print(f"  {n:5}  {msg[:110]}")
    print("\nheaders that break units:")
    for f, n in summary["headers_breaking_units"][:12]:
        print(f"  {n:5}  {f}")
    print("\nportability warnings (count, units):")
    for k, (v, u) in summary["portability_warnings"].items():
        print(f"  {k:<32} {v:6} {u:5}")


if __name__ == "__main__":
    main()
