/*
 * floor_data.c - the floorDataBios* accessors (0x80115280 - 0x80115CB4).
 *
 * The decomp has this unit only as assembly (build/GC6E01/asm/game/
 * floor_data.s). These are C versions written from that assembly, with
 * the same field offsets, checks and log calls. The floor record is the
 * decomp's FloorData (src/game/floor.c): 0x4C bytes, in the table at
 * lbl_80478FBC with its count at *lbl_80478FB8 (both set by common_rel's
 * prolog). Data is big-endian, as everywhere in the image, so the bitfield
 * in byte 0 is read as the original does: its top three bits.
 *
 * When the decomp decompiles floor_data.c, this file goes.
 */
#include "port/types.h"

extern void GSlogWrite(const char* fmt, ...);
extern u32 std092_getCurrentRoomID(void);
extern u16 fightGetFloorKind(void);

extern u32* lbl_80478FB8;               /* floor count */
extern u8* lbl_80478FBC;                /* floor table, 0x4C per floor */

/* Log formats (.rodata) and the function names they print (.data). */
extern const char lbl_80272608[], lbl_8027262C[], lbl_80272658[], lbl_80272680[],
    lbl_802726AC[], lbl_802726D4[];
extern const char lbl_8035B8A0[], lbl_8035B8B4[], lbl_8035B8CC[], lbl_8035B8E8[], lbl_8035B904[],
    lbl_8035B91C[], lbl_8035B938[], lbl_8035B950[], lbl_8035B96C[], lbl_8035B988[], lbl_8035B9A4[],
    lbl_8035B9C0[], lbl_8035B9DC[], lbl_8035B9F8[], lbl_8035BA10[], lbl_8035BA2C[], lbl_8035BA48[],
    lbl_8035BA60[], lbl_8035BA7C[], lbl_8035BA98[], lbl_8035BABC[], lbl_8035BAD8[], lbl_8035BAF4[],
    lbl_8035BB10[], lbl_8035BB30[], lbl_8035BB50[];

#define W(p, off) (*(u32*)((u8*)(p) + (off)))

/* The shadow block: floor +0x10 points at a pointer to it. */
static u8* shadow_block(u8* floor, const char* fn, int* failed)
{
    *failed = 1;
    if (!floor) {
        GSlogWrite(lbl_80272608, fn);
        return 0;
    }
    u32* q = (u32*)W(floor, 0x10);
    if (!q)
        return 0;
    return (u8*)q;
}

u32 floorDataBiosGetShadowReciveNum(u8* floor)
{
    int failed;
    u8* q = shadow_block(floor, lbl_8035BB50, &failed);
    if (!q)
        return 0;
    u8* r = (u8*)W(q, 0);
    if (!r) {
        GSlogWrite(lbl_8027262C, lbl_8035BB50);
        return 0;
    }
    u32 n = 0;
    for (u32 off = 0x8; off <= 0x24; off += 4)
        if (W(r, off))
            n++;
    return n;
}

u32 floorDataBiosGetShadowReciveID(u8* floor, u32 index)
{
    int failed;
    u8* q = shadow_block(floor, lbl_8035BB30, &failed);
    if (!q)
        return 0;
    if (index >= 8)
        return 0;
    u8* r = (u8*)W(q, 0);
    if (!r) {
        GSlogWrite(lbl_8027262C, lbl_8035BB30);
        return 0;
    }
    return W(r, 0x8 + index * 4);
}

static u32 shadow_word(u8* floor, const char* fn, u32 off)
{
    int failed;
    u8* q = shadow_block(floor, fn, &failed);
    if (!q)
        return 0;
    u8* r = (u8*)W(q, 0);
    if (!r) {
        GSlogWrite(lbl_8027262C, fn);
        return 0;
    }
    return W(r, off);
}

u32 floorDataBiosGetShadowLightID(u8* floor) { return shadow_word(floor, lbl_8035BB10, 0x4); }
u32 floorDataBiosGetSunResID(u8* floor) { return shadow_word(floor, lbl_8035BAF4, 0x0); }

void floorDataBiosSetMainSctID(u8* floor, u32 id)
{
    if (!floor)
        GSlogWrite(lbl_80272658, lbl_8035BAD8, id);
    else
        W(floor, 0x34) = id;
}

void floorDataBiosSetMapResID(u8* floor, u32 id)
{
    if (!floor)
        GSlogWrite(lbl_80272680, lbl_8035BABC, id);
    else
        W(floor, 0x8) = id;
}

