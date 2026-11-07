#!/usr/bin/env python3
"""Link everything into a .3dsx, with stubs for what is still missing.

Takes the missing-symbol list from tools/link_check.py and writes
build/link_probe/stubs.c: every missing function becomes a stub that reports
its name and stops (see platform/src/port_stub.c), and every missing data
object becomes zero-filled storage of the size the decomp's symbols.txt
gives. Then it links the game objects, the stubs and libctru into
build/link_probe/colosseum.elf and converts that to a .3dsx.

The result does not run the game. It proves the build chain works end to
end, and the generated stubs.c is the platform layer's work list, sorted by
where each symbol comes from.

Superseded by tools/link_be.py since the port runs big-endian (docs/BIG_ENDIAN.md): objects are now big-endian and this little-endian link no longer fits. link_be.py uses read_symbols() and c_name() from here.
 (little-endian builds only):
    python tools/link_check.py
    python tools/link_probe.py
"""

import json
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import compile_check  # noqa: E402

BIN = compile_check.DEVKITARM / "bin"
DEVKITPRO = compile_check.DEVKITARM.parent
SYM = re.compile(r"^(\S+) = (\S+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")


def read_symbols(decomp):
    """name -> (type, size) from every symbols.txt (main and the RELs)."""
    out = {}
    for path in sorted((decomp / "config" / "GC6E01").rglob("symbols.txt")):
        module = path.parent.name if path.parent.name != "GC6E01" else "main"
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = SYM.match(line)
            if not m:
                continue
            name, kind, size = m.group(1), m.group(4), int(m.group(5) or "0", 16)
            addr = int(m.group(3), 16)
            align = 8
            while align > 1 and addr % align:
                align //= 2
            out.setdefault(name, (kind, size, align))
            if module != "main" and name in ("_prolog", "_epilog", "_unresolved"):
                out.setdefault(f"{module}_{name}", (kind, size, align))
    return out


def c_name(sym, i):
    return f"__stub_{i}"


