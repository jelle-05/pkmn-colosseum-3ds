/*
 * gpu.c - the host's renderer: draws what the GX layer prepared.
 *
 * Two programs. vshader.v.pica takes vertices the GX layer transformed on
 * the CPU; tnl.v.pica takes them decoded but untransformed, with the GX
 * matrices, lights and texture coordinate rows in a uniform block
 * (GpuTnlUniforms) that the GX layer fills and this file uploads.
 *
 * The GX layer (big-endian, platform/src/gx/gx_draw.c) writes vertices
 * into this file's vertex buffers and describes each draw in a
 * GpuDraw (port/gpu.h): textures, combiner stages, alpha test, blending,
 * depth. Here those become citro3d calls. Textures are created here and
 * filled by the GX layer in the PICA's tiled RGBA8 layout.
 */
#include <3ds.h>
#include <citro3d.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "port/gpu.h"
#include "port/host.h"

extern const u8 vshader_shbin[];
extern const u32 vshader_shbin_size;
extern const u8 tnl_shbin[];
extern const u32 tnl_shbin_size;

#define MAX_VERTICES 65536            /* indices are 16 bits */
#define MAX_INDICES (MAX_VERTICES * 3)
#define MAX_TEXTURES 1024

static DVLB_s* dvlb;
static shaderProgram_s program;
static int u_tilt;
static GpuVertex* vbuf;

static DVLB_s* tnl_dvlb;
static shaderProgram_s tnl_program;
static int u_tnl, u_zclamp;
static GpuTnlVertex* tbuf;
static GpuTnlUniforms* tnl_unif;
static u32 tbuf_used;
static int cur_program = -1;          /* bound this frame, -1: none yet */
static u16* ibuf;
static C3D_Tex textures[MAX_TEXTURES];
static u8 tex_used[MAX_TEXTURES];
static int ready;
static u32 vbuf_used;              /* highest vertex drawn this frame */
static u32 ibuf_used;
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
    tnl_dvlb = DVLB_ParseFile((u32*)tnl_shbin, tnl_shbin_size);
    shaderProgramInit(&tnl_program);
    shaderProgramSetVsh(&tnl_program, &tnl_dvlb->DVLE[0]);
    u_tnl = shaderInstanceGetUniformLocation(tnl_program.vertexShader, "U");
    u_zclamp = shaderInstanceGetUniformLocation(tnl_program.vertexShader, "zclamp");
    tbuf = linearAlloc(MAX_VERTICES * sizeof(GpuTnlVertex));
    tnl_unif = malloc(sizeof(GpuTnlUniforms));
    memset(tnl_unif, 0, sizeof(*tnl_unif));
    vbuf = linearAlloc(MAX_VERTICES * sizeof(GpuVertex));
    ibuf = linearAlloc(MAX_INDICES * sizeof(u16));
    ready = 1;
}

/* Binds one of the two programs with its vertex layout and uniforms. The
 * programs share the PICA's uniform registers, so switching uploads all of
 * the one being bound. */
static void bind_program(int which)
{
    C3D_AttrInfo* attr = C3D_GetAttrInfo();
    C3D_BufInfo* buf = C3D_GetBufInfo();
    AttrInfo_Init(attr);
    BufInfo_Init(buf);
    if (which == GPU_PROGRAM_TNL) {
        C3D_BindProgram(&tnl_program);
        AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);          /* position, matrix row */
        AttrInfo_AddLoader(attr, 1, GPU_FLOAT, 3);          /* normal */
        AttrInfo_AddLoader(attr, 2, GPU_UNSIGNED_BYTE, 4);  /* colour */
        AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 3);          /* texture coordinate sources */
        AttrInfo_AddLoader(attr, 4, GPU_FLOAT, 3);
        AttrInfo_AddLoader(attr, 5, GPU_FLOAT, 3);
        BufInfo_Add(buf, tbuf, sizeof(GpuTnlVertex), 6, 0x543210);
        tnl_unif->dirty[0] = tnl_unif->dirty[1] = tnl_unif->dirty[2] = ~0u;
    } else {
        C3D_BindProgram(&program);
        AttrInfo_AddLoader(attr, 0, GPU_FLOAT, 4);          /* position */
        AttrInfo_AddLoader(attr, 1, GPU_UNSIGNED_BYTE, 4);  /* colour */
        AttrInfo_AddLoader(attr, 2, GPU_FLOAT, 2);          /* texcoord 0 */
        AttrInfo_AddLoader(attr, 3, GPU_FLOAT, 2);
        AttrInfo_AddLoader(attr, 4, GPU_FLOAT, 2);
        BufInfo_Add(buf, vbuf, sizeof(GpuVertex), 5, 0x43210);
        /* Landscape clip space to the top screen's portrait framebuffer:
         * the screen's x runs along the framebuffer's y. */
        C3D_Mtx tilt;
        Mtx_Identity(&tilt);
        tilt.r[0].x = 0.0f; tilt.r[0].y = 1.0f;
        tilt.r[1].x = -1.0f; tilt.r[1].y = 0.0f;
        C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, u_tilt, &tilt);
    }
    cur_program = which;
}

