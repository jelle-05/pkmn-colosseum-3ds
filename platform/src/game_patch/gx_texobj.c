/*
 * gx_texobj.c - GXInitTexObj with room for the port's addresses.
 *
 * The SDK's GXInitTexObj (src/dolphin/sdk_range_800BA7C0.c, otherwise
 * reused as it is) keeps the image address as (address >> 5) in a 21-bit
 * register field: enough for the GameCube's 24 MB, not for the port's
 * heap at 0x08000000. This version is the same except that the field
 * keeps all of (address >> 5); the renderer (gx/gx_draw.c) reads it back
 * as image3 << 5. GXInitTexObjCI calls it and needs no change.
 */
#include <string.h>

typedef unsigned int u32;
typedef unsigned short u16;
typedef unsigned char u8;

typedef struct {
    u32 mode0, mode1, image0, image3;
    void* userData;
    u32 format, tlutName;
    u16 loadCount;
    u8 loadFormat, flags;
} TexObj;

#define FIELD(reg, size, shift, v) \
    ((reg) = ((reg) & ~(((1u << (size)) - 1) << (shift))) | (((u32)(v) & ((1u << (size)) - 1)) << (shift)))

void GXInitTexObj(void* object, void* image, u16 width, u16 height, u32 format, u32 wrap_s, u32 wrap_t,
                  u8 mipmap)
{
    TexObj* t = object;
    u32 row_shift, col_shift;

    memset(t, 0, 0x20);
    FIELD(t->mode0, 2, 0, wrap_s);
    FIELD(t->mode0, 2, 2, wrap_t);
    FIELD(t->mode0, 1, 4, 1);
    if (mipmap) {
        u32 max = width > height ? width : height, lod = 0;
        t->flags |= 1;
        t->mode0 = (t->mode0 & 0xFFFFFF1Fu) | ((format - 8u) <= 2u ? 0xA0u : 0xC0u);
        while ((max >>= 1) != 0)
            lod++;
        FIELD(t->mode1, 8, 8, (u8)(16.0f * lod));
    } else {
        t->mode0 = (t->mode0 & 0xFFFFFF1Fu) | 0x80u;
    }
    t->format = format;
    FIELD(t->image0, 10, 0, width - 1);
    FIELD(t->image0, 10, 10, height - 1);
    FIELD(t->image0, 4, 20, format & 0xF);
    t->image3 = (u32)image >> 5;                     /* the whole address */

    switch (format & 0xF) {
    case 0: case 8: t->loadFormat = 1; row_shift = 3; col_shift = 3; break;        /* I4, C4 */
    case 1: case 2: case 9: t->loadFormat = 2; row_shift = 3; col_shift = 2; break;
    case 3: case 4: case 5: case 10: t->loadFormat = 2; row_shift = 2; col_shift = 2; break;
    case 6: t->loadFormat = 3; row_shift = 2; col_shift = 2; break;                /* RGBA8 */
    case 14: t->loadFormat = 0; row_shift = 3; col_shift = 3; break;               /* CMPR */
    default: t->loadFormat = 2; row_shift = 2; col_shift = 2; break;
    }
    u32 rows = (width + (1u << row_shift) - 1) >> row_shift;
    u32 cols = (height + (1u << col_shift) - 1) >> col_shift;
    t->loadCount = (u16)((rows * cols) & 0x7FFF);
    t->flags |= 2;
}
