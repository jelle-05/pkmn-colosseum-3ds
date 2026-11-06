/*
 * gc_mem.h - fixed GameCube addresses, redirected.
 *
 * Force-included into every decomp unit, after compat/mwcc_compat.h.
 *
 * Game code reaches some GameCube memory through constant addresses:
 * OS globals in low memory (the bus clock at 0x800000F8), a scratch area at
 * 0x80001800, and the GPU write-gather pipe at 0xCC008000. Those addresses
 * do not exist on the 3DS. A user process cannot map 0x80000000, and real
 * hardware faults on the access (Azahar only logs it and carries on).
 *
 * tools/unify_prototypes.py rewrites such casts inside function bodies:
 *
 *     (T*)0x800000F8            ->  (T*)__GC_ADDR(0x800000F8)
 *     (T*)(x + 0x80000000)      ->  (T*)__GC_ADDR(x + 0x80000000)
 *     *(volatile u16*)0xCC008000 = v;  ->  __GXFifoU16(v);
 *
 * __GC_ADDR maps the first 16 KB of GameCube RAM (cached 0x80000000 and
 * uncached 0xC0000000) onto __gc_lowmem. Any other fixed address stops the
 * program with a message, so a bad access shows up in the emulator too.
 */
#ifndef PORT_GC_MEM_H
#define PORT_GC_MEM_H

#define GC_LOWMEM_SIZE 0x4000u

extern unsigned char __gc_lowmem[GC_LOWMEM_SIZE];
extern void __port_bad_address(unsigned long addr);

static inline void* __GC_ADDR(unsigned long addr)
{
    /* 0x8xxxxxxx is cached RAM, 0xCxxxxxxx the uncached mirror. */
    unsigned long off = addr & 0x3FFFFFFFu;
    if ((addr >> 30) >= 2u && off < GC_LOWMEM_SIZE)
        return __gc_lowmem + off;
    __port_bad_address(addr);
    return 0;
}

/* GX FIFO writes, owned by platform/gx. The buffer is native-endian because
 * only our own GX layer reads it. */
extern unsigned char* __GXFifoPtr;

static inline void __GXFifoU8(unsigned char v) { *__GXFifoPtr++ = v; }
static inline void __GXFifoU16(unsigned short v)
{
    __builtin_memcpy(__GXFifoPtr, &v, 2);
    __GXFifoPtr += 2;
}
static inline void __GXFifoU32(unsigned long v)
{
    __builtin_memcpy(__GXFifoPtr, &v, 4);
    __GXFifoPtr += 4;
}
static inline void __GXFifoF32(float v)
{
    __builtin_memcpy(__GXFifoPtr, &v, 4);
    __GXFifoPtr += 4;
}

#endif /* PORT_GC_MEM_H */
