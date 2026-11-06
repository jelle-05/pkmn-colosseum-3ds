/*
 * gx_types.h - GameCube GX types as the platform layer sees them.
 *
 * Sizes and layouts match the Dolphin SDK, because game code embeds these
 * objects in its own structures and some SDK code is reused unchanged
 * (GXInitTexObj and the light initialisers fill the objects in their
 * GameCube layout; platform/reuse.txt). Do not include libctru headers here:
 * a few names would clash.
 */
#ifndef PORT_GX_TYPES_H
#define PORT_GX_TYPES_H

typedef unsigned char gx_u8;
typedef signed char gx_s8;
typedef unsigned short gx_u16;
typedef signed short gx_s16;
typedef unsigned long gx_u32;
typedef signed long gx_s32;
typedef float gx_f32;

typedef struct { gx_u8 r, g, b, a; } GXColor;
typedef struct { gx_s16 r, g, b, a; } GXColorS10;

typedef struct { gx_u32 data[8]; } GXTexObj;      /* 0x20 */
typedef struct { gx_u32 data[3]; } GXTlutObj;     /* 0x0C */
typedef struct { gx_u32 data[16]; } GXLightObj;   /* 0x40 */
typedef struct { gx_u8 data[128]; } GXFifoObj;    /* 0x80 */
typedef struct { gx_u16 r[10]; } GXFogAdjTable;

typedef struct {
    gx_u32 viTVmode;
    gx_u16 fbWidth;
    gx_u16 efbHeight;
    gx_u16 xfbHeight;
    gx_u16 viXOrigin;
    gx_u16 viYOrigin;
    gx_u16 viWidth;
    gx_u16 viHeight;
    gx_u32 xFBmode;
    gx_u8 field_rendering;
    gx_u8 aa;
    gx_u8 sample_pattern[12][2];
    gx_u8 vfilter[7];
} GXRenderModeObj;                                  /* 0x3C */

typedef gx_f32 GXMtx[3][4];
typedef gx_f32 GXMtx44[4][4];

typedef void (*GXDrawDoneCallback)(void);
typedef void (*GXDrawSyncCallback)(gx_u16 token);

/* Limits and a few enum values the platform code needs. */
#define GX_MAX_VTXFMT 8
#define GX_MAX_ATTR 26          /* GX_VA_PNMTXIDX .. GX_VA_NBT */
#define GX_MAX_TEVSTAGE 16
#define GX_MAX_TEXMAP 8
#define GX_MAX_TEXCOORD 8
#define GX_MAX_INDSTAGE 4
#define GX_MAX_LIGHT 8
#define GX_MAX_TLUT 20
#define GX_PNMTX_COUNT 10       /* position/normal matrices 0, 3, ..., 27 */
#define GX_TEXMTX_COUNT 10      /* texture matrices 30, 33, ..., 57 */
#define GX_PTTEXMTX_COUNT 20    /* post-transform matrices 64, 67, ..., 121 */

#define GXENUM_PERSPECTIVE 0
#define GXENUM_ORTHOGRAPHIC 1

#endif /* PORT_GX_TYPES_H */
