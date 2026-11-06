#!/usr/bin/env python3
"""Generate the port's object list from the decomp.

The decomp builds 2,256 units, and the list lives in its configure.py.
About 880 of those units are small wrapper files that #define a range and
#include a shared .c file. Taken together the wrappers cover every function
exactly once, so the port has to use the same list. Do not copy it by hand.

This script reads decomp/objdiff.json, which the decomp's configure.py
writes, and sorts every unit into one of these groups:

  game       game code we compile (src/game, src/rel)
  library    third-party code we compile and adapt (src/hsd, src/musyx)
  replaced   SDK, runtime and debugger code the platform layer replaces
  generated  units built from .json by a decomp tool; these need their own path
  skipped    assembly units

Usage:
    python tools/gen_objects.py [--decomp DIR] [-o build/objects.json]

objdiff.json only appears after the decomp's first `ninja` run, because it
depends on dtk's split of the original files. Either build the submodule
(it needs orig/GC6E01 like any decomp checkout), or point --decomp at a
checkout that has been built at the same commit.
"""

import argparse
import json
import sys
from collections import Counter
from pathlib import Path

REPLACED_PREFIXES = ("src/dolphin/", "src/crt", "src/trk/")
LIBRARY_PREFIXES = ("src/hsd/", "src/musyx/")
GAME_PREFIXES = ("src/game/", "src/rel/")


def classify(source):
    if source is None:
        return "skipped"
    if source.endswith(".json"):
        return "generated"
    if source.endswith(".s"):
        return "skipped"
    if source.startswith(REPLACED_PREFIXES) or "__init_cpp_exceptions" in source:
        return "replaced"
    if source.startswith(LIBRARY_PREFIXES):
        return "library"
    if source.startswith(GAME_PREFIXES):
        return "game"
    return "unknown"


def main():
    root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--decomp", type=Path, default=root / "decomp")
    parser.add_argument("-o", "--output", type=Path, default=root / "build" / "objects.json")
    args = parser.parse_args()

    objdiff = args.decomp / "objdiff.json"
    if not objdiff.is_file():
        sys.exit(f"{objdiff} not found; build the decomp first, or pass --decomp "
                 "with a built checkout at the same commit")

    units = json.loads(objdiff.read_text(encoding="utf-8"))["units"]
    out = {"decomp": str(args.decomp), "units": []}
    groups = Counter()
    modules = Counter()
    for unit in units:
        meta = unit.get("metadata", {})
        source = meta.get("source_path")
        group = classify(source)
        module = unit["name"].split("/", 1)[0]
        groups[group] += 1
        modules[module] += 1
        out["units"].append({
            "name": unit["name"],
            "module": module,
            "source": source,
            "group": group,
            "lang": "c++" if source and source.endswith(".cpp") else "c",
        })

    if groups["unknown"]:
        bad = [u["source"] for u in out["units"] if u["group"] == "unknown"]
        sys.exit("unclassified sources: " + ", ".join(bad[:10]))

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(out, indent=1) + "\n", encoding="utf-8")

    print(f"{len(units)} units -> {args.output}")
    for group in ("game", "library", "replaced", "generated", "skipped"):
        print(f"  {group:<10} {groups[group]}")
    print(f"  modules    {len(modules)} (main + {len(modules) - 1} RELs)")


if __name__ == "__main__":
    main()
