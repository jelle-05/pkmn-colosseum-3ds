#!/usr/bin/env python3
"""Derive field layouts of big-endian data from the original PowerPC code.

The decomp stores many data objects as `u8 lbl_xxx[N] = { ... }` in the
GameCube's big-endian byte order. On the little-endian 3DS every 16-, 32- or
64-bit field read from such an array comes out byte-swapped, so the port has
to know where the multi-byte fields are. The original code says: every load
and store has a width (lbz 1, lhz 2, lwz/lfs 4, lfd 8, psq_l by its GQR).

This tool disassembles the decomp's original objects (the dtk split of the
retail DOL and RELs, with relocations) and follows, within each function,
which registers point into which data symbol:

  lis rA, sym@ha / addi rB, rA, sym@l     rB = &sym
  addi rC, rB, 8                          rC = &sym + 8
  mulli/slwi rI, ...; add rD, rB, rI      rD = &sym + i*stride
  lhz rX, 4(rC)                           field at sym+12, 2 bytes
  lwz rX, sym@sda21(r13)                  field at sym+0, 4 bytes

It also tracks pointers passed to functions (the callee's accesses through
its parameter count for the argument) and pointers kept in a global variable
(a later load of that variable points to the same symbol).

From the accesses it builds a layout per byte-array symbol. Arrays touched
only by byte accesses need nothing. Symbols with conflicting widths on the
same bytes, or with indexed accesses of unknown stride mixed with other
widths, are reported, not guessed.

Usage:
    python tools/access_widths.py [--jobs N]

Output:
    build/access_widths.json             all accesses and the verdicts
    platform/data_types.generated.txt    layouts for tools/unify_prototypes.py
"""

import argparse
import json
import math
import os
import re
import subprocess
import sys
from collections import Counter, defaultdict
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

WIDTH = {}
for m, w in [("lbz", 1), ("lbzu", 1), ("stb", 1), ("stbu", 1),
             ("lhz", 2), ("lhzu", 2), ("lha", 2), ("lhau", 2), ("sth", 2), ("sthu", 2),
             ("lwz", 4), ("lwzu", 4), ("stw", 4), ("stwu", 4),
             ("lfs", 4), ("lfsu", 4), ("stfs", 4), ("stfsu", 4),
             ("lfd", 8), ("lfdu", 8), ("stfd", 8), ("stfdu", 8)]:
    WIDTH[m] = w
XWIDTH = {"lbzx": 1, "stbx": 1, "lhzx": 2, "lhax": 2, "sthx": 2, "lwzx": 4, "stwx": 4,
          "lfsx": 4, "stfsx": 4, "lfdx": 8, "stfdx": 8, "lhbrx": 2, "lwbrx": 4}
STORES = {"stb", "stbu", "sth", "sthu", "stw", "stwu", "stfs", "stfsu", "stfd", "stfdu"}
# GSmathInit sets GQR2..5 = u8, u16, s8, s16; GQR0 is float.
GQR_WIDTH = {0: 4, 1: 4, 2: 1, 3: 2, 4: 1, 5: 2, 6: 4, 7: 4}

INSN = re.compile(r"^\s*([0-9a-f]+):\s+(?:[0-9a-f]{2} ){4}\s*(\S+)\s*(.*)$")
RELOC = re.compile(r"^\s*([0-9a-f]+):\s+(R_PPC_\w+)\s+(\S+?)(?:\+0x([0-9a-f]+))?$")
FUNC = re.compile(r"^[0-9a-f]+ <(.+)>:$")
MEM = re.compile(r"^(-?\d+)\((r\d+)\)$")
VOLATILE = {f"r{i}" for i in [0] + list(range(3, 13))}


SYMLINE = re.compile(r"^([0-9a-f]{8})\s+[lgw! ]+[ dDfFO]*\s+(\S+)\s+([0-9a-f]{8})\s+(\S+)$")
DRELOC = re.compile(r"^([0-9a-f]{8})\s+(R_PPC_ADDR32)\s+(\S+?)(?:\+0x([0-9a-f]+))?$")


