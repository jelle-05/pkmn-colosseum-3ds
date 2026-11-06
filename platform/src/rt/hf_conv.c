/*
 * hf_conv.c - 64-bit integer <-> floating-point helpers for hard float.
 *
 * The big-endian image links devkitARM's big-endian libgcc, which is built
 * for soft float: its conversions between 64-bit integers and float/double
 * pass the floating-point value in core registers, where this code has it
 * in VFP registers. These versions take precedence (the image's objects
 * come before libgcc on the link line). Each is built from 32-bit
 * conversions, which the VFP does in hardware, so none of them calls
 * itself or the libgcc version.
 */
typedef unsigned long long u64;
typedef long long s64;
typedef unsigned int u32;
typedef int s32;

#define TWO32 4294967296.0

/* hi * 2^32 is exact and lo is exact, so the sum is rounded once. */
double __aeabi_ul2d(u64 x) { return (double)(u32)(x >> 32) * TWO32 + (double)(u32)x; }
double __aeabi_l2d(s64 x) { return (double)(s32)(x >> 32) * TWO32 + (double)(u32)x; }
/* Through double: a second rounding, wrong only for rare ties. */
float __aeabi_ul2f(u64 x) { return (float)__aeabi_ul2d(x); }
float __aeabi_l2f(s64 x) { return (float)__aeabi_l2d(x); }

u64 __aeabi_d2ulz(double d)
{
    if (!(d > 0.0))
        return 0;                       /* also NaN */
    if (d >= TWO32 * TWO32)
        return ~0ull;
    u32 hi = (u32)(d / TWO32);          /* truncates */
    u32 lo = (u32)(d - (double)hi * TWO32);
    return (u64)hi << 32 | lo;
}

s64 __aeabi_d2lz(double d)
{
    if (d != d)
        return 0;
    if (d < 0.0) {
        if (d <= -9223372036854775808.0)
            return (s64)(1ull << 63);
        return -(s64)__aeabi_d2ulz(-d);
    }
    if (d >= 9223372036854775808.0)
        return (s64)~(1ull << 63);
    return (s64)__aeabi_d2ulz(d);
}

u64 __aeabi_f2ulz(float f) { return __aeabi_d2ulz((double)f); }
s64 __aeabi_f2lz(float f) { return __aeabi_d2lz((double)f); }