/* The changed parts of the transform shader's uniform block. */
static void upload_tnl_uniforms(void)
{
    for (int w = 0; w < 3; w++) {
        u32 bits = tnl_unif->dirty[w];
        if (!bits)
            continue;
        tnl_unif->dirty[w] = 0;
        for (int b = 0; b < 32; b++) {
            int i = w * 32 + b;
            if (!(bits & (1u << b)) || i >= GPU_TNL_UNIFORMS)
                continue;
            const float* v = tnl_unif->u[i];
            C3D_FVUnifSet(GPU_VERTEX_SHADER, u_tnl + i, v[0], v[1], v[2], v[3]);
        }
    }
}

/* Called at the start of every frame, after C3D_FrameBegin. */
void host_gpu_frame(void)
{
    host_gpu_init();
    bind_program(GPU_PROGRAM_CLIP);

    /* GX depth: z/w runs from -1 at the near plane to 0 at the far one. */
    C3D_DepthMap(true, 1.0f, 1.0f);
    have_last = 0;                  /* the frame starts with citro3d's state */
}
/* Before C3D_FrameEnd: the GPU reads this frame's vertices from here. */
void host_gpu_frame_end(void)
{
    if (vbuf_used)
        GSPGPU_FlushDataCache(vbuf, vbuf_used * sizeof(GpuVertex));
    if (tbuf_used)
        GSPGPU_FlushDataCache(tbuf, tbuf_used * sizeof(GpuTnlVertex));
    if (ibuf_used)
        GSPGPU_FlushDataCache(ibuf, ibuf_used * sizeof(u16));
    vbuf_used = ibuf_used = tbuf_used = 0;
}

void* host_gpu_vertices(void) { host_gpu_init(); return vbuf; }
void* host_gpu_indices(void) { host_gpu_init(); return ibuf; }
host_u32 host_gpu_capacity(void) { return MAX_VERTICES; }
host_u32 host_gpu_index_capacity(void) { return MAX_INDICES; }
void* host_gpu_tnl_vertices(void) { host_gpu_init(); return tbuf; }
host_u32 host_gpu_tnl_capacity(void) { return MAX_VERTICES; }
void* host_gpu_tnl_uniforms(void) { host_gpu_init(); return tnl_unif; }

/* ---- textures ------------------------------------------------------------------- */

/* format: a GPU_TEXCOLOR value (the GX layer picks the smallest that holds
 * the GX format). */
host_u32 host_gpu_tex_create(host_u32 width, host_u32 height, host_u32 format)
{
    for (int i = 1; i < MAX_TEXTURES; i++) {
        if (!tex_used[i]) {
            if (!C3D_TexInit(&textures[i], width, height, (GPU_TEXCOLOR)format))
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

extern volatile int host_quitting;

void host_gpu_draw(const void* draw)
{
    if (host_quitting)
        return;
    const GpuDraw* d = draw;
    if (!ready || !d->count || d->first + d->count > MAX_INDICES)
        return;
    if (d->first + d->count > ibuf_used)
        ibuf_used = d->first + d->count;
    if (d->program == GPU_PROGRAM_TNL) {
        if (d->vertices > tbuf_used)
            tbuf_used = d->vertices;
    } else if (d->vertices > vbuf_used) {
        vbuf_used = d->vertices;
    }
    if (d->program != cur_program)
        bind_program(d->program);
    if (d->program == GPU_PROGRAM_TNL)
        upload_tnl_uniforms();

    /* Only what changed since the previous draw (ugen, last, is not state). */
    u32 off = (u32)((const char*)&d->unit[0] - (const char*)d);
    u32 end = (u32)offsetof(GpuDraw, ugen);
    if (have_last && memcmp((const char*)&last + off, (const char*)d + off, end - off) == 0) {
        C3D_DrawElements(GPU_TRIANGLES, d->count, C3D_UNSIGNED_SHORT, &ibuf[d->first]);
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
    if (d->program == GPU_PROGRAM_TNL)
        C3D_BoolUnifSet(GPU_VERTEX_SHADER, u_zclamp, d->zclamp);

    C3D_DrawElements(GPU_TRIANGLES, d->count, C3D_UNSIGNED_SHORT, &ibuf[d->first]);
}
