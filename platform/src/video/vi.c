/*
 * vi.c - the video interface (VI).
 *
 * On the GameCube, VI scans an XFB out to the TV and raises an interrupt at
 * every retrace. Game code (HSD's video.c in particular) swaps framebuffers
 * from the pre- and post-retrace callbacks. Here VIWaitForRetrace waits for
 * the 3DS top screen's vblank and then runs those callbacks, together with
 * the other work the GameCube delivered from interrupts (DVD completions,
 * GX draw-done events). The 3DS presents what GXCopyDisp closed, so the
 * framebuffer the game hands to VISetNextFrameBuffer is only remembered.
 */
#include <stdlib.h>

#include "port/gx_types.h"
#include "display.h"

typedef void (*VIRetraceCallback)(gx_u32 retrace_count);

extern void __port_dvd_pump(void);
extern void __port_gx_deliver(void);
extern void __port_pad_sample(void);

static VIRetraceCallback pre_cb, post_cb;
static gx_u32 retrace_count;
static void* next_fb;
static void* current_fb;
static int black = 1;
static GXRenderModeObj mode;

void VIInit(void) { display_init(); }

void VIConfigure(const GXRenderModeObj* rm)
{
    if (rm)
        mode = *rm;
}

void VIConfigurePan(gx_u16 x, gx_u16 y, gx_u16 w, gx_u16 h) { (void)x; (void)y; (void)w; (void)h; }
void VIFlush(void) {}
void VISetNextFrameBuffer(void* fb) { next_fb = fb; }
void* VIGetNextFrameBuffer(void) { return next_fb; }
void* VIGetCurrentFrameBuffer(void) { return current_fb; }
void VISetBlack(int b) { black = b; }

gx_u32 VIGetRetraceCount(void) { return retrace_count; }
gx_u32 VIGetNextField(void) { return 1; }          /* VI_FIELD_ABOVE */
gx_u32 VIGetCurrentLine(void) { return 0; }
gx_u32 VIGetTvFormat(void) { return 0; }           /* VI_NTSC */
gx_u32 VIGetDTVStatus(void) { return 0; }          /* no component cable */

/* XFB rows are padded to 16 pixels. */
gx_u32 VIPadFrameBufferWidth(gx_u16 width) { return (gx_u32)(width + 15u) & ~15u; }

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback old = pre_cb;
    pre_cb = cb;
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    VIRetraceCallback old = post_cb;
    post_cb = cb;
    return old;
}

void VIWaitForRetrace(void)
{
    if (!display_wait_vblank())
        exit(0);            /* HOME menu close or power off */
    retrace_count++;
    if (pre_cb)
        pre_cb(retrace_count);
    current_fb = next_fb;
    __port_pad_sample();
    __port_gx_deliver();
    __port_dvd_pump();
    if (post_cb)
        post_cb(retrace_count);
}