def parse_data_pointers(path, objdump):
    """Pointers stored in data: {(container symbol, offset): (target, addend)}."""
    out = subprocess.run([str(objdump), "-r", "-t", str(path)], capture_output=True, text=True).stdout
    syms = defaultdict(list)          # section -> [(start, size, name)]
    section = None
    pointers = {}
    relocs = []
    for line in out.splitlines():
        m = SYMLINE.match(line)
        if m and not m.group(2).startswith("*"):
            syms[m.group(2)].append((int(m.group(1), 16), int(m.group(3), 16), m.group(4)))
            continue
        m = re.match(r"RELOCATION RECORDS FOR \[(\S+)\]", line)
        if m:
            section = m.group(1)
            continue
        m = DRELOC.match(line)
        if m and section and not section.startswith(".text"):
            relocs.append((section, int(m.group(1), 16), m.group(3), int(m.group(4) or "0", 16)))
    for sec, off, target, add in relocs:
        for start, size, name in syms.get(sec, ()):
            if start <= off < start + max(size, 1) and name != sec:
                pointers[(name, off - start)] = (target, add)
                break
    return pointers


def parse_object(path, objdump):
    """-> list of functions: (name, [(addr, mnem, ops, reloc)])"""
    out = subprocess.run([str(objdump), "-dr", "--no-show-raw-insn", str(path)],
                         capture_output=True, text=True).stdout
    funcs, cur, name = [], None, None
    for line in out.splitlines():
        m = FUNC.match(line)
        if m:
            if cur is not None:
                funcs.append((name, cur))
            name, cur = m.group(1), []
            continue
        if cur is None:
            continue
        r = RELOC.match(line)
        if r and cur:
            cur[-1][3] = (r.group(2), r.group(3), int(r.group(4) or "0", 16))
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)\s*(.*)$", line)
        if m and not line.strip().startswith("R_"):
            ops = [o.strip() for o in m.group(3).split("<")[0].split(",") if o.strip()]
            cur.append([int(m.group(1), 16), m.group(2), ops, None])
    if cur is not None:
        funcs.append((name, cur))
    return funcs


