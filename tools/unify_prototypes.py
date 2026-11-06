#!/usr/bin/env python3
"""Let conflicting function declarations coexist, the way MWCC allowed.

The decomp declares many functions differently in different places. One
function has eight variants: u32, void*, int or s32 returns, with and without
parameters. MWCC accepted that, and the matching build proves that each call
site's own view is what the original binary does: callers pass the number of
arguments they declare and treat the result as the type they declare.

GCC rejects two different declarations of one function in a translation unit.
Making every declaration agree is not an option either. A canonical prototype
breaks hundreds of call sites (wrong argument counts, values read from void
functions, typed pointer returns), and editing the decomp would change MWCC's
code generation and break the matching build.

So this tool keeps each view and makes it legal. Working on preprocessed text
(gcc -E always succeeds), it gives every additional view of a function in a
unit its own name, bound to the real symbol with an assembler label:

    extern u8 f(void);              ->  extern u8 f__pv1(void) __asm__("f");
    ... f() ...                     ->  ... f__pv1() ...

Two adjustments keep the original semantics on ARM:

  narrowing   a view with a narrower integer return than the definition
              (u8 against u32). ARM's ABI lets GCC assume the callee already
              extended the value, so GCC never masks it. The call is
              rewritten to `((u8)f__pv1__raw(...))`, where the raw view
              returns the definition's type. Measured against the original
              objects: MWCC masked or extended r3 right after the call at
              1,796 of 2,776 such sites, and used it raw at 843. The cast
              matches the majority; at the raw sites the two only differ
              when the callee returns a value outside the narrow type.
  float-abi   an unprototyped view `f()` of a function that takes floats.
              Old-style calls promote float to double. On the GameCube both
              travel in the same FPR, on hard-float ARM they do not, so the
              view gets the definition's parameter list.

Usage:
    python tools/unify_prototypes.py [--jobs N] [--compile]

Output goes to build/unified/: src/<unit>.i (rewritten units),
summary.json, and with --compile, compile.json.
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

KEYWORDS = {
    "return", "else", "case", "goto", "sizeof", "do", "if", "while", "for",
    "switch", "default", "break", "continue", "typedef",
}
BUILTIN_TYPE_WORDS = {
    "void", "char", "short", "int", "long", "float", "double", "signed",
    "unsigned", "struct", "union", "enum", "const", "volatile", "extern",
    "static", "inline", "__inline__", "__inline", "register", "_Bool",
}
INT_SIZES = {
    "char": 1, "signed char": 1, "unsigned char": 1,
    "short": 2, "unsigned short": 2, "signed short": 2, "short int": 2,
    "unsigned short int": 2, "short unsigned int": 2,
    "int": 4, "unsigned int": 4, "signed int": 4, "long": 4, "unsigned long": 4,
    "signed long": 4, "long int": 4, "unsigned long int": 4, "long unsigned int": 4,
    "unsigned": 4, "signed": 4,
    "long long": 8, "unsigned long long": 8, "signed long long": 8,
}
FLOATS = {"float", "double", "long double"}

DECL = re.compile(
    r"^(?P<ret>(?:[A-Za-z_]\w*[\s\*]+)+?)(?P<name>[A-Za-z_]\w*)\s*"
    r"\((?P<params>[^()]*(?:\([^()]*\)[^()]*)*)\)\s*(?P<attr>(?:__attribute__\s*\(\(.*\)\)\s*)*)$",
    re.S)
TYPEDEF = re.compile(r"\btypedef\s+([^;{}()]+?)\s+(\w+)\s*;")
IDENT = re.compile(r"[A-Za-z_]\w*")
STORAGE = re.compile(r"\b(extern|static|inline|__inline__|__inline|register|const|volatile)\b")


def clean(s):
    return re.sub(r"\s+", " ", s).strip()


# ---------------------------------------------------------------- lexing

TOKEN = re.compile(r"""
    (?P<marker>^\#[^\n]*)          |
    (?P<str>"(?:\\.|[^"\\\n])*")   |
    (?P<chr>'(?:\\.|[^'\\\n])*')   |
    (?P<id>[A-Za-z_]\w*)           |
    (?P<num>\.?\d[\w.]*(?:[eEpP][+-]\w*)?) |
    (?P<arrow>->)                  |
    (?P<p>[{}();,.])               |
    (?P<ws>\s+)                    |
    (?P<other>.)
