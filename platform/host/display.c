/*
 * display.c - citro3d setup, frames and vblank (host side).
 *
 * One render target for the top screen, 400x240. The GameCube's 640x480
 * EFB is mapped onto it by the renderer (to come), through the viewport.
 */
#include <3ds.h>
#include <citro3d.h>
#include <stdio.h>
#include <string.h>

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

/* A screenshot of the last frame, requested by host_dump_frame and taken at
 * the next frame start, when the GPU has finished that frame. */
static char dump_path[96];

void host_dump_frame(const char* path)
{
    strncpy(dump_path, path, sizeof(dump_path) - 1);
    dump_path[sizeof(dump_path) - 1] = 0;
}

/* The render target is 240x400, portrait, in the PICA's tiled RGBA8 (8x8
 * tiles, Morton order inside, bytes A, B, G, R). Written as a 400x240
 * landscape BMP. */
static void dump(void)
{
    const u8* fb = (const u8*)top->frameBuf.colorBuf;
    FILE* f = fopen(dump_path, "wb");
    dump_path[0] = 0;
    if (!f || !fb)
        return;
    const int W = 400, H = 240, row = W * 3;
    u8 header[54] = { 'B', 'M' };
    u32 size = 54 + row * H;
    memcpy(header + 2, &size, 4);
    header[10] = 54; header[14] = 40;
    memcpy(header + 18, &W, 4);
    memcpy(header + 22, &H, 4);
    header[26] = 1; header[28] = 24;
    fwrite(header, 1, 54, f);
    static u8 line[400 * 3];
    for (int sy = 0; sy < H; sy++) {             /* BMP rows bottom up */
        for (int sx = 0; sx < W; sx++) {
            /* Landscape (sx, sy from the top) in the portrait buffer: the
             * buffer's x runs down the screen, its y across it. */
            int x = sy, y = sx;
            u32 m = (x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3);
            u32 off = (((u32)(y >> 3) * (240 >> 3) + (x >> 3)) * 64 + m) * 4;
            u8* o = line + sx * 3;
            o[0] = fb[off + 1]; o[1] = fb[off + 2]; o[2] = fb[off + 3];      /* B, G, R */
        }
        fwrite(line, 1, row, f);
    }
    fclose(f);
}

/* rgba is 0xRRGGBBAA as citro3d takes it; z the GameCube's 24-bit depth. */
void host_frame_begin(int clear, host_u32 rgba, host_u32 z)
{
    host_display_init();
    if (frame_open)
        return;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    if (dump_path[0])
        dump();
    if (clear)
        C3D_RenderTargetClear(top, C3D_CLEAR_ALL, rgba, z & 0xFFFFFF);
    C3D_FrameDrawOn(top);
    extern void host_gpu_frame(void);
    host_gpu_frame();
    frame_open = 1;
}

host_u32 host_frame_end(void)
{
    if (!frame_open)
        return frames;
    extern void host_gpu_frame_end(void);
    host_gpu_frame_end();
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