class Analysis:
    """Per-function register tracking. A pointer value is
    ("ptr", frozenset of symbols, offset, stride) where stride is None
    (exact), an int (element size of an array walk) or "?" (indexed, size
    unknown). Parameters at entry are ("param", index, offset, stride)."""

    def __init__(self, data_ptrs):
        self.access = defaultdict(list)       # sym -> [(off, width, stride, where)]
        self.param = defaultdict(list)        # func -> [(param, off, width, stride)]
        self.calls = []                       # (callee, param, syms, off, stride, where)
        self.forwards = []                    # (caller, caller_param, off, callee, callee_param)
        self.global_ptrs = defaultdict(set)   # (global sym, off) -> {(sym, off)}
        self.data_ptrs = data_ptrs            # (container, off) -> (target, addend)
        self.table = defaultdict(list)        # container -> [(off, target, addend)]
        for (c, o), (t, a) in data_ptrs.items():
            self.table[c].append((o, t, a))

    def record(self, v, disp, w, fname, copy=False):
        _, syms, off, stride = v
        for s in syms:
            self.access[s].append((off + disp, w, stride, fname + (" [copy]" if copy else "")))

    def load_pointer(self, v, disp):
        """A 4-byte load through pointer v: does it read a pointer from a
        pointer table, and to what?"""
        _, syms, off, stride = v
        targets = set()
        for s in syms:
            if stride is None:
                hit = self.data_ptrs.get((s, off + disp))
                if hit:
                    targets.add(hit)
                for (o2, gs) in [(off + disp, self.global_ptrs.get((s, off + disp)))]:
                    for t in gs or ():
                        targets.add(t)
            else:
                for o, t, a in self.table.get(s, ()):
                    if stride == "?" or (o - (off + disp)) % stride == 0:
                        targets.add((t, a))
        if not targets:
            return None
        adds = {a for _, a in targets}
        if len(adds) != 1:
            return None
        return ("ptr", frozenset(t for t, _ in targets), adds.pop(), None)

    def run_function(self, fname, insns):
        targets, defs, incs = set(), Counter(), defaultdict(set)
        for a, m, ops, rel in insns:
            bm = m.rstrip(".")
            # Branch targets; calls (bl) are not branches within the function.
            if bm.startswith("b") and bm not in ("bl", "bla", "bctrl", "blrl") and ops \
                    and re.fullmatch(r"[0-9a-f]+", ops[-1]):
                targets.add(int(ops[-1], 16))
            # Restoring a saved register in the epilogue is not a new value.
            if bm in ("lwz", "lmw") and len(ops) == 2 and ops[1].endswith("(r1)"):
                continue
            if ops and re.fullmatch(r"r\d+", ops[0]) and not bm.startswith(("cmp", "tw", "mt", "st", "b")):
                defs[ops[0]] += 1
                if bm == "addi" and len(ops) == 3 and ops[1] == ops[0]:
                    try:
                        incs[ops[0]].add(int(ops[2], 0))
                    except ValueError:
                        pass
            if bm.endswith("u") and (bm in WIDTH) and len(ops) == 2:
                mm = MEM.match(ops[1])
                if mm:
                    incs[mm.group(2)].add(int(mm.group(1)))
                    defs[mm.group(2)] += 1
        regs = {f"r{3 + i}": ("param", i, 0, None) for i in range(8)}

        # A load whose register is next used by a store of the same width
        # is a copy (struct assignment), not a field read.
        def is_copy(i, reg, w):
            # MWCC copies structs as a run of loads followed by a run of
            # stores, so look well ahead.
            for a2, m2, ops2, rel2 in insns[i + 1:i + 24]:
                b2 = m2.rstrip(".")
                uses = [o for o in ops2 if o == reg or o.endswith("(" + reg + ")")]
                if not uses:
                    continue
                return b2 in STORES and WIDTH.get(b2) == w and ops2 and ops2[0] == reg
            return False

        def kill(r):
            regs.pop(r, None)

        def at_label():
            kept = {}
            for r, v in regs.items():
                if v[0] not in ("ptr", "param"):
                    continue
                n_inc = len(incs.get(r, ()))
                if defs[r] <= 1:
                    kept[r] = v
                elif n_inc == 1 and defs[r] == 1 + sum(1 for _ in incs[r]) and v[3] is None:
                    step = abs(next(iter(incs[r])))
                    if step:
                        kept[r] = (v[0], v[1], v[2], step)    # a pointer walking an array
            return kept

        for i, (a, m, ops, rel) in enumerate(insns):
            if a in targets:
                regs = at_label()
            bm = m.rstrip(".")
            # ---- address formation
            if bm == "lis" and rel and rel[0] == "R_PPC_ADDR16_HA":
                regs[ops[0]] = ("ha", rel[1], rel[2])
                continue
            # li rD, sym@sda21 is addi rD, r13, sym: a small-data pointer.
            if bm == "li" and len(ops) == 2 and rel and rel[0] == "R_PPC_EMB_SDA21":
                regs[ops[0]] = ("ptr", frozenset([rel[1]]), rel[2], None)
                continue
            if bm == "addis" and len(ops) == 3 and not rel:
                v = regs.get(ops[1])
                try:
                    imm = int(ops[2], 0) << 16
                except ValueError:
                    imm = None
                if v and v[0] in ("ptr", "param") and imm is not None:
                    regs[ops[0]] = (v[0], v[1], v[2] + imm, v[3])
                else:
                    kill(ops[0])
                continue
            if bm in ("addi", "subi") and len(ops) == 3:
                d, s = ops[0], ops[1]
                if rel and rel[0] in ("R_PPC_ADDR16_LO", "R_PPC_EMB_SDA21"):
                    regs[d] = ("ptr", frozenset([rel[1]]), rel[2], None)
                    continue
                v = regs.get(s)
                try:
                    imm = int(ops[2], 0) * (-1 if bm == "subi" else 1)
                except ValueError:
                    kill(d)
                    continue
                if v and v[0] in ("ptr", "param"):
                    regs[d] = (v[0], v[1], v[2] + imm, v[3])
                else:
                    kill(d)
                continue
            if bm == "mr" and len(ops) == 2:
                if ops[1] in regs:
                    regs[ops[0]] = regs[ops[1]]
                else:
                    kill(ops[0])
                continue
            if bm == "mulli" and len(ops) == 3:
                try:
                    regs[ops[0]] = ("idx", int(ops[2], 0))
                except ValueError:
                    kill(ops[0])
                continue
            if bm in ("slwi", "rlwinm", "clrlslwi") and len(ops) >= 3:
                try:
                    if bm == "slwi":
                        regs[ops[0]] = ("idx", 1 << int(ops[2], 0))
                    elif bm == "rlwinm" and len(ops) == 5 and int(ops[3], 0) == 0 \
                            and int(ops[4], 0) == 31 - int(ops[2], 0):
                        regs[ops[0]] = ("idx", 1 << int(ops[2], 0))
                    elif bm == "clrlslwi" and len(ops) == 4:
                        regs[ops[0]] = ("idx", 1 << int(ops[3], 0))
                    else:
                        regs[ops[0]] = ("idx", None)
                except ValueError:
                    kill(ops[0])
                continue
            if bm == "add" and len(ops) == 3:
                x, y = regs.get(ops[1]), regs.get(ops[2])
                ptr, idx = (x, y) if x and x[0] in ("ptr", "param") else (y, x)
                if ptr and ptr[0] in ("ptr", "param"):
                    stride = idx[1] if idx and idx[0] == "idx" and idx[1] else "?"
                    regs[ops[0]] = (ptr[0], ptr[1], ptr[2], stride)
                else:
                    kill(ops[0])
                continue
            # ---- D-form loads and stores
            if bm in WIDTH and len(ops) == 2:
                w = WIDTH[bm]
                mm = MEM.match(ops[1])
                if rel and rel[0] in ("R_PPC_ADDR16_LO", "R_PPC_EMB_SDA21"):
                    direct = ("ptr", frozenset([rel[1]]), rel[2], None)
                    self.record(direct, 0, w, fname)
                    if bm not in STORES:
                        p = self.load_pointer(direct, 0) if w == 4 else None
                        if p:
                            regs[ops[0]] = p
                        else:
                            kill(ops[0])
                    else:
                        v = regs.get(ops[0])
                        if w == 4 and v and v[0] == "ptr" and v[3] is None and len(v[1]) == 1:
                            self.global_ptrs[(rel[1], rel[2])].add((next(iter(v[1])), v[2]))
                    continue
                if mm:
                    disp, rb = int(mm.group(1)), mm.group(2)
                    v = regs.get(rb)
                    loaded = None
                    if v and v[0] == "ptr":
                        copy = bm not in STORES and w >= 4 and is_copy(i, ops[0], w)
                        self.record(v, disp, w, fname, copy)
                        if bm not in STORES and w == 4:
                            loaded = self.load_pointer(v, disp)
                    elif v and v[0] == "param":
                        self.param[fname].append((v[1], v[2] + disp, w, v[3]))
                    if bm.endswith("u") and v and v[0] in ("ptr", "param"):
                        regs[rb] = (v[0], v[1], v[2] + disp, v[3])
                    if bm not in STORES:
                        if loaded:
                            regs[ops[0]] = loaded
                        else:
                            kill(ops[0])
                elif bm not in STORES:
                    kill(ops[0])
                continue
            # ---- X-form (indexed) loads and stores
            if bm in XWIDTH and len(ops) == 3:
                w = XWIDTH[bm]
                loaded = None
                for r_ in (ops[1], ops[2]):
                    v = regs.get(r_)
                    if v and v[0] == "ptr":
                        iv = (v[0], v[1], v[2], "?" if v[3] is None else v[3])
                        self.record(iv, 0, w, fname)
                        if w == 4 and not bm.startswith("st"):
                            loaded = self.load_pointer(iv, 0)
                    elif v and v[0] == "param":
                        self.param[fname].append((v[1], v[2], w, "?"))
                if not bm.startswith("st"):
                    if loaded:
                        regs[ops[0]] = loaded
                    else:
                        kill(ops[0])
                continue
            # ---- paired singles (quantised by GQR)
            if bm in ("psq_l", "psq_lu", "psq_st", "psq_stu") and len(ops) == 4:
                mm = MEM.match(ops[1])
                try:
                    single, gqr = int(ops[2], 0), int(ops[3], 0)
                except ValueError:
                    continue
                w = GQR_WIDTH.get(gqr, 4)
                if mm:
                    disp, rb = int(mm.group(1)), mm.group(2)
                    v = regs.get(rb)
                    for k in range(1 if single else 2):
                        if v and v[0] == "ptr":
                            self.record(v, disp + k * w, w, fname)
                        elif v and v[0] == "param":
                            self.param[fname].append((v[1], v[2] + disp + k * w, w, v[3]))
                continue
            # ---- calls
            if bm == "bl" and rel and rel[0] == "R_PPC_REL24":
                for i in range(8):
                    v = regs.get(f"r{3 + i}")
                    if v and v[0] == "ptr":
                        self.calls.append((rel[1], i, v[1], v[2], v[3], fname))
                    elif v and v[0] == "param":
                        self.forwards.append((fname, v[1], v[2], rel[1], i))
                for r_ in VOLATILE:
                    kill(r_)
                continue
            if bm in ("b", "blr", "bctr", "bctrl", "blrl"):
                if bm in ("bctrl", "blrl"):
                    for r_ in VOLATILE:
                        kill(r_)
                else:
                    regs = {}
                continue
            if ops and re.fullmatch(r"r\d+", ops[0]) and not bm.startswith(("cmp", "tw", "mt", "st", "b")):
                kill(ops[0])


