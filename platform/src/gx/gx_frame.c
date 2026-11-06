/*
 * gx_frame.c - GX initialisation, EFB copies, primitives and synchronisation.
 *
 * Frame model. On the GameCube the game draws into the embedded framebuffer
 * (EFB), then GXCopyDisp copies the EFB to an external framebuffer (XFB) in
 * main memory, optionally clearing the EFB with the copy-clear colour for
 * the next frame. VI scans the XFB out at the next retrace.
 *
 * Here a 3DS frame opens on the first drawing after a copy and GXCopyDisp
 * closes it, which presents it. If that copy asked for a clear, the next
 * frame starts by clearing to the copy-clear colour and depth. XFB memory
 * is still allocated by the game but nothing is written to it; game code
 * that reads the XFB back would see that (none found so far).
 */
#include <string.h>

#include "gx_state.h"
#include "../video/display.h"

extern void __gx_state_defaults(void);
extern unsigned char* __GXFifoPtr;

static int pending_clear = 1;

static void ensure_frame(void)
{
    if (!display_frame_open()) {
        GXColor c = __gx.copy_clear_color;
        display_begin_frame(pending_clear, c.r, c.g, c.b, c.a, __gx.copy_clear_z);
        pending_clear = 0;
    }
}

void* GXInit(void* base, gx_u32 size)
{
    display_init();
    __gx_state_defaults();
    __gx.fifo_base = base;
    __gx.fifo_size = size;
    __GXFifoPtr = base;     /* immediate-mode vertex data lands here */
    return base;            /* the game keeps it as its GXFifoObj */
}

/* ---- copies ------------------------------------------------------------------ */

void GXSetCopyClear(GXColor color, gx_u32 z)
{
    __gx.copy_clear_color = color;
    __gx.copy_clear_z = z;
}

void GXSetDispCopySrc(gx_u16 left, gx_u16 top, gx_u16 wd, gx_u16 ht)
{
    __gx.disp_src[0] = left; __gx.disp_src[1] = top;
    __gx.disp_src[2] = wd; __gx.disp_src[3] = ht;
}

void GXSetDispCopyDst(gx_u16 wd, gx_u16 ht) { __gx.disp_dst_w = wd; __gx.disp_dst_h = ht; }
void GXSetDispCopyGamma(int gamma) { __gx.disp_gamma = (gx_u8)gamma; }
void GXSetCopyClamp(int clamp) { __gx.copy_clamp = (gx_u8)clamp; }

void GXSetCopyFilter(int aa, const gx_u8 pattern[12][2], int vf, const gx_u8 vfilter[7])
{
    (void)pattern; (void)vfilter;
    __gx.copy_aa = (gx_u8)aa;
    __gx.copy_vf = (gx_u8)vf;
}

/* Vertical scale between EFB and XFB lines. The game uses the standard
 * 480-line modes, where this is 1. */
gx_f32 GXGetYScaleFactor(gx_u16 efb_height, gx_u16 xfb_height)
{
    return efb_height ? (gx_f32)xfb_height / (gx_f32)efb_height : 1.0f;
}

/* Returns the number of XFB lines the copy produces, as the SDK does. */
gx_u32 GXSetDispCopyYScale(gx_f32 vscale)
{
    gx_u32 iscale = (gx_u32)(256.0f / vscale) & 0x1FF;
    gx_u32 ht = __gx.disp_src[3];
    __gx.disp_yscale = vscale;
    if (ht == 0 || iscale == 0)
        return ht;
    return ((ht - 1) * 256u) / iscale + 1;
}

void GXCopyDisp(void* dest, int clear)
{
    (void)dest;
    ensure_frame();             /* an empty frame still shows the clear */
    display_end_frame();
    pending_clear = clear;
}

void GXSetTexCopySrc(gx_u16 left, gx_u16 top, gx_u16 wd, gx_u16 ht)
{
    __gx.tex_src[0] = left; __gx.tex_src[1] = top;
    __gx.tex_src[2] = wd; __gx.tex_src[3] = ht;
}

void GXSetTexCopyDst(gx_u16 wd, gx_u16 ht, int fmt, int mipmap)
{
    __gx.tex_dst_w = wd; __gx.tex_dst_h = ht;
    __gx.tex_dst_fmt = (gx_u8)fmt; __gx.tex_dst_mipmap = (gx_u8)mipmap;
}

/* EFB-to-texture copies (render to texture) come with the renderer. */
void GXCopyTex(void* dest, int clear)
{
    (void)dest;
    if (clear)
        pending_clear = 1;
}

/* ---- primitives ------------------------------------------------------------------ */

void GXBegin(int type, int fmt, gx_u16 nverts)
{
    ensure_frame();
    __gx.prim_type = (gx_u8)type;
    __gx.prim_fmt = (gx_u8)fmt;
    __gx.prim_count = nverts;
    __GXFifoPtr = __gx.fifo_base;
}

void GXCallDisplayList(const void* list, gx_u32 nbytes)
{
    (void)list; (void)nbytes;
    ensure_frame();
}

/* ---- synchronisation -------------------------------------------------------------
 * The 3DS GPU work is synchronous to the frame for now, so the draw-done and
 * draw-sync events are delivered at the next point the game waits or a
 * retrace happens, like the GameCube's PE interrupts would be. */

GXDrawDoneCallback GXSetDrawDoneCallback(GXDrawDoneCallback cb)
{
    GXDrawDoneCallback old = __gx.draw_done_cb;
    __gx.draw_done_cb = cb;
    return old;
}

GXDrawSyncCallback GXSetDrawSyncCallback(GXDrawSyncCallback cb)
{
    GXDrawSyncCallback old = __gx.draw_sync_cb;
    __gx.draw_sync_cb = cb;
    return old;
}

void GXSetDrawDone(void) { __gx.draw_done_pending = 1; }

void GXSetDrawSync(gx_u16 token)
{
    __gx.draw_sync_token = token;
    __gx.draw_sync_pending = 1;
}

void __port_gx_deliver(void)
{
    if (__gx.draw_sync_pending) {
        __gx.draw_sync_pending = 0;
        if (__gx.draw_sync_cb)
            __gx.draw_sync_cb(__gx.draw_sync_token);
    }
    if (__gx.draw_done_pending) {
        __gx.draw_done_pending = 0;
        if (__gx.draw_done_cb)
            __gx.draw_done_cb();
    }
}

/* The draw-done callback runs in interrupt context on the GameCube. */
extern int OSDisableInterrupts(void);
extern int OSRestoreInterrupts(int level);

void GXWaitDrawDone(void)
{
    int level = OSDisableInterrupts();
    __port_gx_deliver();
    OSRestoreInterrupts(level);
}

void GXDrawDone(void)
{
    GXSetDrawDone();
    GXWaitDrawDone();
}

void GXFlush(void) {}
void GXPixModeSync(void) {}

/* GXSetGPMetric (unnamed in the decomp, src/dolphin/sdk_r52_800BD7A0_prefix.c):
 * selects the GPU performance counters. The game's gapp profiler sets them;
 * there are no such counters here. */
void fn_800BD91C(int perf0, int perf1) { (void)perf0; (void)perf1; }
