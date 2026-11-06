Boot progress
=============

How far the probe build gets in Azahar, step by step. Each step lists the
first unimplemented symbol reached (from sdmc:/colosseum-port-stub.log, see
tools/run_azahar.py) and what was added to get past the previous one.
Unmapped memory accesses stay at zero throughout; Azahar tolerates those,
real hardware does not, so any count above zero is a bug.

    first stub reached         added to get there
    -------------------------  -----------------------------------------------
    OSProtectRange             nothing: the first link (2026-11-06)
    OSProtectRange             fixed GameCube addresses redirected
    OSSetErrorHandler          os_core.c: arena, interrupts, time, caches,
                               memory protection, OSReport; ar.c (ARAM, ARQ);
                               SDK arena and heap allocator reused
    GSlogWrite                 OSSetErrorHandler; ai.c placeholder
    _toolentryAlloc            the 4 C++ units compiled (as C, or with g++)
    OSTicksToCalendarTime      MWCC names for calls made from C++ units
    GSscratchInit              SDK calendar code reused; OSGetTime counts from
                               2000-01-01 like the GameCube
    GSmathInit                 game_asm/gs_scratch.c, os/lc.c (locked cache)
    DVDInit                    asm functions stripped from units, so the C
                               around them compiles; game_asm/gs_math.c
    GXInit                     dvd/dvd.c: reads the GC6E01 image from the SD
                               card, FST paths, deferred async completion

At GXInit, main() and the start of the game's init (fn_800057B0) have run:
heaps, ARAM, the log, the scratch allocator, the math tables and the disc.
The next step is the graphics layer.


Running it
----------

The DVD layer reads the user's own disc image from
`sdmc:/3ds/colosseum/GC6E01.iso`. For Azahar that is
`%APPDATA%\Azahar\sdmc\3ds\colosseum\GC6E01.iso`. Then:

    python tools/link_probe.py
    python tools/run_azahar.py


Platform pieces so far
----------------------

    platform/src/os/os_core.c     arena (19 MB, as on the GameCube), interrupts
                                  (a flag; pending work runs when they come
                                  back on), OSGetTime/OSGetTick in GameCube
                                  ticks from 2000-01-01, caches, OSReport,
                                  error handlers
    platform/src/os/ar.c          ARAM as a 16 MB buffer, DMA and ARQ as memcpy
    platform/src/os/lc.c          the locked cache as a 16 KB buffer
    platform/src/audio/ai.c       placeholder audio interface
    platform/src/dvd/dvd.c        disc image, FST, sync and async reads
    platform/src/gc_lowmem.c      GameCube low memory and its boot values
    platform/src/game_asm/        C versions of the game's own assembly
    platform/reuse.txt            SDK units compiled from the decomp as they are

Still in the game_asm work list (removed from their units by
unify_prototypes.py): the game's thread system (_threadSwitch,
threadExecute, threadLoad/Save*Registers, fn_800F02F4), the script engine's
cms_CallStdTrampoline, MusyX reverb (HandleReverb, DoCrossTalk) and
HSD_SaveContext.
