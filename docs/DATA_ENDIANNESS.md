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
- pointers kept in globals and in pointer tables in data (ADDR32
  relocations);
- accesses beyond a symbol's end, which belong to the array that holds that
  address (dtk's labels do not always match the real objects);
- struct copies: a load whose register is next stored with the same width
  is a copy (MWCC copies structs as runs of lwz/stw or lfd/stfd) and only
  counts when nothing else is known.

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
       124  109,428   layout derived, applied
        27  132,708   only byte accesses seen, nothing to do
        32    2,452   unresolved: conflicting widths or copies only
       826  184,605   no accesses found (see below)

    typed arrays read with another width: 28, of which 25 are rewritten
    (read only with narrower widths) and 3 need a decision

Plus the 30 hand-written render modes. 166 objects are rewritten in total.

### Validation

The render modes were decoded by hand from the SDK's GXRenderModeObj before
the tool existed. The tool resolves one of them (lbl_80312D30, from HSD's
struct copy and field reads) independently: every field it finds matches
the hand-written layout, and it finds no field the layout lacks. It finds 2
of the 9 multi-byte fields; the other 7 are read by SDK code that receives
the pointer through a global and va_arg, a path the analysis does not
follow. So derived layouts are correct but can be incomplete: they are "at
least these fields". Unobserved bytes keep their big-endian order, which is
what they had before.

### The 826 arrays with no accesses found

Grouped by the function they are passed to:

    bytes    arrays  passed to
    77,066        2  GStextureLoad: textures built into the DOL
    13,500     ~200  GSlogWrite, OSReport, sprintf, __assert, MWTRACE ...:
                     strings
     5,000      ~30  GScameraLookAt, gfxSetVtxDesc and others: real data the
                     analysis misses (vectors, descriptor tables)
    88,476      576  not passed to any function: reached through pointer
                     tables the analysis does not resolve yet, or unused

Textures must stay big-endian. The GX layer's texture decoder has to read
the GameCube formats anyway, 16-bit texels included, so it reads them as
big-endian. The same goes for the memory card banner and icons, which are
written to the card in the GameCube's format.

### Unresolved (32 arrays, 2.4 KB)

Two kinds:

- a byte read of part of a wider field (lbz at +1 of a u16), and indexed
  2-byte reads next to fields of other widths. No data layout satisfies
  both readers; these need a code patch at the narrow read.
- arrays only ever copied by word. The copy says nothing about the fields;
  the readers of the copy decide, and the analysis does not follow copies
  into stack or heap objects yet.

The list is in build/access_widths.json (verdict per symbol).


Next steps
----------

- Follow struct copies and memcpy into stack and BSS objects, so the
  readers of the copy count for the source.
- Resolve the pointer tables behind the 576 arrays that are not passed to
  functions.
- Keep register values across loop heads with a proper dataflow merge
  instead of the single-definition rule.
- Patch the narrow reads behind the unresolved conflicts.
- Data loaded from files (FSYS archives, HSD .dat) is a separate problem:
  it has to be swapped when it is loaded, by type (roadmap phase 3).