def param_profiles(an):
    """Close the parameter profiles over forwarded pointers (a fixpoint)."""
    prof = {f: set(v) for f, v in an.param.items()}
    for _ in range(8):
        changed = False
        for caller, cp, off, callee, ip in an.forwards:
            for (p, o, w, s) in list(prof.get(callee, ())):
                if p == ip:
                    item = (cp, off + o, w, s)
                    if item not in prof.setdefault(caller, set()):
                        prof[caller].add(item)
                        changed = True
        if not changed:
            break
    return prof


def u8_arrays(decomp):
    """{symbol: size} for every `u8 name[N] = {` definition in the decomp."""
    out = {}
    pat = re.compile(r"^(?:static\s+)?(?:const\s+)?(?:u8|s8|unsigned char|char)\s+(lbl_[0-9A-F]{8}|\w+)\s*\[\s*(\d+|0x[0-9A-Fa-f]+)\s*\]\s*=\s*\{",
                     re.M)
    for p in list((decomp / "src").rglob("*.c")):
        t = p.read_text(encoding="utf-8", errors="replace")
        for m in pat.finditer(t):
            out.setdefault(m.group(1), int(m.group(2), 0))
    return out


SYMTXT = re.compile(r"^(\S+) = (\.\w+):0x([0-9A-Fa-f]+); // type:(\w+)(?: size:0x([0-9A-Fa-f]+))?")


