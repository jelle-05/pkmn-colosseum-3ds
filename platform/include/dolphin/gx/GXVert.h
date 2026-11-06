/*
 * Port override of decomp include/dolphin/gx/GXVert.h.
 *
 * On the GameCube, GXPosition3f32() and friends store straight into the
 * write-gather pipe at 0xCC008000. Here they append to the platform GX
 * layer's command buffer instead. The buffer is in native (little-endian)
 * order, because only our own GX layer reads it. Display lists that come
 * from game files are big-endian and take a separate path.
 */
#ifndef DOLPHIN_GX_GXVERT_H
#define DOLPHIN_GX_GXVERT_H

#include "dolphin/types.h"

/* Current write position, owned by platform/gx. */
extern u8* __GXFifoPtr;

static inline void __GXFifoU8(u8 v) { *__GXFifoPtr++ = v; }
static inline void __GXFifoU16(u16 v)
{
    __builtin_memcpy(__GXFifoPtr, &v, 2);
    __GXFifoPtr += 2;
}
static inline void __GXFifoU32(u32 v)
{
    __builtin_memcpy(__GXFifoPtr, &v, 4);
    __GXFifoPtr += 4;
}
static inline void __GXFifoF32(f32 v)
{
    __builtin_memcpy(__GXFifoPtr, &v, 4);
    __GXFifoPtr += 4;
}

#define __GXW_u8 __GXFifoU8
#define __GXW_u16 __GXFifoU16
#define __GXW_u32 __GXFifoU32
#define __GXW_f32 __GXFifoF32

#define GXVERT_FUNC_1PARAM(name, T) \
    static inline void name##1##T(const T x) { __GXW_##T(x); }
#define GXVERT_FUNC_2PARAM(name, T) \
    static inline void name##2##T(const T x, const T y) { __GXW_##T(x); __GXW_##T(y); }
#define GXVERT_FUNC_3PARAM(name, T)                                \
    static inline void name##3##T(const T x, const T y, const T z) \
    {                                                              \
        __GXW_##T(x); __GXW_##T(y); __GXW_##T(z);                  \
    }
#define GXVERT_FUNC_4PARAM(name, T)                                             \
    static inline void name##4##T(const T x, const T y, const T z, const T w)   \
    {                                                                           \
        __GXW_##T(x); __GXW_##T(y); __GXW_##T(z); __GXW_##T(w);                 \
    }
#define GXVERT_FUNC_INDEX8(name) \
    static inline void name##1x8(const u8 x) { __GXFifoU8(x); }
#define GXVERT_FUNC_INDEX16(name) \
    static inline void name##1x16(const u16 x) { __GXFifoU16(x); }

GXVERT_FUNC_3PARAM(GXPosition, f32)
GXVERT_FUNC_INDEX16(GXPosition)
GXVERT_FUNC_INDEX8(GXPosition)

GXVERT_FUNC_3PARAM(GXNormal, f32)
GXVERT_FUNC_INDEX16(GXNormal)
GXVERT_FUNC_INDEX8(GXNormal)

GXVERT_FUNC_4PARAM(GXColor, u8)
GXVERT_FUNC_1PARAM(GXColor, u32)
GXVERT_FUNC_3PARAM(GXColor, u8)
GXVERT_FUNC_1PARAM(GXColor, u16)
GXVERT_FUNC_INDEX16(GXColor)
GXVERT_FUNC_INDEX8(GXColor)

GXVERT_FUNC_2PARAM(GXTexCoord, f32)
GXVERT_FUNC_2PARAM(GXTexCoord, u8)
GXVERT_FUNC_1PARAM(GXTexCoord, u8)
GXVERT_FUNC_INDEX16(GXTexCoord)
GXVERT_FUNC_INDEX8(GXTexCoord)

GXVERT_FUNC_1PARAM(GXMatrixIndex, u8)

#undef GXVERT_FUNC_1PARAM
#undef GXVERT_FUNC_2PARAM
#undef GXVERT_FUNC_3PARAM
#undef GXVERT_FUNC_4PARAM
#undef GXVERT_FUNC_INDEX8
#undef GXVERT_FUNC_INDEX16

static inline void GXEnd(void) {}

#endif /* DOLPHIN_GX_GXVERT_H */
