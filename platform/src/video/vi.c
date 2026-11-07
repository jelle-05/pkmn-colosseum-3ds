/*
 * vi.c - the video interface (VI).
 *
 * On the GameCube, VI scans an XFB out to the TV and raises an interrupt at
 * every retrace. In that interrupt the SDK runs the pre- and post-retrace
 * callbacks (HSD's video.c swaps framebuffers there, the game's gapp layer
 * runs its per-vsync tasks), samples the controllers, and wakes the threads
 * waiting in VIWaitForRetrace.
 *
 * Here a vblank thread waits for the 3DS top screen's vblank and posts that
 * retrace interrupt to the interrupt thread (os/os_irq.c), which runs it with
 * interrupts disabled. The 3DS presents what GXCopyDisp closed, so the
 * framebuffer the game hands to VISetNextFrameBuffer is only remembered.
 */
#include "port/host.h"
#include "port/prof.h"

#include "port/gx_types.h"
#include "../os/os_thread.h"

typedef void (*VIRetraceCallback)(gx_u32 retrace_count);

extern void __port_gx_deliver(void);
extern void __port_pad_sample(void);
extern long long OSGetTime(void);

static VIRetraceCallback pre_cb, post_cb;
static volatile gx_u32 retrace_count;
static OSThreadQueue retrace_queue;
static void* next_fb;
static void* current_fb;
static int black = 1;
static GXRenderModeObj mode;
static int vblank_running;

/* The retrace interrupt, run by the interrupt thread. */
static void retrace(void* unused, long unused2)
{
    (void)unused; (void)unused2;
    retrace_count++;
    if (pre_cb)
        pre_cb(retrace_count);
    current_fb = next_fb;
    __port_pad_sample();
    __port_gx_deliver();
    if (post_cb)
        post_cb(retrace_count);
    __port_wakeup(&retrace_queue);
}

static void vblank_thread(void* unused)
{
    (void)unused;
    unsigned beats = 0;
    for (;;) {
        host_wait_vblank();
        __port_irq_post(OSGetTime(), retrace, 0, 0);
        /* Heartbeat: the first 20 seconds, every 2 seconds. */
        if (++beats % 120 == 0 && beats <= 1200) {
            extern void __port_log(const char* fmt, ...);
            __port_log("heartbeat %u: %u retraces delivered", beats, (unsigned)retrace_count);
            __port_thread_dump();
        }
    }
}

void VIInit(void)
{
    host_display_init();
    if (!vblank_running) {
        OSInitThreadQueue(&retrace_queue);
        if (!host_thread_create(vblank_thread, 0, 8 * 1024, PRIO_IRQ | 1 << 8))
            host_break(0);
        vblank_running = 1;
    }
}

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
    int level = OSDisableInterrupts();
    VIRetraceCallback old = pre_cb;
    pre_cb = cb;
    OSRestoreInterrupts(level);
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback cb)
{
    int level = OSDisableInterrupts();
    VIRetraceCallback old = post_cb;
    post_cb = cb;
    OSRestoreInterrupts(level);
    return old;
}

extern PortThread* __port_self(void);

void VIWaitForRetrace(void)
{
    /* The HOME menu and power events are handled on the main thread. */
    static int main_checked;
    if (!main_checked || __port_self()->thread == 0) {
        main_checked = 1;
        if (__port_self()->thread == 0 && !host_main_loop())
            host_exit(0);
    }
    PROF_BEGIN(t_wait);
    int level = OSDisableInterrupts();
    gx_u32 count = retrace_count;
    do {
        __port_sleep_on(&retrace_queue);
    } while (count == retrace_count);
    OSRestoreInterrupts(level);
    PROF_END(PROF_RETRACE_WAIT, t_wait);
}