def symbol_addresses(decomp):
    """name -> (space, address, size). The DOL is one address space; each
    REL module's sections are spaces of their own."""
    out = {}
    cfg = decomp / "config" / "GC6E01"
    for path in [cfg / "symbols.txt"] + sorted(cfg.glob("*/symbols.txt")):
        module = "main" if path.parent == cfg else path.parent.name
        for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
            m = SYMTXT.match(line)
            if not m:
                continue
            space = ("main",) if module == "main" else (module, m.group(2))
            out.setdefault(m.group(1), (space, int(m.group(3), 16), int(m.group(5) or "0", 16)))
    return out


def reattribute(access, arrays, addrs):
    """Move accesses that fall outside their symbol (code addressing a
    struct through a neighbouring label) to the array that contains them."""
    import bisect
    by_space = defaultdict(list)
    for name, size in arrays.items():
        if name in addrs:
            space, addr, _ = addrs[name]
            by_space[space].append((addr, size, name))
    for v in by_space.values():
        v.sort()
    starts = {k: [a for a, _, _ in v] for k, v in by_space.items()}
    out = defaultdict(list)
    moved = 0
    for sym, accs in access.items():
        info = addrs.get(sym)
        for (o, w, s, where) in accs:
            size = arrays.get(sym)
            if info and s is None and (size is None or o < 0 or o + w > size):
                space, addr, _ = info
                lst = by_space.get(space, [])
                k = bisect.bisect_right(starts.get(space, []), addr + o) - 1
                if k >= 0:
                    a2, sz2, n2 = lst[k]
                    if a2 <= addr + o and addr + o + w <= a2 + sz2:
                        out[n2].append((addr + o - a2, w, s, where))
                        moved += 1
                        continue
            out[sym].append((o, w, s, where))
    return out, moved


