Where we are, what comes next
=============================

Last updated 2026-11-08, at commit affbe7d.


Where we are
------------

  - All 1,921 decomp units compile; 33 stubs are left, none for game code.
  - In Azahar the game boots through the title screen and the memory card
    questions to the main menu, and Story Mode goes through "start a new
    Story?", the name choice and the keyboard to the first scene, Outskirt
    Stand (module S1_out): Wes, the train, the buildings and the sky draw
    right. The ground draws black. Audio is silent, there is no memory
    card, movies are skipped.
  - Drawing: transform and lighting run on the PICA (host/tnl.v.pica) for
    every draw it can express; the rest takes the CPU path. Textures use
    compact PICA formats and are evicted when memory runs out. See
    docs/RENDERER.md.
  - Speed in Azahar: boot 25-27 frames a second, title screen ~19, main
    menu ~19, Outskirt Stand ~14. Azahar does not model the console's
    costs well (syscalls, VFP stalls, its software vertex shader), so only
    console numbers count.
  - On the New 3DS (docs/HARDWARE.md), last measured at 7029cea, before
    the shader and the texture fix: boot 25-28, title screen 12-14.5, main
    menu 22. The CPU was the limit: 28-31 ms of vertex work a frame on the
    title screen.
  - The console's SD card has the verified disc image and the 3dsx of
    affbe7d, with a test script that goes from the title screen into
    Outskirt Stand and saves screenshots at frames 600, 1450, 3200 and
    3600. It has not been run yet.

What was done on 2026-11-08, in order:

  1. Script VM crash on the console (cms_op32_copyStruct): 43 generated
     data units (script bytecode, battle scripts, messages) were not
     linked. tools/ppc_data.py converts the decomp's PowerPC objects.
  2. Faster boot: draw-done is raised at once instead of at the next
     retrace (every frame lost a retrace).
  3. The last 18 game units compile: four bugs in unify_prototypes.py,
     cast-as-lvalue rewriting, platform/fixups.txt for K&R calls whose
     argument order came from the PowerPC registers.
  4. The game is built at -O2 (without GCC's own inlining: polling loops
     on accessor functions hung). The game's idle function no longer takes
     the interrupt lock 140,000 times a second (game_patch/gs_gapp_idle.c).
  5. Vertex path: lights read once per draw, fewer divisions; the
     profiler splits vertex work, texture decoding and where the main
     thread blocks.
  6. Transform and lighting on the PICA; compact texture formats and
     eviction (the field had run out of texture memory).


Next, in order
--------------

  1. Run affbe7d on the console (about five minutes, then HOME), fetch
     /colosseum-port.log, the screenshots and any new crash dump. Check:
       - do the screenshots match Azahar's (the shader on real hardware)?
       - the "prof:" lines on the title screen, the menu and the field;
       - did quitting crash? (not seen since the fix, the runs ended
         earlier)
  2. Speed, depending on that. Candidates:
       - what is left of the ~40 ms a frame of game code (model
         animation and matrix work is the guess; measure first);
       - interrupt handlers (audio callbacks, 3-6 ms a frame);
       - decoding display lists once instead of every frame.
  3. The black ground in Outskirt Stand (both paths draw it black, so it
     is not the shader: a texture format, the TEV setup or fog).
  4. Gameplay: walking, the first dialogue, the first battle. Battle Mode
     from the main menu is a quick way into a battle.
  5. Smaller open items:
       - GSflagClear "Initialization has not finished" x3 at boot.
       - audio (currently a silent DSP), memory card saves, THP movies.
       - Renderer gaps: lines and points, GXCopyTex, indirect textures,
         fog, swap tables (docs/RENDERER.md).
       - The left side of some 2D screens is cut off (no scissor; the
         640x480 frame is mapped 4:3 into the 400x240 screen).


How to pick it up
-----------------

Build and run in Azahar (from the port repo):

    python tools/unify_prototypes.py --compile    # after game, replace.txt or fixups.txt changes
    python tools/link_check.py
    python tools/link_be.py                       # check "link exit 0"
    python tools/run_azahar.py --seconds 120 --script "650 press START 8;1020 press A 6;1160 press UP 4;1190 press A 6;1450 shot"

Into Outskirt Stand (Azahar is slow there; "1 tnl 0" keeps it on the
faster CPU path until the field):

    "1 tnl 0;650 press START 8;1020 press A 6;1160 press UP 4;1190 press A 6;1500 press A 6;1620 press UP 4;1650 press A 6;2000 press DOWN 4;2030 press A 6;2200 press A 6;2400 press A 6;2700 press A 6;3000 press A 6;3200 shot;3205 tnl 1;3215 shot"

Screenshots land in build/shots/. Azahar runs minimised and the port
presses the buttons itself; never capture the desktop or send keys to it.
After a run, check Azahar's log for "unmapped" lines: reads that would
crash on the console.

For hangs: `run_azahar.py --gdb --interrupt 25` (gdb's stop on attach
shows where the main thread spins).

On the console: docs/HARDWARE.md.