def main():
    report = json.loads((ROOT / "build" / "link_check" / "report.json").read_text(encoding="utf-8"))
    manifest = json.loads((ROOT / "build" / "objects.json").read_text(encoding="utf-8"))
    decomp = Path(manifest["decomp"])
    syms = read_symbols(decomp)
    out = ROOT / "build" / "link_probe"
    out.mkdir(parents=True, exist_ok=True)

    def cc(src, obj):
        cmd = [str(BIN / "arm-none-eabi-gcc.exe"), *compile_check.ARCH, "-O1", "-std=gnu99",
               "-fno-common", "-fdata-sections", "-D__3DS__", "-I", str(DEVKITPRO / "libctru" / "include"),
               "-I", str(ROOT / "platform" / "include"), "-c", str(src), "-o", str(obj)]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode:
            sys.exit(p.stdout + p.stderr)

    # Platform sources first: whatever they define must not be stubbed.
    platform_objs = []
    sources = sorted((ROOT / "platform" / "src").rglob("*.c")) + sorted((ROOT / "platform" / "src").rglob("*.S"))
    for src in sources:
        obj = out / "platform" / src.relative_to(ROOT / "platform" / "src").with_suffix(".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        cc(src, obj)
        platform_objs.append(obj)
    provided = set()
    for obj in platform_objs:
        nm = subprocess.run([str(BIN / "arm-none-eabi-nm.exe"), "--defined-only", str(obj)],
                            capture_output=True, text=True).stdout
        provided |= {l.split()[-1] for l in nm.splitlines() if len(l.split()) == 3}

    results = json.loads((ROOT / "build" / "unified" / "compile.json").read_text(encoding="utf-8"))
    objs = [ROOT / "build" / "unified" / "obj" / (r["name"] + ".o") for r in results if r["ok"]]

    # Data the game defines, with the port's sizes. A missing data label that
    # lies inside one of these (dtk split a struct the source keeps whole,
    # e.g. lbl_80478D7B in u8 lbl_80478D78[8]) must name that storage, not
    # get a separate stub.
    from concurrent.futures import ThreadPoolExecutor
    from access_widths import symbol_addresses
    import bisect

    def data_sizes(obj):
        out = subprocess.run([str(BIN / "arm-none-eabi-nm.exe"), "-S", "--defined-only", str(obj)],
                             capture_output=True, text=True).stdout
        return {p[3]: int(p[1], 16) for p in (l.split() for l in out.splitlines())
                if len(p) == 4 and p[2] in "DdBbRr"}
    with ThreadPoolExecutor() as pool:
        game_data = {}
        for d in pool.map(data_sizes, objs):
            game_data.update(d)
    retail = symbol_addresses(decomp)
    spaces = {}
    for name, size in game_data.items():
        if name in retail:
            space, addr, _ = retail[name]
            spaces.setdefault(space, []).append((addr, size, name))
    for v in spaces.values():
        v.sort()
    starts = {k: [a for a, _, _ in v] for k, v in spaces.items()}

    def container(name):
        if name not in retail:
            return None
        space, addr, _ = retail[name]
        k = bisect.bisect_right(starts.get(space, []), addr) - 1
        if k < 0:
            return None
        paddr, psize, parent = spaces[space][k]
        if parent != name and paddr < addr < paddr + psize:
            return parent, addr - paddr
        return None

    aliases = {}
    lines = [
        "/* Generated by tools/link_probe.py. Do not edit. */",
        "extern void __port_stub(const char* name);",
        "",
    ]
    counts = {}
    i = 0
    for group, names in sorted(report["left_by_category"].items()):
        lines.append(f"/* ---- {group}: {len(names)} */")
        for name in sorted(names):
            if name in provided:
                continue                 # implemented in platform/src
            kind, size, align = syms.get(name, ("function", 0, 8))
            i += 1
            label = name.replace("\\", "\\\\").replace('"', '\\"')
            if kind == "function" or (kind not in ("object",) and size == 0):
                lines.append(f'void {c_name(name, i)}(void) __asm__("{label}");')
                lines.append(f'void {c_name(name, i)}(void) {{ __port_stub("{label}"); }}')
            elif container(name):
                aliases[name] = container(name)
                lines.append(f"/* {name} = {aliases[name][0]} + {aliases[name][1]:#x} (linker script) */")
            else:
                lines.append(f'unsigned char {c_name(name, i)}[{max(size, 1)}] '
                             f'__asm__("{label}") __attribute__((aligned({align})));')
            counts[group] = counts.get(group, 0) + 1
        lines.append("")
    stubs = out / "stubs.c"
    stubs.write_text("\n".join(lines) + "\n", encoding="utf-8")

    cc(stubs, out / "stubs.o")
    alias_file = out / "aliases.txt"
    alias_file.write_text("".join(f"{n} {p} {d}\n" for n, (p, d) in sorted(aliases.items())), encoding="utf-8")

    # The probe allows the known duplicates, so a platform symbol that the
    # game also defines would be resolved silently. Refuse that.
    def defined(obj):
        out = subprocess.run([str(BIN / "arm-none-eabi-nm.exe"), "--defined-only", str(obj)],
                             capture_output=True, text=True).stdout
        return {l.split()[-1] for l in out.splitlines() if len(l.split()) == 3 and l.split()[1] in "TDBRS"}
    with ThreadPoolExecutor() as pool:
        game_defined = set().union(*pool.map(defined, objs))
    strong_platform = set().union(*(defined(o) for o in platform_objs))
    clash = sorted(strong_platform & game_defined)
    if clash:
        sys.exit("platform/src defines symbols the game also defines: " + ", ".join(clash))
    rsp = out / "objects.rsp"
    rsp.write_text("\n".join(o.as_posix() for o in objs + [out / "stubs.o"] + platform_objs) + "\n",
                   encoding="utf-8")
    elf = out / "colosseum.elf"
    dup = report.get("duplicates", {})
    # Data at its GameCube offsets (game code reaches neighbours through
    # one symbol's address); the generated specs use the generated script.
    specs = out / "port.specs"
    p = subprocess.run([sys.executable, str(ROOT / "tools" / "gen_layout.py"), str(rsp),
                        str(out / "colosseum.ld"), str(specs), str(alias_file)],
                       capture_output=True, text=True)
    print(p.stdout.strip())
    if p.returncode:
        sys.exit(p.stderr)
    cmd = [str(BIN / "arm-none-eabi-gcc.exe"), f"-specs={specs.as_posix()}", *compile_check.ARCH,
           f"@{rsp.as_posix()}", "-L", str(DEVKITPRO / "libctru" / "lib"), "-lcitro3d", "-lctru", "-lm",
           f"-Wl,-Map,{(out / 'colosseum.map').as_posix()}", "-o", str(elf)]
    if dup:
        # Known duplicates are listed by link_check; keep the first copy for
        # now rather than hiding the rest of the result.
        cmd.insert(-2, "-Wl,--allow-multiple-definition")
    p = subprocess.run(cmd, capture_output=True, text=True)
    (out / "link.log").write_text(p.stdout + p.stderr, encoding="utf-8")
    undefined = sorted(set(re.findall(r"undefined reference to [`']([^'`]+)'", p.stdout + p.stderr)))
    if p.returncode:
        print("link failed; build/link_probe/link.log")
        print(f"{len(undefined)} undefined symbols, e.g. {undefined[:15]}")
        print((p.stdout + p.stderr)[-1200:])
        sys.exit(1)

    tdsx = DEVKITPRO / "tools" / "bin" / "3dsxtool.exe"
    target = out / "colosseum.3dsx"
    p2 = subprocess.run([str(tdsx), str(elf), str(target)], capture_output=True, text=True)
    if p2.returncode:
        sys.exit(p2.stdout + p2.stderr)

    size = subprocess.run([str(BIN / "arm-none-eabi-size.exe"), str(elf)], capture_output=True, text=True).stdout
    print(f"linked {len(objs)} game objects and {i} stubs")
    for g, n in sorted(counts.items(), key=lambda kv: -kv[1]):
        print(f"  {n:5} stubs  {g}")
    if dup:
        print(f"  allowed {len(dup)} duplicate definition(s): {', '.join(sorted(dup))}")
    print(size.strip())
    print(f"-> {target} ({target.stat().st_size:,} bytes)")


if __name__ == "__main__":
    main()
