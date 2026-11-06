pkmn-colosseum-3ds
==================

A native Nintendo 3DS port of Pokémon Colosseum, built on the
[pkmn-colosseum](https://github.com/dougchansan/pkmn-colosseum) decompilation.
The `decomp/` submodule points to [my fork](https://github.com/jelle-05/pkmn-colosseum)
of it.

This is an early work in progress. Nothing runs yet. The plan and the current
state are in the decomp's [docs/3DS_PORT_ROADMAP.md](decomp/docs/3DS_PORT_ROADMAP.md).

This repository contains no game data, and never will. You need your own copy of
the game (NTSC-U, GC6E01).


Status
------

- Phase 0 (matching baseline): done. The decomp builds and matches on Windows.
- Phase 1 (compile and link the game code): mostly done. 1,874 of 1,898 game
  and library units compile with devkitARM and link into a .3dsx, with stubs
  for the 617 symbols that are still missing. In Azahar it starts and runs
  until the first SDK call. See [docs/COMPILE_SURVEY.md](docs/COMPILE_SURVEY.md)
  and [docs/LINK_PROBE.md](docs/LINK_PROBE.md).
- Phase 2 (platform layer): started. OS, ARAM and DVD basics are in, and the
  boot runs through main() and the start of the game's init until GXInit.
  See [docs/BOOT_PROGRESS.md](docs/BOOT_PROGRESS.md). The full work list is
  [docs/PLATFORM_TODO.txt](docs/PLATFORM_TODO.txt).

To run the probe build you need your own GC6E01 disc image at
`sdmc:/3ds/colosseum/GC6E01.iso` (for Azahar:
`%APPDATA%\Azahar\sdmc\3ds\colosseum\GC6E01.iso`).

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

The output goes to `build/objects.json`. Then, to compile everything for the
3DS and see what breaks:

    python tools/compile_check.py
    python tools/unify_prototypes.py --compile

To link it into build/link_probe/colosseum.3dsx:

    python tools/link_check.py
    python tools/link_probe.py


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
    platform/    SDK replacement: OS, DVD, GX, PAD, CARD, audio (header
                 overrides so far)
    compat/      MWCC compatibility header, force-included in every unit
    docs/        notes and measurements
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

This port would not be possible without the decompilation. All of the game code
in `decomp/` is their work:

- [dougchansan/pkmn-colosseum](https://github.com/dougchansan/pkmn-colosseum),
  the original decompilation, and its contributors
- [chasem-dev/pkmn-colosseum](https://github.com/chasem-dev/pkmn-colosseum),
  the fork my fork is based on
- [jelle-05/pkmn-colosseum](https://github.com/jelle-05/pkmn-colosseum), my fork,
  with a Windows build fix and the 3DS roadmap

The decompilation is MIT licensed. See `decomp/LICENSE`, `decomp/NOTICE` and
`decomp/README.md` for the full credits, including the third-party libraries it
reconstructs.

Thanks also to the devkitPro, libctru and citro3d developers.


License
-------

The code in this repository is MIT licensed, see `LICENSE`. The decompilation in
`decomp/` keeps its own MIT license and copyright notices.

Pokémon Colosseum is © 2003–2004 Pokémon / Nintendo / Creatures / GAME FREAK /
Genius Sonority. This project is not affiliated with or endorsed by any of them.
