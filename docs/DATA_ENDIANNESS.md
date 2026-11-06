Data endianness
===============

First measured 2026-11-06 against decomp commit 1ab993327.

The problem
-----------

The decomp stores much of the game's static data as byte arrays in the
GameCube's big-endian order:

    u8 lbl_803130F0[60] = { 0x00, 0x00, 0x00, 0x04, 0x02, 0x80, ... };

On the little-endian 3DS, any 16-, 32- or 64-bit field read from such an
array comes out byte-swapped. The first time this bit, a render mode's
640-pixel width read as 0x8002, the framebuffer size became 0xE01D0020, and
the next heap landed outside memory (see BOOT_PROGRESS.md).

There are 1,009 such arrays in the decomp, 429,193 bytes. A second, smaller
problem: typed arrays (u16, u32 ...) are stored in native order, which is
only right when the code reads them with the element width. 28 typed arrays
are read with other widths, for example a u32 colour table read byte by
byte (RGBA would come out as ABGR).


The approach
------------

The original code knows the field widths: every load and store has one
(lbz 1, lhz 2, lwz and lfs 4, lfd 8, psq_l by its GQR). tools/access_widths.py
disassembles the decomp's original objects (the dtk split of the retail DOL
and RELs, with their relocations) and follows which registers point into
which data symbol:

- address formation: lis/addi, addis for offsets above 64 KB, li and addi
  with SDA21 relocations (small data);
- accesses through those registers, with displacement;
- array walks: mulli/slwi plus add give an element stride, and a pointer
  bumped by `addi rX, rX, k` in a loop walks with stride k;
- pointers passed to functions: the callee's accesses through its parameter
  count for the argument, closed over calls that pass the pointer on;
- pointers returned by functions: the game's many `...DataBiosGetPtr`
  getters load an entry from a pointer table and return it; the caller's
  accesses through r3 count for every table entry (four passes, so getters
  calling getters resolve too);
- pointers kept in globals and in pointer tables in data (ADDR32
  relocations);
- control flow: forward branches carry their register state to the
  target, where incoming states are merged (NULL agrees with a pointer,
  pointers at the same offset merge into one pointer to any of their
  symbols); loop heads keep registers with a single definition and
  pointers that walk the array with a fixed step;
- the stack frame as a pseudo-symbol, so struct copies and memcpy into
  locals or globals can be followed: what is read from the copy counts for
  the source;
- accesses beyond a symbol's end, which belong to the array that holds that
  address (dtk's labels do not always match the real objects);
- struct copies themselves (a load whose register is next stored with the
  same width; MWCC copies structs as runs of lwz/stw or lfd/stfd) say
  nothing about fields and never decide a layout.

Evidence is ranked. Direct accesses decide the layout. Accesses through a
pointer that may point at several symbols only fill bytes nothing direct
covers, and are dropped where they disagree. Accesses carried back through
a copy only count for symbols with no direct evidence at all.

From the accesses it writes platform/data_types.generated.txt. The rewrite
pass in unify_prototypes.py applies it together with the hand-written
platform/data_types.txt (which wins for any symbol in both): byte arrays get
each multi-byte field byte-swapped, typed arrays read only with narrower
widths get values recomputed so memory holds the bytes the code expects.

Run it with:

    python tools/access_widths.py


Results
-------

    arrays  bytes     verdict
       157  118,432   layout derived, applied
        32  133,030   only byte accesses seen, nothing to do
         9   25,280   unresolved: conflicting widths or copies only
       811  152,451   no accesses found (see below)

    typed arrays read with another width: 32; those read only with
    narrower widths are rewritten, the rest need a decision

Plus the 30 hand-written render modes. 198 objects are rewritten in total.

The first version of the tool (before return values, control-flow merging
and copies) derived 124 layouts and left 185 KB without accesses, 88 KB of
it not passed to any function. Return values and merging resolved the
getter pattern; that 88 KB is now 55 KB.

### Validation, and a corrected hand layout

The render modes were decoded by hand before the tool existed. The tool
resolves one of them (lbl_80312D30) independently. Comparing the two showed
that the hand layout was wrong: it put xFBmode at +18, but the SDK's
GXRenderModeObj aligns that u32 to +20 after two bytes of padding, and the
data agrees (lbl_803130F0 has 00 00 at +18 and xFBmode = 1 at +20). The
hand layout is corrected. The tool's fields agree with the corrected one; it
finds 2 of the 9 multi-byte fields, the others being read by SDK code that
gets the pointer through a global and va_arg.

So derived layouts are correct as far as they go but can be incomplete:
they are "at least these fields". Unobserved bytes keep their big-endian
order, which is what they had before. And hand layouts need the same
checking against the data as everything else.

### The 811 arrays with no accesses found

    bytes    arrays  what
    77,066        2  textures built into the DOL, passed to GStextureLoad
    13,985      194  strings for GSlogWrite, OSReport, sprintf, __assert ...
     6,205       59  passed to other functions the analysis does not see into
    55,195      556  not passed to any function; by content:
                       11,284 strings (including the memory card icon)
                          258 zero-filled
                       43,636 other, including the 6,144-byte memory card
                              banner; the rest are small tables (most under
                              1 KB) reached through paths not followed yet

Textures must stay big-endian. The GX layer's texture decoder has to read
the GameCube formats anyway, 16-bit texels included, so it reads them as
big-endian. The same goes for the memory card banner and icons, which are
written to the card in the GameCube's format.

### Unresolved (9 arrays, 25 KB)

The largest is lbl_802E7CE8 (23 KB, the item sprite table), where two
readers disagree about the field at +0 of each 24-byte entry. Two kinds:

- a byte read of part of a wider field (lbz at +1 of a u16), and indexed
  2-byte reads next to fields of other widths. No data layout satisfies
  both readers; these need a code patch at the narrow read.
- arrays only ever copied by word. The copy says nothing about the fields;
  the readers of the copy decide, and the analysis does not follow copies
  into stack or heap objects yet.

The list is in build/access_widths.json (verdict per symbol).


Next steps
----------

- Look into lbl_802E7CE8 and the other unresolved arrays one by one;
  patch the narrow reads behind real conflicts.
- Follow pointers through heap objects and through function pointers
  (bctrl), which is where most of the remaining small tables go.
- A proper fixpoint over loops instead of the single-definition rule.
- Data loaded from files (FSYS archives, HSD .dat) is a separate problem:
  it has to be swapped when it is loaded, by type (roadmap phase 3).
