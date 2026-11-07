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

From here no stub is reached any more; the game stops on data problems
instead, found with the heartbeat in the port log and `run_azahar.py --gdb`
(a backtrace of every thread at the first heartbeat).

    where it stopped           added to get past it
    -------------------------  -----------------------------------------------
    (no stub, no frames)       os/: threads on 3DS threads, interrupt thread
                               for retrace, alarms and DVD completions,
                               message queues, mutexes, alarms; the game's
                               coroutines (threadExecute, _threadSwitch) on
                               an ARM context switch; THP placeholder; the
                               PowerPC-varargs menu functions replaced
    GSmemGetFreeTotal          reads OSAlloc's heap variables through
                               &__OSStartTime + 0x10. Data is now linked at
                               its GameCube offsets (tools/gen_layout.py);
                               labels inside a larger object are aliases
    windowOpen, loop           menu items: a void* table read as halfwords
                               (pointer tables now get field layouts too) and
                               bitfields (MWCC fills them from the top bit;
                               the tool reverses them per container)
    _fsysGetFilename, loop     the archive table of contents is file data
                               from the disc, big-endian. Next: file formats

The second thread (prio 16) is running at that point.

Then the port moved to the ARM11's big-endian data mode (docs/BIG_ENDIAN.md),
so file data is read as it is, and so is everything above that needed
converting:

    where it stopped           added to get past it
    -------------------------  -----------------------------------------------
    OSLink                     big-endian image in a little-endian host
                               (tools/link_be.py); the archive table of
                               contents now reads right, the loader thread
                               finishes
    (stalls, 3 s per frame)    os/os_link.c: OSLink finds the compiled-in
                               module by its REL ID and sets its prolog;
                               common_rel and mail link. The interrupt lock
                               holder is raised while others wait for it (the
                               idle function held it at the lowest priority
                               while the main thread spun in OSYieldThread);
                               OSYieldThread lets go of it
    DSPInit                    audio/dsp.c: a silent DSP that answers MusyX's
                               task and command lists; audio/ai.c: the AI DMA
                               as a 5 ms clock, so the synth keeps time
    (3 s per frame)            the interrupt lock holder raises itself above
                               the game threads, as a thread with interrupts
                               off is not preempted on the GameCube
    HandleReverb               MusyX's reverb kernels (assembly): empty until
                               there is sound
    _msgctrlMakeDigit__FPUslUll  C units with #pragma cplusplus on get MWCC's
                               mangled names
    CARDInit                   card/card.c: no card in either slot
    pokemonOboeWaza, bad read  two units' private inline_fn shared one copy;
                               such helpers are now local to their unit
    floorDataBiosGetFloorKind  game_c/floor_data.c: the floor accessors, which
                               the decomp has only as assembly
    seqStartPlay, bad read     the song's data was never loaded: fsys's ARAM
                               cache allocator treated every pointer below
                               0x80000000 as invalid; such checks are now null
                               checks. ARAM DMA completes from the interrupt
                               thread, after the request returns
    DLInsert                   SDK heap helper reused (platform/reuse.txt)
    GXInitLightDistAttn        SDK light initialisers reused
    cms_CallStdTrampoline      game_asm/cms_trampoline.c: the script VM's
                               native call
    (runs)                     no stub, no fault: 60 frames a second, the
                               floor manager running the first scene's
                               scripts. Nothing is drawn yet; the next step is
                               the renderer
    title screen               gx/gx_draw.c and host/gpu.c: the renderer
                               (docs/RENDERER.md). The title screen shows: the
                               desert with its ruins and the 3D logo, at about
                               10 frames a second in Azahar
    no text after START        the text camera's aspect ratio was 0:
                               gfxSetPerspective passes its floats on in
                               registers without naming them; replaced
                               (game_patch/gs_gfx_projection.c)
    garbled text               the font's glyph texture is rewritten; textures
                               flushed with DCFlushRange are decoded again
    main menu                  START, then A on "There is no Memory Card in
                               Slot A", YES on "Continue without saving?":
                               the main menu (Battle Mode, Story Mode, Options)
                               at 15 frames a second in Azahar


Floats passed in registers
--------------------------

Some decomp functions match MWCC's code by relying on float arguments
staying in the PowerPC's f1-f8 from one call to the next, without passing
them. On ARM those values are lost. A scan of the decomp finds 24 calls
without arguments to functions that take floats; the projection setters
are replaced, the rest are listed here and will show up as wrong values
when the game reaches them:

    GSmtx44Ortho, GSmtx44Perspective     replaced
    gfxSetPerspective, gfxSetOrtho       replaced (record path too)
    __GXInitGX                           not used (the platform's GXInit)
    _preReliveDisplayHokoraParticleThreadFunc__Fv  gsParticleSetVecB, waitTime__Ff
    dbgMenuSendAllMail                   mailMainSendAllMail
    effectPlayWaza, fieldStartEncount, fightActionFlowKaisiPre,
    fightStartEncount                    fadeSet
    fadeEffectHookFunction_carde         fn_801C5F6C
    fightOutPokemonHokakuEffect          fn_801DA2C4
    fightTrainerAiWazaValueMigawari      fightTrainerAiGetHpPercent
    floorElevatorSetupCharacters         GSmodelGetFrameCount, peopleStartTurn,
                                         std059_characterSetPosition
    fn_800E0718                          C_QUATRotAxisRad
    fn_80118DE0                          psSetGeneratorAngleRadiusScale
    pcMenuDrawPurifiedCount              gfxVtxTexCoord
    reliveCeremonyAll                    pokemonSetDp
    std232_characterApproachPlayer       peopleTurnToPoint

The game's own log (GSlogWrite) now goes to the port log as well, which
says what it is doing: it reports "GSflagClear: Initialization has not
finished" three times early on, and the two movies it cannot open.

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

    python tools/unify_prototypes.py --compile
    python tools/link_check.py
    python tools/link_be.py
    python tools/run_azahar.py           # or --gdb for threads and stacks


Platform pieces so far
----------------------

    platform/src/os/os_core.c     arena (19 MB, as on the GameCube),
                                  OSGetTime/OSGetTick in GameCube ticks from
                                  2000-01-01, caches, OSReport, error and
                                  reset handlers, the port log
    platform/src/os/os_irq.c      interrupts: a recursive lock, and a thread
                                  that delivers retrace, alarms and DVD
                                  completions at their due time
    platform/src/os/os_thread.c   OS threads on 3DS threads (priority 0x19 +
                                  GameCube priority), sleep queues, heartbeat
    platform/src/os/os_sync.c     message queues, mutexes, conditions, alarms
    platform/src/thread/          ARM context switch for the game's coroutines
    platform/src/game_patch/      game functions replaced (platform/replace.txt)
    platform/src/thp/             movie player placeholder (movies are skipped)
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
unify_prototypes.py): the script engine's cms_CallStdTrampoline, MusyX
reverb (HandleReverb, DoCrossTalk) and HSD_SaveContext. The game's thread
system is in game_asm/gs_thread.c.