""", re.X | re.M | re.S)


def tokens(text):
    for m in TOKEN.finditer(text):
        kind = m.lastgroup
        if kind in ("ws", "marker"):
            continue
        yield kind, m.group(kind), m.start(), m.end()


# ---------------------------------------------------------------- types

def typedef_table(text):
    out = {}
    for m in TYPEDEF.finditer(text):
        out[m.group(2)] = clean(STORAGE.sub(" ", m.group(1)))
    for k in list(out):
        seen = set()
        while out.get(k) in out and out[k] not in seen:
            seen.add(out[k])
            out[k] = out[out[k]]
    return out


def split_params(p):
    out, depth, cur = [], 0, ""
    for ch in p:
        if ch == "(":
            depth += 1
        elif ch == ")":
            depth -= 1
        if ch == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += ch
    if cur.strip():
        out.append(cur)
    return [clean(x) for x in out]


def classify(t, tds):
    """ABI class of a declared type: ('int', size), ('float', name),
    ('ptr',), ('void',), ('...',) or None for anything else."""
    t = clean(STORAGE.sub(" ", t))
    if t == "...":
        return ("...",)
    if "*" in t or "[" in t or "(" in t:
        return ("ptr",)
    words = t.split()
    if not words:
        return None
    # Strip a trailing parameter name.
    if len(words) > 1 and words[-1] not in INT_SIZES and words[-1] not in FLOATS \
            and words[-1] not in ("int", "char", "short", "long", "double", "float", "unsigned", "signed"):
        t = " ".join(words[:-1])
    t = tds.get(t, t)
    t = clean(STORAGE.sub(" ", t))
    if "*" in t:
        return ("ptr",)
    if t == "void":
        return ("void",)
    if t in INT_SIZES:
        return ("int", INT_SIZES[t], t)
    if t in FLOATS:
        return ("float", t)
    if t.startswith("enum "):
        return ("int", 4, "int")
    return None


PP_LINE = re.compile(r"(?m)^\s*#.*$")


def parse_head(head, tds, block_scope):
    # Line markers and #pragma lines survive preprocessing; they are not
    # part of the declaration.
    h = clean(PP_LINE.sub(" ", head))
    if not h or "=" in h:
        return None
    m = DECL.match(h)
    if not m:
        return None
    ret = clean(m.group("ret"))
    words = ret.replace("*", " ").split()
    if not words or any(w in KEYWORDS for w in words):
        return None
    if block_scope and words[0] not in BUILTIN_TYPE_WORDS and words[0] not in tds:
        return None   # an expression statement such as `a * f(x);`
    params = split_params(m.group("params"))
    return {
        "name": m.group("name"),
        "ret": ret,
        "params": params,
        "proto": not (len(params) == 0),
        "static": "static" in words,
        "head": h,
        "attr": m.group("attr") or "",
    }


def ret_class(d, tds):
    return classify(re.sub(r"\b(extern|static|inline|__inline__|__inline)\b", " ", d["ret"]), tds)


def view_key(d):
    # Storage and function specifiers do not change the type.
    ret = re.sub(r"\b(extern|inline|__inline__|__inline)\b", " ", d["ret"])
    return clean(ret) + "|" + ",".join(
        clean(re.sub(r"\b\w+$", "", p)) if " " in p.strip() and "*" not in p.split()[-1] else p
        for p in d["params"])


# ---------------------------------------------------------------- statements

def statements(text, with_top=False):
    """Yield (start, end, head, depth, kind) for declarations and definition
    heads. kind is 'decl' (ends with ;) or 'def' (head before a {). With
    with_top, also yield the start of the enclosing top-level head."""
    depth = paren = 0
    start = 0
    top = 0
    last_paren_start = 0    # last top-level statement with a '(' in it
    for kind, val, s, e in tokens(text):
        if kind != "p":
            continue
        if val == "(":
            if depth == 0 and paren == 0:
                last_paren_start = start
            paren += 1
        elif val == ")":
            paren -= 1
        elif paren == 0 and val == "{":
            if depth == 0:
                # K&R definitions put `type param;` lines between the
                # declarator and the body; the function starts at the
                # declarator.
                top = start if "(" in text[start:s] else last_paren_start
            item = (start, s, text[start:s], depth, "def")
            yield item + ((top,) if with_top else ())
            depth += 1
            start = e
        elif paren == 0 and val == "}":
            depth -= 1
            start = e
        elif paren == 0 and val == ";":
            item = (start, e, text[start:s], depth, "decl")
            yield item + ((top if depth else start,) if with_top else ())
            start = e


def strip_extern_c(text):
    """Remove `extern "C" { ... }` wrappers. A few units are C files that
    MWCC compiled as C++ only to get C++'s .bss ordering."""
    while True:
        m = re.search(r'extern\s+"C"\s*\{', text)
        if not m:
            return text
        depth, i = 1, m.end()
        for kind, val, s, e in tokens(text[m.end():]):
            if val == "{":
                depth += 1
            elif val == "}":
                depth -= 1
                if depth == 0:
                    i = m.end() + s
                    break
        text = text[:m.start()] + " " * (m.end() - m.start()) + text[m.end():i] + " " + text[i + 1:]


# ---------------------------------------------------------------- passes

def preprocess(unit, decomp, out_dir):
    out = out_dir / "pp" / (unit["name"] + ".i")
    out.parent.mkdir(parents=True, exist_ok=True)
    cc = compile_check.DEVKITARM / "bin" / "arm-none-eabi-gcc.exe"
    cmd = [str(cc), *compile_check.ARCH, *compile_check.SEMANTICS, "-E", "-std=gnu99",
           "-DBUILD_VERSION=0", "-DVERSION_GC6E01", "-DNDEBUG=1", "-DPORT=1", "-D__3DS__",
           *("-D" + d for d in unit.get("defines", [])),
           "-I", str(ROOT / "platform" / "include"),
           "-include", str(ROOT / "compat" / "mwcc_compat.h"),
           "-include", str(ROOT / "platform" / "include" / "port" / "gc_mem.h"),
           "-I", str(decomp), "-I", str(decomp / "include"),
           str(decomp / unit["source"]), "-o", str(out)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode != 0:
        return unit, None
    return unit, out.read_text(encoding="utf-8", errors="replace")


def collect_definitions(pp):
    """name -> (ret class, [param classes], prototyped, params text)"""
    defs, clash = {}, set()
    for u, text in pp:
        tds = typedef_table(text)
        for s, e, head, depth, kind in statements(text):
            if kind != "def" or depth != 0:
                continue
            d = parse_head(head, tds, False)
            if not d or d["static"]:
                continue
            rc = ret_class(d, tds)
            pcs = [classify(p, tds) for p in d["params"] if p not in ("void", "")]
            info = (rc, pcs, d["proto"])
            if d["name"] in defs and defs[d["name"]][:2] != info[:2]:
                clash.add(d["name"])
            defs.setdefault(d["name"], info)
    for k in clash:
        defs.pop(k, None)
    return defs, clash


def abi_text(c):
    if c is None:
        return None
    if c[0] == "ptr":
        return "void*"
    if c[0] == "void":
        return "void"
    if c[0] == "int":
        return c[2]
    if c[0] == "float":
        return c[1]
    if c[0] == "...":
        return "..."
    return None


OBJ = re.compile(
    r"^(?P<ret>extern\s+(?:[A-Za-z_]\w*[\s\*]+)+?)(?P<name>[A-Za-z_]\w*)\s*"
    r"(?P<arr>(?:\[[^\]]*\]\s*)*)(?P<attr>(?:__attribute__\s*\(\(.*\)\)\s*)*)$", re.S)


def parse_object(head):
    """An `extern T name[...];` object declaration."""
    h = clean(PP_LINE.sub(" ", head))
    if not h or "=" in h or "(" in h.split("__attribute__")[0]:
        return None
    m = OBJ.match(h)
    if not m:
        return None
    words = m.group("ret").replace("*", " ").split()
    if any(w in KEYWORDS for w in words):
        return None
    return {"name": m.group("name"), "ret": clean(m.group("ret")), "arr": clean(m.group("arr")),
            "attr": m.group("attr") or "", "head": h}


OBJ_DEF = re.compile(r"^(?:(?:const|volatile|static)\s+)*(?:[A-Za-z_]\w*[\s\*]+)+?([A-Za-z_]\w*)\s*(?:\[[^\]]*\]\s*)*(?:__attribute__\s*\(\(.*\)\)\s*)*(?:=.*)?$", re.S)


def rewrite_unit(u, text, defs, stats):
    tds = typedef_table(text)
    # 1. Find declarations and definitions.
    # MWCC lets a block-scope extern declaration stay visible for the rest
    # of the file, as pre-standard compilers did. C limits it to the block.
    # Such declarations are hoisted to just before the enclosing top-level
    # definition, which gives GCC the same visibility. `hoist` maps a
    # declaration's start to that insertion point.
    decls = []          # (start, end, name, view, depth, d, kind)
    hoist = {}
    static_defs = set()
    obj_defs = set()
    # Where each typedef becomes visible at file scope. A declaration that
    # uses a type declared inside a function cannot be hoisted.
    file_typedef_at = {}
    type_names = set(tds)
    pending = {}            # depth -> True after `typedef struct ... {`
    for s, e, head, depth, kind in statements(text):
        h = clean(PP_LINE.sub(" ", head))
        if kind == "def":
            if h.startswith("typedef"):
                pending[depth] = True
            continue
        if pending.pop(depth, False):
            # `} Name, *PName;` closes a braced typedef.
            for name in IDENT.findall(h):
                type_names.add(name)
                if depth == 0:
                    file_typedef_at.setdefault(name, s)
            continue
        m = TYPEDEF.search(h + ";")
        if m:
            type_names.add(m.group(2))
            if depth == 0:
                file_typedef_at.setdefault(m.group(2), s)

    def hoistable(head, top):
        for ident in IDENT.findall(PP_LINE.sub(" ", head)):
            if ident in type_names and file_typedef_at.get(ident, 1 << 60) > top:
                return False
        return True

    decl_depth = {}
    for s, e, head, depth, kind, top in statements(text, with_top=True):
        d = parse_head(head, tds, depth > 0)
        if d:
            if kind == "def" and depth != 0:
                continue
            if d["static"]:
                if kind == "def":
                    static_defs.add(d["name"])
                continue
            eff = depth
            if depth and hoistable(head, top):
                hoist[s] = top
                eff = 0
            decls.append((s, e, d["name"], view_key(d), eff, d, kind))
            continue
        if kind == "def" and depth == 0:
            h = clean(PP_LINE.sub(" ", head))
            if h.endswith("=") and not h.startswith(("typedef", "extern")):
                m = OBJ_DEF.match(h[:-1].strip())
                if m:
                    obj_defs.add(m.group(1))   # object with an initializer
            continue
        if kind == "decl":
            o = parse_object(head)
            if o:
                o["object"] = True
                eff = depth
                if depth and hoistable(head, top):
                    hoist[s] = top
                    eff = 0
                decls.append((s, e, o["name"], o["head"], eff, o, "decl"))
            elif depth == 0:
                h = clean(PP_LINE.sub(" ", head))
                if h and not h.startswith(("typedef", "extern", "struct", "union", "enum")):
                    m = OBJ_DEF.match(h)
                    if m:
                        obj_defs.add(m.group(1))

    by_name = defaultdict(list)
    for x in decls:
        by_name[x[2]].append(x)

    # 2. Aliases. Every declaration that differs from the primary view gets
    # its own alias. Block-scope declarations always do, unless they repeat
    # a file-scope primary: types declared inside a block (struct tags) make
    # textually equal declarations distinct to GCC.
    alias_of = {}       # decl start -> alias name
    primary_of = {}
    for name, xs in by_name.items():
        defined = [x for x in xs if x[6] == "def"]
        filescope = [x for x in xs if x[6] == "decl" and x[4] == 0]
        is_obj = any(x[5].get("object") for x in xs)
        if defined:
            primary, pdepth = defined[0][3], 0
        elif is_obj and name in obj_defs:
            primary, pdepth = None, 0        # defined here: alias every extern view
        elif filescope:
            primary, pdepth = filescope[0][3], 0
        else:
            primary, pdepth = xs[0][3], xs[0][4]
        primary_of[name] = primary
        n = 0
        shared = {}
        for x in xs:
            if x[6] != "decl" or name in static_defs:
                continue
            if x[3] == primary and (x[4] == 0 or pdepth != 0 or x is xs[0]):
                continue
            if x[4] == 0 and x[3] in shared:
                alias_of[x[0]] = shared[x[3]]
                continue
            n += 1
            a = f"{name}__pv{n}"
            alias_of[x[0]] = a
            if x[4] == 0:
                shared[x[3]] = a
        if n:
            stats["aliased_names"] += 1

    # 3. Classify each declaration for the ARM adjustments.
    plan = {}           # decl start -> dict(new decl text, call wrapper)
    for (s, e, name, view, depth, d, kind) in decls:
        if kind != "decl":
            continue
        if name in static_defs:
            # MWCC let `extern f` precede `static f() {}`; C does not.
            body = re.sub(r"\bextern\b", " ", d["head"])
            plan[s] = {"end": e, "text": "static " + clean(body) + ";", "name": name,
                       "target": name, "raw": name, "cast": None}
            stats["static_fixed"] += 1
            continue
        if d.get("object"):
            target = alias_of.get(s, name)
            if target != name:
                plan[s] = {"end": e, "name": name, "target": target, "raw": target, "cast": None,
                           "text": f"{d['ret']} {target}{d['arr']} {d['attr']}__asm__(\"{name}\");"}
            continue
        target = alias_of.get(s, name)
        info = defs.get(name)
        ret = re.sub(r"\bextern\b", " ", d["ret"])
        params = ", ".join(d["params"]) if d["params"] else ""
        cast = None
        if info:
            drc, dpcs, dproto = info
            mrc = ret_class(d, tds)
            if mrc and drc and mrc[0] == "int" and drc[0] == "int" and mrc[1] < drc[1]:
                cast = clean(ret)
                ret = abi_text(drc)
                stats["narrowing"][name].add(u["source"])
            needs_float = (drc and drc[0] == "float") or any(p and p[0] == "float" for p in dpcs)
            if needs_float and not d["proto"] and dproto and all(abi_text(p) for p in dpcs):
                params = ", ".join(abi_text(p) for p in dpcs) or "void"
                stats["float_abi"][name].add(u["source"])
            elif needs_float and not d["proto"]:
                stats["float_unfixed"][name].add(u["source"])
        if target == name and cast is None and params == (", ".join(d["params"]) if d["params"] else ""):
            continue
        raw = target + "__raw" if cast else target
        if params == "" and not d["params"]:
            ptxt = ""
        else:
            ptxt = params
        new = f"extern {clean(ret)} {raw}({ptxt}) {d['attr']}__asm__(\"{name}\");"
        plan[s] = {"end": e, "text": new, "name": name, "target": target, "raw": raw, "cast": cast}

    # Hoisted declarations: blank them in place, insert before their function.
    inserts = defaultdict(list)
    # MWCC gives out-of-line copies of plain `inline` functions shared
    # linkage; with -fgnu89-inline every unit emits one. GCC ignores `weak`
    # on an inline function, so drop the `inline` and make the definition
    # weak: the linker keeps one copy and a strong definition still wins.
    # (`extern inline` is inline-only in gnu89 and emits nothing.)
    drop = set()
    for (s, e, name, view, depth, d, kind) in decls:
        if kind != "def" or re.search(r"\bextern\b", d["ret"]):
            continue
        if not re.search(r"\b(inline|__inline__|__inline)\b", d["ret"]):
            continue
        for kind_, val, ts, te in tokens(text[s:e]):
            if val in ("inline", "__inline__", "__inline"):
                drop.add(s + ts)
        inserts[s].append("__attribute__((weak))")
        stats["weak_inline"] += 1
    for (s, e, name, view, depth, d, kind) in decls:
        if s not in hoist:
            continue
        p = plan.get(s) or {"end": e, "name": name, "target": name, "raw": name, "cast": None,
                            "text": clean(PP_LINE.sub(" ", text[s:e]))}
        new = p["text"]
        if name in static_defs:
            new = ""            # the static definition follows; keep it local
        inserts[hoist[s]].append(new)
        p = dict(p, text="")
        plan[s] = p
        stats["hoisted"] += 1

    if not plan and not alias_of and not inserts:
        return text

    # 4. Walk tokens, keep track of the visible declaration per name, rename
    # uses. A use with no visible declaration is an implicit declaration
    # (int f()); give it its own view so a later declaration cannot clash.
    events = sorted(((hoist.get(x[0], x[1]), x[2], x[0], x[4] == 0) for x in decls
                     if x[6] == "decl"), key=lambda t: t[0])
    ins_points = sorted(inserts)
    ii = 0
    def_events = sorted(((x[1], x[2]) for x in decls if x[6] == "def"), key=lambda t: t[0])
    implicit = set()
    # Ranges of each declaration or definition head, so the declared name
    # itself is never treated as a use.
    own = set()
    for x in decls:
        m = re.search(r"\b%s\b" % re.escape(x[2]), text[x[0]:x[1]])
        if m:
            own.add(x[0] + m.start())
    out = []
    pos = 0
    scope = [{}]
    ev = 0
    dv = 0
    toks = list(tokens(text))
    i = 0
    starts = sorted(plan)
    pi = 0
    while i < len(toks):
        kind, val, s, e = toks[i]
        while ii < len(ins_points) and ins_points[ii] <= s:
            at = ins_points[ii]
            if at >= pos:
                out.append(text[pos:at])
                pos = at
            out.append(" ".join(t for t in inserts[at] if t) + " ")
            ii += 1
        while ev < len(events) and events[ev][0] <= s:
            end, name, start, filescope = events[ev]
            # File-scope (and hoisted) declarations are visible in all open
            # scopes; the rest only in the current block.
            for sc in (scope if filescope else scope[-1:]):
                sc[name] = start
            ev += 1
        while dv < len(def_events) and def_events[dv][0] <= s:
            scope[0][def_events[dv][1]] = "def"
            for sc in scope:
                sc[def_events[dv][1]] = sc.get(def_events[dv][1], "def")
            dv += 1
        # A statement's start may sit on whitespace or a line marker, so
        # match the first token at or after it.
        while pi < len(starts) and plan[starts[pi]]["end"] <= s:
            pi += 1
        if pi < len(starts) and starts[pi] <= s < plan[starts[pi]]["end"]:
            p = plan[starts[pi]]
            pi += 1
            out.append(text[pos:s])
            # Keep the declaration's own line count.
            out.append(p["text"] + "\n" * text[s:p["end"]].count("\n"))
            pos = p["end"]
            while i < len(toks) and toks[i][2] < p["end"]:
                i += 1
            continue
        if s in drop:
            out.append(text[pos:s])
            pos = e
            i += 1
            continue
        if kind == "p" and val == "{":
            scope.append(dict(scope[-1]))
        elif kind == "p" and val == "}":
            if len(scope) > 1:
                scope.pop()
        elif kind == "id" and val in by_name:
            prev = toks[i - 1][1] if i else ""
            if prev not in (".", "->") and s not in own:
                start = scope[-1].get(val)
                if start is None and i + 1 < len(toks) and toks[i + 1][1] == "(" \
                        and val not in static_defs and not any(x[5].get("object") for x in by_name[val]):
                    implicit.add(val)
                    out.append(text[pos:s])
                    out.append(f"{val}__pvi")
                    pos = e
                    stats["implicit_views"] += 1
                    i += 1
                    continue
                if start is not None and start != "def":
                    target = alias_of.get(start, val)
                    pl = plan.get(start)
                    if pl and pl["cast"] and i + 1 < len(toks) and toks[i + 1][1] == "(":
                        # Find the matching ')' and wrap the call in a cast.
                        depth = 0
                        j = i + 1
                        while j < len(toks):
                            if toks[j][1] == "(":
                                depth += 1
                            elif toks[j][1] == ")":
                                depth -= 1
                                if depth == 0:
                                    break
                            j += 1
                        if j < len(toks):
                            out.append(text[pos:s])
                            out.append(f"(({pl['cast']}){pl['raw']}")
                            out.append(text[e:toks[j][3]])
                            out.append(")")
                            pos = toks[j][3]
                            i = j + 1
                            stats["calls_cast"] += 1
                            continue
                    if target != val:
                        out.append(text[pos:s])
                        out.append(target)
                        pos = e
                        stats["uses_renamed"] += 1
        i += 1
    out.append(text[pos:])
    prelude = "".join(f'extern int {n}__pvi() __asm__("{n}");\n' for n in sorted(implicit))
    return prelude + "".join(out)


GC_HEX = r"0x[0-9A-Fa-f]{8}"
CAST_PTR = r"\(\s*(?:(?:const|volatile)\s+)*[A-Za-z_]\w*(?:\s+[A-Za-z_]\w*)*\s*\*+\s*\)\s*"
FIFO_WRITE = re.compile(r"\*\s*\(\s*volatile\s+(u8|s8|u16|s16|u32|s32|f32|unsigned char|unsigned short|"
                        r"unsigned long|float)\s*\*\s*\)\s*0x[cC][cC]008000\s*=")
CAST_CONST = re.compile("(" + CAST_PTR + ")(" + GC_HEX + r")\b")
CAST_EXPR = re.compile("(" + CAST_PTR + r")\(")
FIFO_FN = {"u8": "U8", "s8": "U8", "unsigned char": "U8", "u16": "U16", "s16": "U16",
           "unsigned short": "U16", "u32": "U32", "s32": "U32", "unsigned long": "U32",
           "f32": "F32", "float": "F32"}


def gc_fixed(value):
    """Is this a GameCube address the port cannot use as-is?"""
    return (0x80000000 <= value < 0x82000000 or 0xC0000000 <= value < 0xC2000000
            or 0xCC000000 <= value < 0xCD000000 or 0xE0000000 <= value < 0xE0004000)


def function_bodies(text):
    """(start, end) of every top-level function body, braces included."""
    bodies = []
    for s, e, head, depth, kind in statements(text):
        if kind != "def" or depth != 0:
            continue
        h = clean(PP_LINE.sub(" ", head))
        if not h or h.endswith("=") or h.startswith(("typedef", "struct", "union", "enum")) \
                or re.search(r"\b(struct|union|enum)\s*\w*$", h):
            continue
        d = 0
        for kind_, val, ts, te in tokens(text[e:]):
            if val == "{":
                d += 1
            elif val == "}":
                d -= 1
                if d == 0:
                    bodies.append((e, e + te))
                    break
    return bodies


def matching_paren(text, i):
    """Index just past the ')' matching the '(' at text[i]."""
    d = 0
    for j in range(i, len(text)):
        if text[j] == "(":
            d += 1
        elif text[j] == ")":
            d -= 1
            if d == 0:
                return j + 1
    return None


def redirect_fixed_addresses(text, stats):
    """Rewrite fixed GameCube addresses inside function bodies; see
    platform/include/port/gc_mem.h. Data initializers are left alone: there
    the words are usually colours or flags typed as void*."""
    out, pos = [], 0
    for bs, be in function_bodies(text):
        body = text[bs:be]

        # GX FIFO stores first, so their address is not taken by CAST_CONST.
        parts, p = [], 0
        for m in FIFO_WRITE.finditer(body):
            if m.start() < p:
                continue
            end = body.find(";", m.end())
            if end < 0:
                continue
            parts.append(body[p:m.start()])
            parts.append(f"__GXFifo{FIFO_FN[m.group(1)]}({body[m.end():end].strip()})")
            p = end
            stats["fifo_writes"] += 1
        parts.append(body[p:])
        body = "".join(parts)

        def const_sub(m):
            if not gc_fixed(int(m.group(2), 16)):
                return m.group(0)
            stats["fixed_addresses"] += 1
            return f"{m.group(1)}__GC_ADDR({m.group(2)})"
        body = CAST_CONST.sub(const_sub, body)

        # (T*)(expr) where expr contains a fixed address, e.g. start + 0x80000000.
        parts, p = [], 0
        for m in CAST_EXPR.finditer(body):
            if m.start() < p:
                continue
            op_start = m.end() - 1
            op_end = matching_paren(body, op_start)
            if op_end is None:
                continue
            operand = body[op_start:op_end]
            if "__GC_ADDR" in operand or not any(gc_fixed(int(h, 16)) for h in re.findall(GC_HEX, operand)):
                continue
            parts.append(body[p:m.end() - 1])
            parts.append(f"__GC_ADDR{operand}")
            p = op_end
            stats["fixed_address_exprs"] += 1
        parts.append(body[p:])
        body = "".join(parts)

        out.append(text[pos:bs])
        out.append(body)
        pos = be
    out.append(text[pos:])
    return "".join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--objects", type=Path, default=ROOT / "build" / "objects.json")
    ap.add_argument("--groups", default="game,library")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--compile", action="store_true")
    ap.add_argument("--only", default=None)
    ap.add_argument("-o", "--out", type=Path, default=ROOT / "build" / "unified")
    args = ap.parse_args()

    manifest = json.loads(args.objects.read_text(encoding="utf-8"))
    decomp = Path(manifest["decomp"]).resolve()
    groups = set(args.groups.split(","))
    units = [u for u in manifest["units"] if u["group"] in groups and u["lang"] == "c"]

    with ThreadPoolExecutor(args.jobs) as pool:
        pp = list(pool.map(lambda u: preprocess(u, decomp, args.out), units))
    failed_pp = [u["source"] for u, t in pp if t is None]
    pp = [(u, t) for u, t in pp if t is not None]

    defs, clash = collect_definitions(pp)

    stats = {"aliased_names": 0, "uses_renamed": 0, "calls_cast": 0,
             "static_fixed": 0, "implicit_views": 0, "hoisted": 0, "weak_inline": 0,
             "fifo_writes": 0, "fixed_addresses": 0, "fixed_address_exprs": 0,
             "narrowing": defaultdict(set), "float_abi": defaultdict(set),
             "float_unfixed": defaultdict(set)}
    work = [(u, t) for u, t in pp if not args.only or args.only in u["source"]]
    for u, text in work:
        new = rewrite_unit(u, strip_extern_c(text), defs, stats)
        new = redirect_fixed_addresses(new, stats)
        if u["module"] != "main":
            # Every REL defines its own _prolog/_epilog/_unresolved. Linked
            # statically they need distinct names; the platform's OSLink
            # table calls <module>__prolog and so on.
            new = re.sub(r"\b(_prolog|_epilog|_unresolved)\b",
                         lambda m: f"{u['module']}_{m.group(1)}", new)
        dst = args.out / "src" / (u["name"] + ".i")
        dst.parent.mkdir(parents=True, exist_ok=True)
        dst.write_text(new, encoding="utf-8")

    summary = {
        "units": len(units),
        "preprocess_failed": failed_pp,
        "definitions": len(defs),
        "conflicting_definitions": sorted(clash),
        "aliased_names": stats["aliased_names"],
        "uses_renamed": stats["uses_renamed"],
        "calls_cast": stats["calls_cast"],
        "narrowing": {k: sorted(v) for k, v in sorted(stats["narrowing"].items())},
        "float_abi_fixed": {k: sorted(v) for k, v in sorted(stats["float_abi"].items())},
        "float_abi_unfixed": {k: sorted(v) for k, v in sorted(stats["float_unfixed"].items())},
    }
    args.out.mkdir(parents=True, exist_ok=True)
    (args.out / "summary.json").write_text(json.dumps(summary, indent=1), encoding="utf-8")
    print(f"{len(units)} units, {len(failed_pp)} failed to preprocess")
    print(f"{len(defs)} external definitions, {len(clash)} names defined with different types")
    print(f"{stats['aliased_names']} (unit, function) pairs given aliases, "
          f"{stats['uses_renamed']} uses renamed")
    print(f"narrowing views: {len(stats['narrowing'])} functions, {stats['calls_cast']} calls cast")
    print(f"implicit-call views: {stats['implicit_views']}, extern-before-static fixed: "
          f"{stats['static_fixed']}, block-scope declarations hoisted: {stats['hoisted']}, "
          f"inline functions made weak: {stats['weak_inline']}")
    print(f"fixed GameCube addresses redirected: {stats['fixed_addresses']} constants, "
          f"{stats['fixed_address_exprs']} computed, {stats['fifo_writes']} GX FIFO writes")
    print(f"float ABI: {len(stats['float_abi'])} functions fixed, "
          f"{len(stats['float_unfixed'])} unprototyped without a known definition")

    if args.compile:
        compile_unified(work, args)


def compile_unified(pp, args):
    cc = compile_check.DEVKITARM / "bin" / "arm-none-eabi-gcc.exe"

    def one(item):
        u, _ = item
        src = args.out / "src" / (u["name"] + ".i")
        obj = args.out / "obj" / (u["name"] + ".o")
        obj.parent.mkdir(parents=True, exist_ok=True)
        cmd = [str(cc), *compile_check.ARCH, "-O1", *compile_check.SEMANTICS,
               "-fdiagnostics-plain-output", "-fmax-errors=50", "-std=gnu99",
               "-Wdouble-promotion", *compile_check.PERMISSIVE, "-x", "cpp-output",
               "-c", str(src), "-o", str(obj)]
        p = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8", errors="replace")
        text = p.stdout + p.stderr
        errs = [l for l in text.splitlines() if ": error: " in l]
        promo = sum(1 for l in text.splitlines() if "[-Wdouble-promotion]" in l)
        return u, p.returncode == 0, errs, promo

    with ThreadPoolExecutor(args.jobs) as pool:
        res = list(pool.map(one, pp))
    ok = sum(1 for _, good, _, _ in res if good)
    msgs = Counter()
    for _, good, errs, _ in res:
        seen = set()
        for e in errs:
            k = compile_check.normalise(e.split(": error: ", 1)[1])
            if k not in seen:
                msgs[k] += 1
                seen.add(k)
    print(f"\nunified compile: {ok}/{len(res)} units")
    print(f"double-promotion warnings left: {sum(r[3] for r in res)}")
    for k, n in msgs.most_common(20):
        print(f"  {n:5}  {k[:110]}")
    (args.out / "compile.json").write_text(json.dumps(
        [{"source": u["source"], "name": u["name"], "ok": good, "errors": errs[:10]}
         for u, good, errs, _ in res], indent=1), encoding="utf-8")


if __name__ == "__main__":
    main()
