/*
 * si.c - the serial interface (SI): controller ports and the GBA link cable.
 *
 * Port 1 reports a standard controller, the others nothing. There is no
 * GBA link, so transfers fail; the game's GBA features (game/gba) see a
 * missing GBA and turn themselves off.
 */
typedef unsigned long u32;
typedef long s32;
typedef void (*SITypeAndStatusCallback)(s32 chan, u32 type);
typedef void (*SICallback)(s32 chan, u32 sr, void* context);

#define SI_GC_CONTROLLER 0x09000000u
#define SI_ERROR_NO_RESPONSE 0x00000008u

static u32 type_of(s32 chan) { return chan == 0 ? SI_GC_CONTROLLER : SI_ERROR_NO_RESPONSE; }

void SISetSamplingRate(u32 msec) { (void)msec; }
void SIRefreshSamplingRate(void) {}
u32 SIProbe(s32 chan) { return type_of(chan); }
u32 SIGetType(s32 chan) { return type_of(chan); }

u32 SIGetTypeAsync(s32 chan, SITypeAndStatusCallback cb)
{
    u32 type = type_of(chan);
    if (cb)
        cb(chan, type);
    return type;
}

int SITransfer(s32 chan, void* out, u32 out_bytes, void* in, u32 in_bytes, SICallback cb, long long delay)
{
    (void)chan; (void)out; (void)out_bytes; (void)in; (void)in_bytes; (void)cb; (void)delay;
    return 0;
}

int SIIsChanBusy(s32 chan) { (void)chan; return 0; }
