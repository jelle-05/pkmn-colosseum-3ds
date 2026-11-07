/*
 * card.c - the memory card (CARD) API, with no card in either slot.
 *
 * Every call answers as the SDK does when nothing is inserted:
 * CARD_RESULT_NOCARD, and asynchronous calls that fail at once return the
 * error without calling their callback. The game then runs without saving.
 *
 * Saving needs a memory card image on the SD card behind this API: the
 * directory and FAT of a GameCube card (the decomp has the SDK's code for
 * that, src/dolphin/card*), read and written through the host file
 * functions. That comes later; see docs/PLATFORM_TODO.txt.
 */
#include "port/types.h"

extern void __port_log(const char* fmt, ...);

#define CARD_RESULT_NOCARD (-3)

typedef void (*CARDCallback)(s32 chan, s32 result);

static int logged;

static s32 nocard(const char* what)
{
    if (logged < 8) {
        logged++;
        __port_log("CARD: %s: no card", what);
    }
    return CARD_RESULT_NOCARD;
}

void CARDInit(void) {}

s32 CARDProbeEx(s32 chan, s32* mem_size, s32* sector_size)
{
    (void)chan;
    if (mem_size)
        *mem_size = 0;
    if (sector_size)
        *sector_size = 0;
    return nocard("CARDProbeEx");
}

s32 CARDMountAsync(s32 chan, void* work, CARDCallback detach, CARDCallback attach)
{
    (void)chan; (void)work; (void)detach; (void)attach;
    return nocard("CARDMountAsync");
}

s32 CARDGetResultCode(s32 chan) { (void)chan; return CARD_RESULT_NOCARD; }
s32 CARDUnmount(s32 chan) { (void)chan; return CARD_RESULT_NOCARD; }
s32 CARDCancel(void* file) { (void)file; return CARD_RESULT_NOCARD; }

s32 CARDCheckAsync(s32 chan, CARDCallback cb) { (void)chan; (void)cb; return nocard("CARDCheckAsync"); }
s32 CARDFormatAsync(s32 chan, CARDCallback cb) { (void)chan; (void)cb; return nocard("CARDFormatAsync"); }

s32 CARDFreeBlocks(s32 chan, s32* bytes_free, s32* files_free)
{
    (void)chan;
    if (bytes_free)
        *bytes_free = 0;
    if (files_free)
        *files_free = 0;
    return CARD_RESULT_NOCARD;
}

s32 CARDGetSerialNo(s32 chan, u64* serial)
{
    (void)chan;
    if (serial)
        *serial = 0;
    return CARD_RESULT_NOCARD;
}

s32 CARDCreateAsync(s32 chan, const char* name, u32 size, void* file, CARDCallback cb)
{
    (void)chan; (void)name; (void)size; (void)file; (void)cb;
    return nocard("CARDCreateAsync");
}

s32 CARDOpen(s32 chan, const char* name, void* file)
{
    (void)chan; (void)name; (void)file;
    return nocard("CARDOpen");
}

s32 CARDClose(void* file) { (void)file; return CARD_RESULT_NOCARD; }

s32 CARDDeleteAsync(s32 chan, const char* name, CARDCallback cb)
{
    (void)chan; (void)name; (void)cb;
    return CARD_RESULT_NOCARD;
}

s32 CARDGetStatus(s32 chan, s32 file_no, void* stat)
{
    (void)chan; (void)file_no; (void)stat;
    return CARD_RESULT_NOCARD;
}

s32 CARDSetStatusAsync(s32 chan, s32 file_no, void* stat, CARDCallback cb)
{
    (void)chan; (void)file_no; (void)stat; (void)cb;
    return CARD_RESULT_NOCARD;
}

s32 CARDWriteAsync(void* file, const void* buf, s32 length, s32 offset, CARDCallback cb)
{
    (void)file; (void)buf; (void)length; (void)offset; (void)cb;
    return CARD_RESULT_NOCARD;
}

s32 CARDGetAttributes(s32 chan, s32 file_no, u8* attr)
{
    (void)chan; (void)file_no;
    if (attr)
        *attr = 0;
    return CARD_RESULT_NOCARD;
}

s32 __CARDEraseSector(s32 chan, u32 addr, CARDCallback cb)
{
    (void)chan; (void)addr; (void)cb;
    return CARD_RESULT_NOCARD;
}
