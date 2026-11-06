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

    OSGetConsoleSimulatedMemSize  gx/: GX state setters, frame model, citro3d;
                               video/vi.c with retrace callbacks; SDK
                               GXInitTexObj and light initialisers reused
    C_MTXPerspective           OSGetConsoleSimulatedMemSize (from low memory)
    OSGetProgressiveMode       SDK projection math reused
    PADSetAnalogMode           data_types.txt: the 30 SDK render modes byte-
                               swapped (they are big-endian u8 arrays; see
                               below); mtx/mtx.c; progressive/sound mode
    fn_800AB4FC                pad/pad.c: 3DS buttons as a GameCube controller
    SISetSamplingRate          PADControlAllMotors, PADReset, PAD reset handler
    OSSetIdleFunction          pad/si.c: controller ports, no GBA link

At OSSetIdleFunction, HSD_InitComponent has run: video set up, the first
frame presented through GXCopyDisp (sdmc:/colosseum-port-frames.log), the
HSD heap, lights and input. The next step is the OS thread system.

The heap failure on the way showed the data problem predicted in the
roadmap: the decomp stores many objects as big-endian u8 arrays (about
500 KB in 974 arrays). A 640-pixel-wide render mode read 0x8002 as its
width, so the framebuffer size became 0xE01D0020 and the following heap
landed outside memory. platform/data_types.txt now gives such objects a
field layout and the tool byte-swaps their initializers. The general fix,
deriving layouts from the access widths in the original code, is a work
package of its own.


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
    platform/src/gx/              GX state (gx_state.c), init, copies, frame
                                  model and sync (gx_frame.c); nothing is drawn
                                  yet
    platform/src/video/           citro3d setup and frames (display.c), VI and
                                  retrace callbacks (vi.c)
    platform/src/mtx/mtx.c        matrix and vector math the SDK does in asm
    platform/src/pad/             controller (pad.c) and serial ports (si.c)
    platform/reuse.txt            SDK units compiled from the decomp as they are
    platform/data_types.txt       field layouts for big-endian data objects

Still in the game_asm work list (removed from their units by
unify_prototypes.py): the game's thread system (_threadSwitch,
threadExecute, threadLoad/Save*Registers, fn_800F02F4), the script engine's
cms_CallStdTrampoline, MusyX reverb (HandleReverb, DoCrossTalk) and
HSD_SaveContext.
