Compile survey
==============

First measured 2026-11-06 against decomp commit 1ab993327, with devkitARM GCC 16.1.0.

The question: how much of the decomp's game code compiles for the 3DS as it
is, and what breaks? Units come from `build/objects.json`. Only the `game`
group (src/game, src/rel) and the `library` group (src/hsd, src/musyx) are
counted, 1,902 units in total. The SDK, runtime and debugger units are
replaced by the platform layer and are not included.

Nothing is linked yet. "Compiles" means `arm-none-eabi-gcc -c` succeeds.


Results
-------

    step                                         units that compile
    no changes                                   1402 / 1902   73.7%
    + compat/mwcc_compat.h, header overrides     1522 / 1902   80.0%
    + tools/unify_prototypes.py                  1871 / 1898   98.6%

The last row has 1,898 units because the 4 C++ units are not handled by the
tool yet. Later on the same day, hoisting block-scope declarations, per-unit
defines and the K&R fix brought it to 1,874; see LINK_PROBE.md.

To reproduce:

    python tools/gen_objects.py --decomp ../pkmn-colosseum
    python tools/compile_check.py --no-compat -o build/compile_check_baseline
    python tools/compile_check.py
    python tools/unify_prototypes.py --compile


What broke, and why
-------------------

### Compiler syntax (fixed in compat/ and platform/include/)

- `__declspec(section ".sdata2")` in about 100 data units. This is only
  section placement, so the compat header defines `__declspec(x)` away.
- MWCC's address-bound variables, `extern u32 __OSBusClock : 0x800000F8;`.
  `platform/include/dolphin/os/OSClock.h` replaces it with the constant
  162 MHz bus clock. Game code keeps counting in GameCube ticks.
- The GX write-gather pipe at 0xCC008000 (`GXVert.h`). The override appends
  to a command buffer that the platform GX layer owns.
- `crt/stdarg.h` reproduced the PowerPC `va_list`. It now uses the GCC
  builtins. With the old header every variadic function would have read
  garbage on ARM.
- PowerPC intrinsics (`__cntlzw`, `__fabs`, `__frsqrte`, `__lwbrx`, cache
  ops) are implemented in plain C in the compat header.

### Conflicting declarations (fixed by tools/unify_prototypes.py)

This was the big one. After the compat header, 364 of the 385 failing units
failed only because one function was declared in two different ways in one
translation unit. `pokemonGetStatus` alone has at least eight variants in the
tree. MWCC accepted that. GCC does not.

Giving every function one canonical prototype does not work. That was tried
first and dropped compilation to 1,301 units, because the call sites really
do disagree with the definitions: 269 calls pass a different number of
arguments, 184 use the value of a function defined as `void`, and many treat
the result as a different pointer type. The matching build proves that this
is what the original game does, so the port has to keep each call site's own
view.

The tool works on preprocessed text and gives each extra view its own name,
bound to the real symbol with an assembler label:

    extern u8 f__pv1(void) __asm__("f");

The decomp itself is not touched. Changing declarations there would change
MWCC's code generation and break the matching build.

Other things the tool handles:

- implicit declarations used before a conflicting later declaration
  (146 call sites);
- `extern` declarations that precede a `static` definition (52);
- conflicting `extern` declarations of data, mostly `const` mismatches.

### ARM ABI differences found on the way

These compile without complaint and would have been silent bugs.

- **Narrow return views.** A caller declares `u8 f()` while the definition
  returns `u32`. On ARM, GCC assumes the callee already extended the value
  and never masks it. Measured on the original objects, MWCC masked or
  extended r3 right after the call at 1,796 of 2,776 such sites (65%). The
  tool casts these calls, which covers 2,961 calls to 266 functions.
- **Floats through unprototyped calls.** `f()` declarations promote float
  arguments to double. On the GameCube both travel in the same FPR, so it did
  not matter. On hard-float ARM a double takes two S registers. For the 26
  affected functions with a known definition, the tool uses the definition's
  parameter list. After rewriting, the only float-to-double argument
  promotions left are genuine variadic calls (`OSReport`, `sprintf`,
  `HSD_ForeachAnim`, `gfxRecordCommand`).
- **Argument order across register classes.** Some callers pass arguments in
  a different order than the definition, for example
  `colsysWallSweep(ptr, ptr, 0, float)` against
  `(Vec3f*, Vec3f*, f32 radius, Vec3f*)`. This works on both PowerPC and
  ARM-VFP, because integer and float arguments are allocated to separate
  register files. A prototype built from the definition breaks these calls.
  They need per-call prototypes built from the register classes of the
  actual arguments. The tool currently reports them as errors so that they
  cannot pass silently.
- **`inline`.** MWCC emits an external copy of a plain `inline` function.
  C99 does not, unless another declaration is visible, so functions quietly
  went missing. Everything is now compiled with `-fgnu89-inline`.

### Checking that the rewrite is behaviour-neutral

For units that compile both directly and through the tool (1,512), the
object code was compared:

- 1,333 are identical.
- 172 differ only by the narrowing casts above.
- 4 differ only by the float prototype fix.
- 3 differ in ways that do not change behaviour: function order inside the
  object, a duplicated literal load (alias and original name are separate
  declarations to GCC), and calls through an alias that GCC can no longer
  inline.


Remaining 27 units
------------------

    group                                                    units
    MWCC `asm` functions (thread switch, math, debug, reverb)    8
    arguments in register-class order (colsysWallSweep etc.)     7
    tool edge cases (array types, implicit decls, missing syms)  6
    section attribute on a block-scope declaration               3
    cast as lvalue, argument count against a same-unit def       3

The `asm` functions overlap with the 10 game entries in the decomp's
`docs/asm_evidence/registry.json`. The thread switch in
`gs_thread_exact_800F0030.c` belongs in the platform layer anyway.


Not covered yet
---------------

- The 4 C++ units.
- The 43 generated units (messages, battle scripts). Part of them are built
  straight into PowerPC objects by decomp tools, so they need their own path.
- Linking. Nothing above says the units link or run.
- Struct layout. Pointer size matches (32-bit), but bitfield order and any
  data read straight from files still need checking (see the roadmap).
