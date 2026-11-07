/*
 * cms_trampoline.c - the script VM's call into a native function.
 *
 * cms_op37_callStandard (src/game/gs_vm_exact_800F10E8.c) splits a script
 * call's arguments into an integer image (lbl_8047AC40, eight words) and a
 * float image (lbl_8047AC3C, eight floats), by the native's argument-type
 * bytes, and calls cms_CallStdTrampoline. The original is assembly
 * (src/game/gs_thread_exact_800F106C.c): it loads r3-r10 and f1-f8 from
 * the images and calls lbl_8047AC38, whatever the native's prototype.
 *
 * ARM's hard-float calling convention also assigns integers and floats
 * independently: integer arguments to r0-r3 and then the stack, float
 * arguments to s0-s7. So one call with eight integers and eight floats
 * puts each of them where a native with any mix of up to eight integer
 * and eight float parameters looks for it, in the same order, as on the
 * GameCube. Integer results come back in r0; the VM stores only those.
 */
typedef unsigned int u32;
typedef float f32;

extern u32 (*lbl_8047AC38)(void);      /* the native */
extern f32* lbl_8047AC3C;              /* float argument image */
extern u32* lbl_8047AC40;              /* integer argument image */

typedef u32 (*Native)(u32, u32, u32, u32, u32, u32, u32, u32,
                      f32, f32, f32, f32, f32, f32, f32, f32);

u32 cms_CallStdTrampoline(void)
{
    const u32* i = lbl_8047AC40;
    const f32* f = lbl_8047AC3C;
    return ((Native)lbl_8047AC38)(i[0], i[1], i[2], i[3], i[4], i[5], i[6], i[7],
                                  f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7]);
}