TYPED = {"u16": 2, "s16": 2, "short": 2, "unsigned short": 2, "u32": 4, "s32": 4, "int": 4,
         "unsigned int": 4, "long": 4, "unsigned long": 4, "f32": 4, "float": 4, "f64": 8,
         "double": 8, "u64": 8, "s64": 8}


def typed_arrays(decomp):
    """{symbol: (element size, total size)} for typed array definitions."""
    out = {}
    pat = re.compile(r"^(?:static\s+)?(?:const\s+)?(" + "|".join(sorted(TYPED, key=len, reverse=True)) +
                     r")\s+(\w+)\s*\[\s*(\d+|0x[0-9A-Fa-f]+)\s*\]\s*=\s*\{", re.M)
    for p in (decomp / "src").rglob("*.c"):
        t = p.read_text(encoding="utf-8", errors="replace")
        for m in pat.finditer(t):
            e = TYPED[m.group(1)]
            out.setdefault(m.group(2), (e, e * int(m.group(3), 0)))
    return out


def layout_for(size, accesses):
    """-> (layout or None, reason)"""
    width = [0] * size
    real = [a for a in accesses if not a[3].endswith("[copy]")]
    if not real:
        return None, "only struct-copy accesses (word copies say nothing about fields)"
    accesses = real
    exact = [(o, w) for o, w, s, _ in accesses if s is None]
    strided = [(o, w, s) for o, w, s, _ in accesses if isinstance(s, int)]
    unknown = [(o, w) for o, w, s, _ in accesses if s == "?"]

    outside = 0

    def mark(o, w):
        nonlocal outside
        if o < 0 or o + w > size:
            # Belongs to a neighbouring object that is not a byte array
            # (reattribute() already moved the ones that are).
            outside += 1
            return None
        for k in range(o, o + w):
            if width[k] and width[k] != w:
                return "conflict"
        # A field must start where an equal-width field starts, or on free bytes.
        for k in range(o, o + w):
            width[k] = w
        return None

    for o, w in exact:
        err = mark(o, w)
        if err:
            return None, f"{err} at +{o:#x} ({w} bytes)"
    for o, w, s in strided:
        for k in range(o % s, size - w + 1, s):
            err = mark(k, w)
            if err:
                return None, f"{err} in stride {s} at +{k:#x} ({w} bytes)"
    if unknown:
        ws = {w for _, w in unknown}
        if ws == {1}:
            pass
        elif len(ws) == 1 and size % next(iter(ws)) == 0:
            # A table indexed with an unknown stride: every element has this
            # width, as long as the other accesses agree.
            w = next(iter(ws))
            if any(x and x != w for x in width):
                return None, f"indexed {w}-byte accesses next to fields of other widths"
            for k in range(0, size, w):
                if not width[k]:
                    err = mark(k, w)
                    if err:
                        return None, f"indexed {w}-byte accesses: {err} at +{k:#x}"
        else:
            return None, f"indexed accesses of several widths ({sorted(ws)})"
    # Fields are assumed aligned runs; rebuild them from the width map.
    fields, k = [], 0
    while k < size:
        w = width[k] or 1
        if w > 1 and k % w:
            return None, f"unaligned {w}-byte field at +{k:#x}"
        fields.append(w)
        k += w
    if all(f == 1 for f in fields):
        return "", "byte accesses only"
    return fields, "ok"


