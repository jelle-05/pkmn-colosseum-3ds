/*
 * menu_shop_varargs.c - fn_80029FAC, fn_8002A0B8, fn_8002A1C4, fn_8002A2CC.
 *
 * The decomp's versions (src/game/menuShop_exact_80029FAC.c) fill in MWCC's
 * PowerPC va_list by hand (gpr count, overflow and save-area pointers) to
 * match the retail code, then read it with __va_arg. That layout means
 * nothing on ARM, so these are the same functions with standard varargs.
 *
 * All four take a list of s32 values ending at the first negative one,
 * read as (key, value) pairs for msgctrlSetValue, and look up a message
 * entry through a shop table: entry = table + map[index * 4] * stride, its
 * first byte a sound or window id, then u32 message ids from +4.
 *
 *   fn_80029FAC, fn_8002A0B8: store the entry's first byte to *out and
 *     return message id number `which`. Tables lbl_80478E4C (0x4C-byte
 *     entries) and lbl_80478E3C (0x3C-byte entries).
 *   fn_8002A1C4, fn_8002A2CC: open that message with the entry's first byte
 *     as the sound (winMsgOpenWithSE), then close window 1.
 */
#include <stdarg.h>

typedef unsigned char u8;
typedef unsigned long u32;
typedef long s32;

extern u32 lbl_80478E54;   /* u8* index map */
extern u32 lbl_80478E4C;   /* u8* table, 0x4C-byte entries */
extern u32 lbl_80478E3C;   /* u8* table, 0x3C-byte entries */

extern void msgctrlSetValue(s32 key, s32 value);
extern void winMsgOpenWithSE(u32 a, u32 msg, u32 b, u32 c, u8 se);
extern void winMsgClose(u32 window);

static void set_pairs(s32 v, va_list ap)
{
    int expect_key = 1;
    s32 key = 0;
    while (v >= 0) {
        if (expect_key) {
            key = v;
            expect_key = 0;
        } else {
            expect_key = 1;
            msgctrlSetValue(key, v);
        }
        v = va_arg(ap, s32);
    }
}

static u8* entry_of(u32 table, s32 index, u32 stride)
{
    u8* map = (u8*)lbl_80478E54;
    return (u8*)table + map[index << 2] * stride;
}

u32 fn_80029FAC(u8* out, s32 index, s32 which, s32 first, ...)
{
    va_list ap;
    u8* e = entry_of(lbl_80478E4C, index, 0x4C);
    *out = e[0];
    va_start(ap, first);
    set_pairs(first, ap);
    va_end(ap);
    return *(u32*)(e + 4 + (which << 2));
}

u32 fn_8002A0B8(u8* out, s32 index, s32 which, s32 first, ...)
{
    va_list ap;
    u8* e = entry_of(lbl_80478E3C, index, 0x3C);
    *out = e[0];
    va_start(ap, first);
    set_pairs(first, ap);
    va_end(ap);
    return *(u32*)(e + 4 + (which << 2));
}

void fn_8002A1C4(s32 index, s32 which, s32 first, ...)
{
    va_list ap;
    u8* e = entry_of(lbl_80478E4C, index, 0x4C);
    va_start(ap, first);
    set_pairs(first, ap);
    va_end(ap);
    winMsgOpenWithSE(2, *(u32*)(e + 4 + (which << 2)), 1, 0, e[0]);
    winMsgClose(1);
}

void fn_8002A2CC(s32 index, s32 which, s32 first, ...)
{
    va_list ap;
    u8* e = entry_of(lbl_80478E3C, index, 0x3C);
    va_start(ap, first);
    set_pairs(first, ap);
    va_end(ap);
    winMsgOpenWithSE(2, *(u32*)(e + 4 + (which << 2)), 1, 0, e[0]);
    winMsgClose(1);
}
