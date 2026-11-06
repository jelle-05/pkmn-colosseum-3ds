pkmn-colosseum-3ds
==================

A native Nintendo 3DS port of Pokémon Colosseum, built on the
[pkmn-colosseum](https://github.com/dougchansan/pkmn-colosseum) decompilation.

This is an early work in progress. Nothing runs yet. The plan and the current
state are in the decomp's [docs/3DS_PORT_ROADMAP.md](decomp/docs/3DS_PORT_ROADMAP.md).

This repository contains no game data, and never will. You need your own copy of
the game (NTSC-U, GC6E01).


Status
------

- Phase 0 (matching baseline): done. The decomp builds and matches on Windows.
- Phase 1 (compile the game code for the host and the 3DS): started.
  `tools/gen_objects.py` produces the object list.

The target is the New 3DS. The Old 3DS is probably too slow.


Getting started
---------------

Clone with the submodule:

    git clone --recursive <url> pkmn-colosseum-3ds

The decomp lives in `decomp/` and is used as-is. Build it once as described in
its README. That needs your disc in `decomp/orig/GC6E01/`. Then generate the
object list:

    python tools/gen_objects.py

If you already have a built decomp checkout at the same commit, point at it
instead:

    python tools/gen_objects.py --decomp ../pkmn-colosseum

The output goes to `build/objects.json`.


Requirements
------------

- Python 3.8 or newer
- devkitPro with `3ds-dev` (devkitARM, libctru, citro3d) for 3DS builds
- A 32-bit host compiler (GCC or Clang with `-m32`, or i686 mingw) for host builds
- Azahar or real hardware (New 3DS with Luma3DS) to run it


Layout
------

    decomp/      the decompilation (submodule, do not edit here)
    tools/       build helpers
    platform/    SDK replacement: OS, DVD, GX, PAD, CARD, audio (planned)
    compat/      MWCC compatibility headers (planned)
    swap/        endian swappers for game file formats (planned)
    patches/     small, documented changes to decomp sources (planned)


Notes
-----

- The host build has to be 32-bit. The game relocates pointers inside its data
  files in place, which only works with 4-byte pointers.
- The decomp covers 25 of the 168 REL modules on the disc. Most maps after the
  first town are missing until the other 143 are decompiled.


Credits
-------

The decompilation is the work of the pkmn-colosseum contributors. See
`decomp/README.md` and `decomp/NOTICE`.

Pokémon Colosseum is © 2003–2004 Pokémon / Nintendo / Creatures / GAME FREAK /
Genius Sonority. This project is not affiliated with or endorsed by any of them.
