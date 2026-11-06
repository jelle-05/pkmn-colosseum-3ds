/*
 * gs_scratch.c - C version of GSscratchInit (0x800EEDF8).
 *
 * The decomp keeps GSscratchInit as the developers' own assembly
 * (src/game/gs_scratch_exact_800EEDF8.c, docs/asm_evidence/gs_scratch.md),
 * so that unit cannot be built for ARM. This is a line-by-line C reading of
 * that assembly:
 *
 *   - save the caller's return address (lbl_8047ABE4), clear the block mask
 *     (lbl_8047ABEC);
 *   - mark the 32 allocation records free: byte 0 of each 8-byte record
 *     at lbl_804018F0 = 0xFF;
 *   - enable the locked cache and point the scratch base (lbl_8047ABE0) at
 *     it; lbl_8047ABD8 = reservedBlocks * 0x200;
 *   - if reservedBlocks > 0: set that many bits from the top of the block
 *     mask, record 0 = { 0, reservedBlocks }; then move the caller's stack
 *     into the top of the reserved area and return straight to the caller;
 *   - clear lbl_8047ABE8 and return TRUE.
 *
 * The stack move cannot be expressed in C. The game calls GSscratchInit(0)
 * from main(), which never takes that path, so here a nonzero argument stops
 * with a message rather than running on with the wrong stack.
 *
 * The locked cache (16 KB at 0xE0000000 on the GameCube) is a plain buffer,
 * see platform/src/os/lc.c.
 */
#include <3ds.h>
#include <stdio.h>

extern unsigned char lbl_804018F0[];
extern unsigned char* lbl_8047ABE0;
extern unsigned long lbl_8047ABE4;
extern unsigned char lbl_8047ABE8;
extern unsigned long lbl_8047ABEC;
extern unsigned long lbl_8047ABD8;

extern void LCEnable(void);
extern unsigned char* __port_locked_cache(void);

int GSscratchInit(unsigned char reservedBlocks)
{
    lbl_8047ABE4 = (unsigned long)__builtin_return_address(0);
    lbl_8047ABEC = 0;
    for (int i = 0; i < 32; i++)
        lbl_804018F0[i * 8] = 0xFF;

    LCEnable();
    lbl_8047ABD8 = (unsigned long)reservedBlocks << 9;
    lbl_8047ABE0 = __port_locked_cache();

    if (reservedBlocks != 0) {
        unsigned long bit = 0x80000000u;
        for (unsigned n = reservedBlocks; n != 0; n--) {
            lbl_8047ABEC |= bit;
            bit >>= 1;
        }
        lbl_804018F0[0] = 0;
        lbl_804018F0[1] = reservedBlocks;

        /* Here the original moves the caller's stack into the locked cache. */
        char line[64];
        int len = snprintf(line, sizeof(line), "port: GSscratchInit(%u) needs a stack move", reservedBlocks);
        svcOutputDebugString(line, len);
        svcBreak(USERBREAK_PANIC);
    }

    lbl_8047ABE8 = 0;
    return 1;
}
