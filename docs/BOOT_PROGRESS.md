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
them. On ARM those values are lost. A scan of the decomp found 24 places
that call a function taking floats without arguments; most of them are
K&R declarations inside a function (`extern int fadeSet();`) whose calls do
pass their arguments, and whose float passing unify_prototypes.py fixes.
The real pass-throughs, all replaced (platform/replace.txt):

    GSmtx44Perspective, GSmtx44Ortho     gs_math_mtx44.c
    gfxSetPerspective, gfxSetOrtho       gs_gfx_range_800D9AF0.c (record path too)
    fn_800E0718                          gs_math_quat.c (C_QUATRotAxisRad)
    fadeEffectHookFunction_carde         effect/fade_effect.c (fn_801C5F6C)

Left as they are: dbgMenuSendAllMail (a debug menu entry) and __GXInitGX
(the platform has its own GXInit). GXInitTexObjLOD is still called
unprototyped with floats from gs_gfx_backfb.c; the renderer ignores the
LOD values it sets.

The dark full-screen quad drawn before the memory card message is not
missing: the game draws it with colour writes off, to reset the depth
buffer before the 2D windows.