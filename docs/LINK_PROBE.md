Link probe
==========

First run 2026-11-06 against decomp commit 1ab993327, with devkitARM GCC 16.1.0 and libctru.

After the compile survey (see COMPILE_SURVEY.md), the next question was
whether the game code links, and what it still needs from outside. The short
answer: it links into a .3dsx once the missing symbols are stubbed, and it
starts in Azahar and runs until the first SDK call.

To reproduce:

    python tools/gen_objects.py --decomp ../pkmn-colosseum
    python tools/unify_prototypes.py --compile
    python tools/link_check.py
    python tools/link_probe.py

The output is build/link_probe/colosseum.3dsx.


What linking found
------------------

The first symbol scan reported 186 symbols defined in more than one object.
Three causes:

1. Per-unit defines. 23 units get a `-D` from `extra_cflags` in the decomp's
   configure.py, for example `-DPDA_RANGE_80037158_ONLY`, which leaves out
   the part of a shared .c file that another unit owns. gen_objects.py now
   reads them from objdiff.json, and every compile passes them. This removed
   174 of the 186 duplicates. It also removed 27 of the 28 "defined with
   different types" warnings, which were really the same functions
   compiled twice.
2. `inline` functions. Some units declare a function `inline` on purpose, so
   that MWCC expands it without emitting a second copy of a symbol another
   unit owns. With -fgnu89-inline, GCC emits an external copy in every unit.
   GCC also ignores `weak` on inline functions. So unify_prototypes.py drops
   the `inline` from plain (non-static, non-extern) inline definitions and
   makes them weak; the owner's strong definition wins. 129 definitions are
   affected. GCC no longer inlines those calls, which costs speed but not
   correctness.
3. REL entry points. Every REL defines `_prolog`, `_epilog` and
   `_unresolved`. They are renamed to `<module>__prolog` and so on, for the
   static module table that will replace `OSLink`.

One duplicate is left, `fn_80012858`, in `gs_range_80011EA4` and
`gs_range_candidate_80012858`. The probe links with
`--allow-multiple-definition` for now and says so.


What is still missing
---------------------

649 symbols are referenced but not defined by the compiled game code. 32 of
them come from newlib, libgcc or libctru (memcpy, sin, sprintf, the ARM
division helpers). The other 617 are stubbed. To see where each one comes
from, the decomp's own MWCC objects are checked:

    316  from the SDK, runtime and debugger units the port replaces
    158  from the 24 units that do not compile yet
    116  from the generated units (messages, battle scripts)
     17  from the 4 C++ units, which the tool does not handle yet
      3  from compiled units, but missing from the ARM objects
      7  not defined anywhere: __GXFifoPtr (ours), __builtin_va_info and
         __memcpy (MWCC builtins), hsdForgetClassLibrary,
         hsdObjIsDescendantOf, salReconnectVoice, and one asm-unit label

The 316 are the platform layer's real scope. PLATFORM_TODO.txt lists them:

    102  GX (graphics)
     63  OS (threads, alarms, time, memory)
     35  matrix and vector math
     19  CARD (memory card)
     18  AI / AR / ARQ / DSP (audio hardware)
     14  THP (video player)
     11  DVD (disc)
     11  VI (video)
     10  PAD / SI (controller)
      5  cache (DC/IC)
     28  other, including 18 unnamed SDK functions (fn_800...)


The .3dsx
---------

    text 1,986,312   data 1,004,936   bss 994,712   (about 3.8 MB in total)
    colosseum.3dsx   about 3.0 MB

That is well inside even an Old 3DS's memory, before any game data is
loaded.

In Azahar the program starts, runs the ARM build of the game's `main`, and
stops in the first stub, `OSProtectRange`. That is called from the error
handler setup in `fn_80006250` (main_tail.c), before the heap is set up.
Each stub writes its name to `sdmc:/colosseum-port-stub.log` and breaks, see
platform/src/port_stub.c.

One warning from that run: before the stub, `main` clears GameCube low
memory with `memset((void*)0x80001800, 0, 0x1800)`. Azahar logs 1,536
unmapped writes for this and carries on. Real hardware would fault. So a
clean run in Azahar does not prove the code works on a 3DS, and the port
needs a way to redirect GameCube low-memory addresses (0x80000000-0x80003FFF)
to a real buffer.


Checking that the rewrite is still behaviour-neutral
----------------------------------------------------

Same check as in the compile survey, now with the defines, the weak inline
change and the REL renames in place. 1,508 units compile both directly and
through the tool:

    1284  identical object code
     174  differ only by narrowing casts
      25  differ only by the REL entry-point renames
      20  differ only because weak functions are no longer inlined
       4  differ only by the float prototype fix
       1  same functions in a different order (gs_part_exact_800EE150)


Next steps
----------

- Redirect GameCube low-memory accesses to a buffer.
- Start the platform layer with what the boot reaches first: OS (memory,
  arena, threads, alarms, interrupts), then DVD, so the game can load its
  first files. Implement it so that each stub replaced moves the first-stub
  marker further through the boot.
- Handle the C++ units and the generated data units, and finish the
  24 remaining units.
