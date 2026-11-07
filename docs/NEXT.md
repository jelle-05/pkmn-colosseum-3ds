Where we are, what comes next
=============================

Last updated 2026-11-07, at commit 7b8dee4.


Where we are
------------

  - The game boots in Azahar through the title screen, "There is no Memory
    Card in Slot A" and "Continue without saving?" to the main menu (Battle
    Mode, Story Mode, Options). Text, windows and 3D models draw right.
    Audio is silent, there is no memory card, movies are skipped.
  - Speed in Azahar: title screen 12-15 frames a second, main menu 19.5.
  - On a real New 3DS (docs/HARDWARE.md): it boots and gets to the memory
    card check, at 5 frames a second. The disc image is on the console's
    SD card and verified (SHA-256 matches).
  - The 3dsx on the console's SD card is 7b8dee4, which has the New 3DS
    clock, VFP RunFast and the fix for the crash on quitting. It has not
    been run yet.

What was done on 2026-11-07, in order:

  1. Text after START: gfxSetPerspective/gfxSetOrtho and the GSmtx44
     wrappers relied on floats left in PowerPC registers; replaced
     (game_patch/gs_gfx_projection.c). Same for fn_800E0718 and
     fadeEffectHookFunction_carde (game_patch/gs_float_passthrough.c).
     docs/BOOT_PROGRESS.md has the full scan.
  2. Garbled text: textures flushed with DCFlushRange are decoded again.
  3. The "missing" dark overlay is drawn with colour writes off on purpose
     (it resets depth). Nothing to fix.
  4. Speed: draw batching, state caching, -O2 for the platform, then an
     index buffer (each GX vertex transformed once). Profiler (os/prof.c)
     and scripted runs with in-port screenshots (os/script.c,
     run_azahar.py --script).
  5. First hardware run; crash on quitting found from the Luma dump and
     fixed; osSetSpeedupEnable and RunFast added.


Next, in order
--------------

  1. Run 7b8dee4 on the console for two minutes, quit with HOME, fetch
     /colosseum-port.log and any new crash dump. Check:
       - did quitting still crash?
       - the "prof:" lines: how far did vertex time drop from 95 ms?
  2. Speed, depending on what that shows:
       - if vertex work is still the bulk: the CPU transform in
         gx_draw.c emit() (matrix, projection, two light channels,
         texgen per vertex). Ideas: move the transform to the PICA's
         vertex shader for unlit draws (upload the position matrix as a
         uniform; most 2D and many 3D draws need no CPU lighting), cache
         transformed display lists, skip normal work when unlit.
       - game code is built at -O1 because -O2 hangs (probably a
         busy-wait on a variable that is not volatile). Finding that wait
         and building the game at -O2 is likely a large gain.
       - the interrupt lock is taken ~65,000 times a second; check it is
         not costing much on hardware.
  3. Gameplay: go from the main menu into Story Mode with the script and
     fix whatever breaks next (first field scene, characters, fades).
  4. Smaller open items:
       - "bad x" in the log: draws with w = 0 on the title screen (a
         projection whose clip w is 0); find which and why.
       - GSflagClear "Initialization has not finished" x3 at boot.
       - audio (currently a silent DSP), memory card saves, THP movies.
       - Renderer gaps: lines and points, GXCopyTex, indirect textures,
         fog, swap tables (docs/RENDERER.md).


How to pick it up
-----------------

Build and run in Azahar (from the port repo):

    python tools/unify_prototypes.py --compile    # after game or replace.txt changes
    python tools/link_check.py
    python tools/link_be.py                       # check "link exit 0"
    python tools/run_azahar.py --seconds 120 --script "650 press START 8;1020 press A 6;1160 press UP 4;1190 press A 6;1450 shot"

Screenshots land in build/shots/. Azahar runs minimised and the port
presses the buttons itself; never capture the desktop or send keys to it.

On the console: docs/HARDWARE.md.
