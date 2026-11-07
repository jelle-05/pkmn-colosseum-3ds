/*
 * gpu.c - the host's renderer: draws what the GX layer prepared.
 *
 * The GX layer (big-endian, platform/src/gx/gx_draw.c) writes transformed
 * vertices into this file's vertex buffer and describes each draw in a
 * GpuDraw (port/gpu.h): textures, combiner stages, alpha test, blending,
 * depth. Here those become citro3d calls. Textures are created here and
 * filled by the GX layer in the PICA's tiled RGBA8 layout.
 */
#include <3ds.h>
#include <citro3d.h>
#include <string.h>

#include "port/gpu.h"
#include "port/host.h"

extern const u8 vshader_shbin[];
extern const u32 vshader_shbin_size;

#define MAX_VERTICES 65536
#define MAX_TEXTURES 1024

static DVLB_s* dvlb;
static shaderProgram_s program;
static int u_tilt;
static GpuVertex* vbuf;
static C3D_Tex textures[MAX_TEXTURES];
static u8 tex_used[MAX_TEXTURES];
static int ready;
static u32 vbuf_used;              /* highest vertex drawn this frame */
static GpuDraw last;                /* the state set by the previous draw */
static int have_last;

void host_gpu_init(void)
{
    if (ready)
        return;
    dvlb = DVLB_ParseFile((u32*)vshader_shbin, vshader_shbin_size);
    shaderProgramInit(&program);
    shaderProgramSetVsh(&program, &dvlb->DVLE[0]);
    u_tilt = shaderInstanceGetUniformLocation(program.vertexShader, "tilt");
    vbuf = linearAlloc(MAX_VERTICES * sizeof(GpuVertex));
    ready = 1;
}

/* Called at the start of every frame, after C3D_FrameBegin. */
void host_gpu_frame(void)
{
    host_gpu_init();
    C3D_BindProgram(&program);

    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    AttrInfo_Init(attr);
    AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);          /* position */
    AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 4);  /* colour */
    AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);          /* texcoord 0 */
    AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(attr, 4, GPU_FLOAT, 2);

    C3D_BufInfo* buf = C3D_GetBufInfo();
    BufInfo_Init(buf);
    BufInfo_Add(buf, vbuf, sizeof(GpuVertex), 5, 0x43210);

    /* Landscape clip space to the top screen's portrait framebuffer:
     * the screen's x runs along the framebuffer's y. */
    C3D_Mtx tilt;
    Mtx_Identity(&tilt);
    tilt.r[0].x = 0.0f; tilt.r[0].y = 1.0f;
    tilt.r[1].x = -1.0f; tilt.r[1].y = 0.0f;
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_tilt, &tilt);

    /* GX depth: z/w runs from -1 at the near plane to 0 at the far one. */
    C3D_DepthMap(true, 1.0f, 1.0f);
    have_last = 0;                  /* the frame starts with citro3d's state */
}

/* Before C3D_FrameEnd: the GPU reads this frame's vertices from here. */
void host_gpu_frame_end(void)
{
    if (vbuf_used)
        GSPGPU_FlushDataCache(vbuf, vbuf_used * sizeof(GpuVertex));
    vbuf_used = 0;
}

void* host_gpu_vertices(void) { host_gpu_init(); return vbuf; }
host_u32 host_gpu_capacity(void) { return MAX_VERTICES; }

/* ---- textures ------------------------------------------------------------------- */

host_u32 host_gpu_tex_create(host_u32 width, host_u32 height)
{
    for (int i = 1; i < MAX_TEXTURES; i++) {
        if (!tex_used[i]) {
            if (!C3D_TexInit(&textures[i], width, height, GPU_RGBA8))
                return 0;
            tex_used[i] = 1;
            return i;
        }
    }
    return 0;
}

void* host_gpu_tex_data(host_u32 handle)
{
    return handle && handle < MAX_TEXTURES && tex_used[handle] ? textures[handle].data : 0;
}

void host_gpu_tex_upload(host_u32 handle)
{
    if (handle && handle < MAX_TEXTURES && tex_used[handle])
        C3D_TexFlush(&textures[handle]);
}

void host_gpu_tex_free(host_u32 handle)
{
    if (handle && handle < MAX_TEXTURES && tex_used[handle]) {
        C3D_TexDelete(&textures[handle]);
        tex_used[handle] = 0;
    }
}

/* ---- drawing -------------------------------------------------------------------- */

void host_gpu_draw(const void* draw)
{
    const GpuDraw* d = draw;
    if (!ready || !d->count || d->first + d->count > MAX_VERTICES)
        return;
    if (d->first + d->count > vbuf_used)
        vbuf_used = d->first + d->count;

    /* Only what changed since the previous draw. */
    u32 off = (u32)((const char*)&d->unit[0] - (const char*)d);
    if (have_last && memcmp((const char*)&last + off, (const char*)d + off, sizeof(GpuDraw) - off) == 0) {
        C3D_DrawArrays(GPU_TRIANGLES, d->first, d->count);
        return;
    }
    last = *d;
    have_last = 1;

    for (int u = 0; u < GPU_MAX_UNITS; u++) {
        u32 h = d->unit[u].tex;
        if (h && h < MAX_TEXTURES && tex_used[h]) {
            C3D_Tex* t = &textures[h];
            C3D_TexSetWrap(t, d->unit[u].wrap_s, d->unit[u].wrap_t);
            C3D_TexSetFilter(t, d->unit[u].mag, d->unit[u].min);
            C3D_TexBind(u, t);
        }
    }

    for (int s = 0; s < GPU_MAX_STAGES; s++) {
        C3D_TexEnv* env = C3D_GetTexEnv(s);
        C3D_TexEnvInit(env);
        if (s < d->num_stages) {
            const GpuStage* g = &d->stage[s];
            C3D_TexEnvSrc(env, C3D_RGB, g->src_rgb[0], g->src_rgb[1], g->src_rgb[2]);
            C3D_TexEnvSrc(env, C3D_Alpha, g->src_a[0], g->src_a[1], g->src_a[2]);
            C3D_TexEnvOpRgb(env, g->op_rgb[0], g->op_rgb[1], g->op_rgb[2]);
            C3D_TexEnvOpAlpha(env, g->op_a[0], g->op_a[1], g->op_a[2]);
            C3D_TexEnvFunc(env, C3D_RGB, g->func_rgb);
            C3D_TexEnvFunc(env, C3D_Alpha, g->func_a);
            C3D_TexEnvScale(env, C3D_RGB, g->scale_rgb);
            C3D_TexEnvScale(env, C3D_Alpha, g->scale_a);
            C3D_TexEnvColor(env, g->color);
        } else {
            C3D_TexEnvSrc(env, C3D_Both, GPU_PREVIOUS, 0, 0);
            C3D_TexEnvFunc(env, C3D_Both, GPU_REPLACE);
        }
    }

    C3D_AlphaTest(d->alpha_test, d->alpha_func, d->alpha_ref);
    if (d->blend)
        C3D_AlphaBlend(d->blend_eq, d->blend_eq, d->blend_src, d->blend_dst, d->blend_src, d->blend_dst);
    else
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
    C3D_DepthTest(d->depth_test, d->depth_func, d->write_mask);
    C3D_CullFace(d->cull);

    C3D_DrawArrays(GPU_TRIANGLES, d->first, d->count);
}
