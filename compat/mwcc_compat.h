/*
 * mwcc_compat.h - make Metrowerks-flavoured decomp sources build with GCC.
 *
 * Force-included before every decomp unit (-include). Keep this file small:
 * it only neutralises compiler-specific syntax and provides the PowerPC
 * intrinsics the sources call. Anything that changes behaviour belongs in
 * platform/, and anything specific to one source file belongs in patches/.
 */
#ifndef MWCC_COMPAT_H
#define MWCC_COMPAT_H

/* __declspec(section ".sdata2") and friends only steer MWCC's section
 * placement, which the port does not care about. GCC ignores the matching
 * `#pragma section` lines with a warning. */
#define __declspec(x)

/* MWCC's register-pinning and alignment hints. */
#ifndef __MWERKS__
#define __option(x) 0
#endif

/* ---- PowerPC intrinsics ------------------------------------------------
 * Defined as static functions, not macros, because decomp headers also
 * declare some of them (`f64 __fabs(f64);`), and a later declaration of a
 * static function is valid C. */

static inline unsigned int __cntlzw(unsigned int x)
{
    return x ? (unsigned int)__builtin_clz(x) : 32u;
}

static inline double __fabs(double x) { return __builtin_fabs(x); }
static inline float __fabsf(float x) { return __builtin_fabsf(x); }
static inline double __fnabs(double x) { return -__builtin_fabs(x); }
static inline int __abs(int x) { return x < 0 ? -x : x; }
static inline long __labs(long x) { return x < 0 ? -x : x; }

/* frsqrte/fres are estimates with about 5 bits of precision on Gekko. The
 * game refines them with Newton-Raphson steps, so an exact value is fine. */
static inline double __frsqrte(double x) { return 1.0 / __builtin_sqrt(x); }
static inline float __fres(float x) { return 1.0f / x; }
static inline double __fsel(double a, double b, double c) { return a >= 0.0 ? b : c; }

/* Byte-reversed loads and stores: on a big-endian GameCube these read
 * little-endian data. On the little-endian 3DS they are plain accesses. */
static inline unsigned short __lhbrx(const void* p, int off)
{
    return *(const unsigned short*)((const char*)p + off);
}
static inline unsigned int __lwbrx(const void* p, int off)
{
    return *(const unsigned int*)((const char*)p + off);
}
static inline void __sthbrx(unsigned short v, void* p, int off)
{
    *(unsigned short*)((char*)p + off) = v;
}
static inline void __stwbrx(unsigned int v, void* p, int off)
{
    *(unsigned int*)((char*)p + off) = v;
}

/* Cache and pipeline control has no meaning for the game logic here. */
static inline void __sync(void) { __sync_synchronize(); }
static inline void __isync(void) {}
static inline void __eieio(void) {}
static inline void __dcbf(void* p, int off) { (void)p; (void)off; }
static inline void __dcbst(void* p, int off) { (void)p; (void)off; }
static inline void __dcbi(void* p, int off) { (void)p; (void)off; }
static inline void __dcbt(void* p, int off) { (void)p; (void)off; }
static inline void __icbi(void* p, int off) { (void)p; (void)off; }
static inline void __dcbz(void* p, int off)
{
    __builtin_memset((char*)p + off, 0, 32);
}

#endif /* MWCC_COMPAT_H */
