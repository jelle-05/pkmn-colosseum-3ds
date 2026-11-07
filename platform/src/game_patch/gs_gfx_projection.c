/*
 * gs_gfx_projection.c - projection setters that pass floats through registers.
 *
 * On the GameCube these rely on float arguments staying in f1-f4 from the
 * caller to the next call, so the decomp writes them without passing them:
 *
 *     void GSmtx44Perspective(u8* p) { C_MTXPerspective(); }
 *     void gfxSetPerspective(f32 a, f32 b, f32 c, f32 d) {
 *         if (recording) gfxRecordCommand(0x39, 0xe, a);   // format 14: four floats
 *         else { u8 tmp[0x48]; GSmtx44Perspective(tmp); GXSetProjection(tmp, 0); }
 *     }
 *
 * That matches MWCC's code, but in C on ARM the floats are gone by then
 * (the text camera's aspect ratio arrived as 0). These versions pass every
 * argument. src/game/gs_math_mtx44.c and gs_gfx_range_800D9AF0.c.
 */
typedef float f32;
typedef int s32;
typedef unsigned char u8;

extern void C_MTXPerspective(void* m, f32 fovy, f32 aspect, f32 near, f32 far);
extern void C_MTXOrtho(void* m, f32 top, f32 bottom, f32 left, f32 right, f32 near, f32 far);
extern void GXSetProjection(const void* m, int type);
extern void gfxRecordCommand(unsigned command, unsigned format, ...);
extern void gfxLoadIdentity(void);
extern s32* lbl_8047AA80;                 /* GSgfx state; word 0: 1 while recording */
extern f32 lbl_8047CA50, lbl_8047CA54;   /* the 2D near and far planes */

void GSmtx44Perspective(void* p, f32 fovy, f32 aspect, f32 near, f32 far)
{
    C_MTXPerspective(p, fovy, aspect, near, far);
}

void GSmtx44Ortho(void* p, f32 top, f32 bottom, f32 left, f32 right, f32 near, f32 far)
{
    C_MTXOrtho(p, top, bottom, left, right, near, far);
}

/* The recorded command holds all four floats (format 14); the replay
 * (src/game/gs_gfx_range_800D3FA4.c) passes them back the same way. */
void gfxSetPerspective(f32 fovy, f32 aspect, f32 near, f32 far)
{
    if (*lbl_8047AA80 == 1) {
        gfxRecordCommand(0x39, 0xe, (double)fovy, (double)aspect, (double)near, (double)far);
    } else {
        u8 tmp[0x48] __attribute__((aligned(8)));
        GSmtx44Perspective(tmp, fovy, aspect, near, far);
        GXSetProjection(tmp, 0);
    }
}

void gfxSetOrtho(f32 a, f32 b, f32 c, f32 d)
{
    if (*lbl_8047AA80 == 1) {
        gfxRecordCommand(0x3a, 0xe, (double)a, (double)b, (double)c, (double)d);
    } else {
        u8 tmp[0x48] __attribute__((aligned(8)));
        GSmtx44Ortho(tmp, b, d, a, c, lbl_8047CA50, lbl_8047CA54);
        GXSetProjection(tmp, 1);
        gfxLoadIdentity();
    }
}