/* floor + off points at a pointer to the list. */
static u32 list_ptr(u8* floor, const char* fn, u32 off)
{
    if (!floor) {
        GSlogWrite(lbl_80272608, fn);
        return 0;
    }
    u32* q = (u32*)W(floor, off);
    return q ? *q : 0;
}

u32 floorDataBiosGetFieldCameraListPtr(u8* floor) { return list_ptr(floor, lbl_8035BA98, 0x1C); }
u32 floorDataBiosGetPosListPtr(u8* floor) { return list_ptr(floor, lbl_8035BA7C, 0x18); }

/* The character list: floor +0x14 -> pointer -> { u32* count, u8* entries }. */
u8* floorDataBiosGetCharInfo(u8* floor, u32 index)
{
    if (!floor) {
        GSlogWrite(lbl_80272608, lbl_8035BA60);
        return 0;
    }
    u32* q = (u32*)W(floor, 0x14);
    if (!q)
        return 0;
    u8* list = (u8*)*q;
    u32* count = (u32*)W(list, 0);
    if (index >= *count)
        return 0;
    return (u8*)W(list, 4) + index * 0x24;
}

u32 floorDataBiosGetCharNum(u8* floor)
{
    if (!floor) {
        GSlogWrite(lbl_80272608, lbl_8035BA48);
        return 0;
    }
    u32* q = (u32*)W(floor, 0x14);
    if (!q)
        return 0;
    u8* list = (u8*)*q;
    return *(u32*)W(list, 0);
}

static u32 field(u8* floor, const char* fmt, const char* fn, u32 off)
{
    if (!floor) {
        GSlogWrite(fmt, fn);
        return 0;
    }
    return W(floor, off);
}

u32 floorDataBiosGetPostFunc(u8* f) { return field(f, lbl_80272608, lbl_8035BA2C, 0x44); }
u32 floorDataBiosGetMainFunc(u8* f) { return field(f, lbl_80272608, lbl_8035BA10, 0x40); }
u32 floorDataBiosGetPreFunc(u8* f) { return field(f, lbl_80272608, lbl_8035B9F8, 0x3C); }
u32 floorDataBiosGetPlaceName(u8* f) { return field(f, lbl_80272608, lbl_8035B9DC, 0x24); }
u32 floorDataBiosGetPostSctID(u8* f) { return field(f, lbl_80272608, lbl_8035B9C0, 0x38); }
u32 floorDataBiosGetMainSctID(u8* f) { return field(f, lbl_80272608, lbl_8035B9A4, 0x34); }
u32 floorDataBiosGetPreSctID(u8* f) { return field(f, lbl_80272608, lbl_8035B988, 0x30); }
u32 floorDataBiosGetModifySctID(u8* f) { return field(f, lbl_80272608, lbl_8035B96C, 0x2C); }
u32 floorDataBiosGetSoundSctID(u8* f) { return field(f, lbl_80272608, lbl_8035B950, 0x28); }
u32 floorDataBiosGetFloorID(u8* f) { return field(f, lbl_80272608, lbl_8035B938, 0x0C); }
u32 floorDataBiosGetFileGroupID(u8* f) { return field(f, lbl_80272608, lbl_8035B91C, 0x04); }
u32 floorDataBiosGetGroupID(u8* f) { return field(f, lbl_80272608, lbl_8035B904, 0x04); }
u32 floorDataBiosGetMapResID(u8* f) { return field(f, lbl_802726AC, lbl_8035B8CC, 0x08); }

/* The kind is the top three bits of byte 0. Kind 2 reads as 1 while the
 * battle's floor kind is 0. */
u32 floorDataBiosGetFloorKind(u8* floor)
{
    if (!floor) {
        GSlogWrite(lbl_80272608, lbl_8035B8E8);
        return 0;
    }
    if (((floor[0] >> 5) & 7) == 2 && (u16)fightGetFloorKind() == 0)
        return 1;
    return (floor[0] >> 5) & 7;
}

u32 floorDataBiosGetArea(u8* floor)
{
    if (!floor) {
        GSlogWrite(lbl_80272608, lbl_8035B8B4);
        return 0;
    }
    return floor[1];
}

u8* floorDataBiosGetPtr(u32 id)
{
    u32 n = *lbl_80478FB8;
    u8* f = lbl_80478FBC;
    for (u32 i = 0; i < n; i++, f += 0x4C)
        if (W(f, 0xC) == id)
            return f;
    GSlogWrite(lbl_802726D4, lbl_8035B8A0);
    return 0;
}

u8* floorDataBiosGetCurrentPtr(void) { return floorDataBiosGetPtr(std092_getCurrentRoomID()); }