def compress(fields):
    out, prev, n = [], None, 0
    names = {1: "b", 2: "h", 4: "w", 8: "d"}
    for f in fields + [None]:
        if f == prev:
            n += 1
            continue
        if prev is not None:
            out.append(names[prev] + (str(n) if n > 1 else ""))
        prev, n = f, 1
    return " ".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()

    manifest = json.loads((ROOT / "build" / "objects.json").read_text(encoding="utf-8"))
    decomp = Path(manifest["decomp"]).resolve()
    objdump = decomp / "build" / "binutils" / "powerpc-eabi-objdump.exe"
    objs = sorted((decomp / "build" / "GC6E01").glob("obj/**/*.o"))
    for mod in (decomp / "build" / "GC6E01").iterdir():
        if (mod / "obj").is_dir():
            objs += sorted((mod / "obj").rglob("*.o"))

    with ThreadPoolExecutor(args.jobs) as pool:
        parsed = list(pool.map(lambda p: parse_object(p, objdump), objs))
        tables = list(pool.map(lambda p: parse_data_pointers(p, objdump), objs))
    functions = [f for fs in parsed for f in fs]
    data_ptrs = {}
    for t in tables:
        data_ptrs.update(t)

    an = Analysis(data_ptrs)
    # Two passes: the first learns which globals hold pointers to data.
    for _ in range(2):
        gp = an.global_ptrs
        an.access.clear(); an.param.clear(); an.calls.clear(); an.forwards.clear()
        an.global_ptrs = defaultdict(set, {k: set(v) for k, v in gp.items()})
        for name, insns in functions:
            an.run_function(name, insns)

    prof = param_profiles(an)
    for callee, p, syms, off, stride, where in an.calls:
        for (pp, o, w, s) in prof.get(callee, ()):
            if pp == p:
                for sym in syms:
                    an.access[sym].append((off + o, w, s if s is not None else stride, f"{where}->{callee}"))

    arrays = u8_arrays(decomp)
    addrs = symbol_addresses(decomp)
    an.access, moved = reattribute(an.access, arrays, addrs)
    print(f"{moved} accesses moved to the array that contains them")
    verdicts = Counter()
    result = {}
    lines = [
        "# Generated by tools/access_widths.py from the original PowerPC code.",
        "# Do not edit; put corrections in platform/data_types.txt, which wins.",
        "",
    ]
    layouts = {}
    passed_to = defaultdict(set)
    for callee, p, syms, off, stride, where in an.calls:
        for s in syms:
            passed_to[s].add(callee)
    for sym, size in sorted(arrays.items()):
        acc = an.access.get(sym, [])
        if not acc:
            verdicts["no accesses found"] += 1
            result[sym] = {"size": size, "verdict": "no accesses found",
                           "passed_to": sorted(passed_to.get(sym, ()))[:6]}
            continue
        fields, reason = layout_for(size, acc)
        if fields is None:
            verdicts["unresolved"] += 1
        elif fields == "":
            verdicts["bytes only"] += 1
        else:
            verdicts["layout"] += 1
            spec = compress(fields)
            layouts.setdefault(spec, []).append(sym)
        result[sym] = {"size": size, "verdict": reason,
                       "accesses": sorted({(o, w, str(s)) for o, w, s, _ in acc})[:40],
                       "from": sorted({x for *_, x in acc})[:8]}
    # Typed arrays (u16/u32/f32 ...) are stored in native order by the
    # compiler, which is only right if the code reads them with the element
    # width. Byte reads of a u32 table, or u16 reads of a u32 table, are not.
    typed = typed_arrays(decomp)
    mismatched = []
    for sym, (elem, size) in sorted(typed.items()):
        acc = [a for a in an.access.get(sym, []) if not a[3].endswith("[copy]") and 0 <= a[0] < size]
        bad = sorted({w for _, w, _, _ in acc if w != elem})
        if bad:
            mismatched.append((sym, elem, size, bad, sorted({x for *_, x in acc})[:4]))
    typed_report = [
        {"symbol": s, "element": e, "size": n, "read_widths": b, "from": f} for s, e, n, b, f in mismatched]

    # Coverage of each generated layout: bytes inside observed multi-byte
    # fields. Unobserved bytes keep their big-endian order.
    for spec, syms in layouts.items():
        for s in syms:
            covered = sum(w for o, w, st, _ in an.access.get(s, []) if w > 1 and st is None)
            result[s]["multibyte_bytes_observed"] = min(covered, result[s]["size"])

    for i, (spec, syms) in enumerate(sorted(layouts.items(), key=lambda kv: -len(kv[1]))):
        lines.append(f"layout gen{i} = {spec}")
        for s in syms:
            lines.append(f"{s} gen{i}")
        lines.append("")

    # Typed arrays read only with narrower widths get a byte layout too:
    # unify_prototypes.py rewrites their values so memory holds the bytes
    # the code expects. Arrays also read with the element width are left
    # alone and reported.
    typed_lines, n = [], 0
    for s, e, size, bad, _ in mismatched:
        acc = [a for a in an.access.get(s, []) if not a[3].endswith("[copy]") and 0 <= a[0] < size]
        if any(w == e for _, w, _, _ in acc):
            continue
        fields, reason = layout_for(size, acc)
        if fields is None:
            continue
        spec = compress(fields) if fields else f"b{size}"
        typed_lines.append(f"layout tgen{n} = {spec}")
        typed_lines.append(f"typed {s} {e} tgen{n}")
        n += 1
    if typed_lines:
        lines += ["# Typed arrays read only with narrower widths (element size given).", ""] + typed_lines
    (ROOT / "platform" / "data_types.generated.txt").write_text("\n".join(lines), encoding="utf-8")
    (ROOT / "build").mkdir(exist_ok=True)
    (ROOT / "build" / "access_widths.json").write_text(json.dumps(result, indent=1), encoding="utf-8")
    (ROOT / "build" / "access_widths_typed.json").write_text(json.dumps(typed_report, indent=1),
                                                             encoding="utf-8")

    total = sum(arrays.values())
    print(f"{len(objs)} objects, {len(functions)} functions")
    print(f"{len(arrays)} u8 arrays in the decomp, {total:,} bytes")
    for k, n in verdicts.most_common():
        b = sum(result[s]["size"] for s in result if (result[s]["verdict"] == k
                or (k == "layout" and result[s]["verdict"] == "ok")
                or (k == "bytes only" and result[s]["verdict"] == "byte accesses only")
                or (k == "unresolved" and result[s]["verdict"] not in
                    ("ok", "byte accesses only", "no accesses found"))))
        print(f"  {n:5}  {k:<20} {b:>9,} bytes")
    print(f"-> platform/data_types.generated.txt ({sum(len(v) for v in layouts.values())} symbols)")
    print(f"typed arrays read with another width: {len(mismatched)}")
    for s, e, n, b, f in mismatched[:12]:
        print(f"    {s} (elements {e} bytes, {n} bytes) read as {b} in {', '.join(f)}")


if __name__ == "__main__":
    main()
