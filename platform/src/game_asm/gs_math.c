/*
 * gs_math.c - C version of GSmathInit (0x800E0790, src/game/gs_math_init.c).
 *
 * The original is the developers' assembly (docs/asm_evidence/
 * gs_math_init.md) because it writes the Gekko's GQR2-GQR5 registers:
 * GQRn = n | n << 16, which make paired-single quantized loads and stores
 * use type n for both directions (GQR2 u8, GQR3 u16, GQR4 s8, GQR5 s16, each
 * with scale 0). The ARM build has no paired singles and no quantized
 * loads, so those writes have nothing to configure. What remains are the
 * two calls.
 */
extern void fn_800E0C78(void);
extern void GSmathInitCosTable(void);

void GSmathInit(void)
{
    fn_800E0C78();
    GSmathInitCosTable();
}
