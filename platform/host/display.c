/*
 * display.c - citro3d setup, frames and vblank (host side).
 *
 * One render target for the top screen, 400x240. The GameCube's 640x480
 * EFB is mapped onto it by the renderer (to come), through the viewport.
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>

#include "port/host.h"

#define DISPLAY_TRANSFER_FLAGS                                                              \
    (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |        \
     GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
     GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))

static C3D_RenderTarget* top;
static int initialised;
static int frame_open;
static unsigned long frames;

void host_display_init(void)
{
    if (initialised)
        return;
    gfxInitDefault();
    C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    top = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    C3D_RenderTargetSetOutput(top, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
    initialised = 1;
}

/* rgba is 0xRRGGBBAA as citro3d takes it; z the GameCube's 24-bit depth. */
void host_frame_begin(int clear, host_u32 rgba, host_u32 z)
{
    host_display_init();
    if (frame_open)
        return;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    if (clear)
        C3D_RenderTargetClear(top, C3D_CLEAR_ALL, rgba, z & 0xFFFFFF);
    C3D_FrameDrawOn(top);
    frame_open = 1;
}

host_u32 host_frame_end(void)
{
    if (!frame_open)
        return frames;
    C3D_FrameEnd(0);
    frame_open = 0;
    frames++;

    /* A heartbeat for runs without a screen to look at. */
    if (frames == 1 || frames % 300 == 0) {
        FILE* f = fopen("sdmc:/colosseum-port-frames.log", "a");
        if (f) {
            fprintf(f, "frame %lu\n", frames);
            fclose(f);
        }
    }
    return frames;
}

void host_wait_vblank(void) { gspWaitForVBlank(); }
