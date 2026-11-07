/*
 * gx_draw.c - GX primitives to the host's renderer.
 *
 * On the GameCube the CPU hands vertices to the GPU, which transforms,
 * lights and textures them. Here the transform work is done on the CPU,
 * in GameCube terms: this file decodes the vertex stream (immediate-mode
 * data from the FIFO, or display lists from the game's files, both in the
 * GameCube's own byte order, which this image reads natively), applies the
 * position matrices, projection and viewport, the lighting channels and
 * the texture coordinate generators, and assembles triangles. The result
 * goes to the host's vertex buffer (port/gpu.h) in clip space for the
 * landscape screen, with the GX pixel state translated to the PICA200's
 * combiners, alpha test, blending and depth test.
 *
 * Simplifications (to revisit as the game shows a need):
 *  - one PICA combiner per TEV stage, at most six; GX's colour registers
 *    are read as the constants the game set, swap tables and indirect
 *    texturing are ignored;
 *  - lines and points are not drawn;
 *  - textures are decoded once per image address and never refreshed.
 *
 * The GameCube's 640x480 frame is shown 4:3 in the middle of the 400x240
 * top screen, at half size.
 */
#include <math.h>
#include <stddef.h>
#include <string.h>

#include "gx_state.h"
#include "port/gpu.h"
#include "port/host.h"
#include "port/gc_mem.h"
#include "port/prof.h"

extern void __port_log(const char* fmt, ...);
extern void __gx_ensure_frame(void);
extern unsigned __port_frame_number_peek(void);

/* ---- GameCube enums used here ------------------------------------------------ */

enum { GX_NONE = 0, GX_DIRECT = 1, GX_INDEX8 = 2, GX_INDEX16 = 3 };
enum {
    GX_VA_PNMTXIDX = 0, GX_VA_TEX0MTXIDX = 1, GX_VA_POS = 9, GX_VA_NRM = 10, GX_VA_CLR0 = 11,
    GX_VA_CLR1 = 12, GX_VA_TEX0 = 13, GX_VA_POS_MTX_ARRAY = 21, GX_VA_NRM_MTX_ARRAY = 22,
    GX_VA_TEX_MTX_ARRAY = 23, GX_VA_LIGHT_ARRAY = 24, GX_VA_NBT = 25,
};
enum { GX_U8 = 0, GX_S8 = 1, GX_U16 = 2, GX_S16 = 3, GX_F32 = 4 };
enum { GX_RGB565 = 0, GX_RGB8 = 1, GX_RGBX8 = 2, GX_RGBA4 = 3, GX_RGBA6 = 4, GX_RGBA8 = 5 };
enum { GX_QUADS = 0x80, GX_TRIANGLES = 0x90, GX_TRIANGLESTRIP = 0x98, GX_TRIANGLEFAN = 0xA0,
       GX_LINES = 0xA8, GX_LINESTRIP = 0xB0, GX_POINTS = 0xB8 };

/* ---- the vertex stream ----------------------------------------------------- */

#define MAX_PRIM_VERTS 0x10000

typedef struct {
    float pos[3], nrm[3];
    unsigned char clr[2][4];
    float tex[8][2];
    unsigned char pnmtx, texmtx[8];
    unsigned char has_nrm, has_clr[2], has_tex[8], has_pnmtx, has_texmtx[8];
} Vtx;

static unsigned rd16(const unsigned char* p) { return (unsigned)p[0] << 8 | p[1]; }
static unsigned rd32(const unsigned char* p)
{
    return (unsigned)p[0] << 24 | (unsigned)p[1] << 16 | (unsigned)p[2] << 8 | p[3];
}

static int comp_size(int type) { return type == GX_F32 ? 4 : type >= GX_U16 ? 2 : 1; }

static float comp(const unsigned char* p, int type, int frac)
{
    float scale = 1.0f / (float)(1u << frac);
    switch (type) {
    case GX_U8: return p[0] * scale;
    case GX_S8: return (signed char)p[0] * scale;
    case GX_U16: return rd16(p) * scale;
    case GX_S16: return (short)rd16(p) * scale;
    default: {
        unsigned u = rd32(p);
        float f;
        memcpy(&f, &u, 4);
        return f;
    }
    }
}

static int color_size(int type)
{
    switch (type) {
    case GX_RGB565: case GX_RGBA4: return 2;
    case GX_RGB8: case GX_RGBA6: return 3;
    default: return 4;
    }
}

static void color(const unsigned char* p, int type, unsigned char* out)
{
    unsigned v;
    switch (type) {
    case GX_RGB565:
        v = rd16(p);
        out[0] = (unsigned char)((v >> 11) * 255 / 31);
        out[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
        out[2] = (unsigned char)((v & 31) * 255 / 31);
        out[3] = 255;
        break;
    case GX_RGB8: case GX_RGBX8:
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = 255;
        break;
    case GX_RGBA4:
        v = rd16(p);
        out[0] = (unsigned char)((v >> 12) * 17); out[1] = (unsigned char)(((v >> 8) & 15) * 17);
        out[2] = (unsigned char)(((v >> 4) & 15) * 17); out[3] = (unsigned char)((v & 15) * 17);
        break;
    case GX_RGBA6:
        v = (unsigned)p[0] << 16 | (unsigned)p[1] << 8 | p[2];
        out[0] = (unsigned char)((v >> 18) * 255 / 63); out[1] = (unsigned char)(((v >> 12) & 63) * 255 / 63);
        out[2] = (unsigned char)(((v >> 6) & 63) * 255 / 63); out[3] = (unsigned char)((v & 63) * 255 / 63);
        break;
    default:
        out[0] = p[0]; out[1] = p[1]; out[2] = p[2]; out[3] = p[3];
        break;
    }
}

/* Size in bytes of one attribute's data in the array, given its format. */
static int attr_data_size(int attr, int fmt)
{
    int cnt = __gx.vtx_fmt[fmt][attr].cnt, type = __gx.vtx_fmt[fmt][attr].type;
    if (attr == GX_VA_POS)
        return (cnt ? 3 : 2) * comp_size(type);
    if (attr == GX_VA_NRM)
        return (cnt ? 9 : 3) * comp_size(type);
    if (attr == GX_VA_CLR0 || attr == GX_VA_CLR1)
        return color_size(type);
    return (cnt ? 2 : 1) * comp_size(type);              /* texture coordinates */
}

/* Bytes one vertex takes in the stream. */
static int vertex_size(int fmt)
{
    int n = 0;
    for (int a = GX_VA_PNMTXIDX; a <= GX_VA_TEX0 + 7; a++) {
        int d = __gx.vtx_desc[a];
        if (a < GX_VA_POS) {
            n += d ? 1 : 0;
        } else if (d == GX_DIRECT) {
            n += attr_data_size(a, fmt);
        } else if (d == GX_INDEX8) {
            n += 1;
        } else if (d == GX_INDEX16) {
            n += 2;
        }
    }
    return n;
}

/* The vertex layout of one draw: the attributes present, in stream order,
 * with what reading them needs. Built once per draw (layout_setup). */
typedef struct {
    unsigned char attr, desc, type, cnt, frac, cs, size;
    const unsigned char* base;
    unsigned stride;
} AttrIn;

static AttrIn layout[GX_VA_TEX0 + 8];
static int layout_n, layout_mtx_n;

static void layout_setup(int fmt)
{
    layout_n = 0;
    for (int a = GX_VA_PNMTXIDX; a < GX_VA_POS; a++)
        if (__gx.vtx_desc[a])
            layout[layout_n++] = (AttrIn){ (unsigned char)a, 1, 0, 0, 0, 1, 1, 0, 0 };
    layout_mtx_n = layout_n;
    for (int a = GX_VA_POS; a <= GX_VA_TEX0 + 7; a++) {
        int d = __gx.vtx_desc[a];
        if (!d)
            continue;
        AttrIn* l = &layout[layout_n++];
        l->attr = (unsigned char)a;
        l->desc = (unsigned char)d;
        l->type = __gx.vtx_fmt[fmt][a].type;
        l->cnt = __gx.vtx_fmt[fmt][a].cnt;
        l->frac = __gx.vtx_fmt[fmt][a].frac;
        l->cs = (unsigned char)comp_size(l->type);
        l->size = (unsigned char)attr_data_size(a, fmt);
        l->base = __gx.array[a].base;
        l->stride = __gx.array[a].stride;
    }
}

/* Decodes one vertex at *pp (in the layout set up for the draw) and
 * advances it. Only the flags are cleared: fields are read when present. */
static void read_vertex(const unsigned char** pp, int fmt, Vtx* v)
{
    (void)fmt;
    const unsigned char* p = *pp;
    memset(&v->has_nrm, 0, sizeof(*v) - offsetof(Vtx, has_nrm));
    int i = 0;
    for (; i < layout_mtx_n; i++) {
        int a = layout[i].attr;
        if (a == GX_VA_PNMTXIDX) { v->pnmtx = *p++; v->has_pnmtx = 1; }
        else { v->texmtx[a - GX_VA_TEX0MTXIDX] = *p++; v->has_texmtx[a - GX_VA_TEX0MTXIDX] = 1; }
    }
    for (; i < layout_n; i++) {
        const AttrIn* l = &layout[i];
        const unsigned char* src;
        if (l->desc == GX_DIRECT) {
            src = p;
            p += l->size;
        } else {
            unsigned idx = l->desc == GX_INDEX8 ? p[0] : rd16(p);
            p += l->desc == GX_INDEX8 ? 1 : 2;
            if (!l->base)
                continue;
            src = l->base + idx * l->stride;
        }
        int a = l->attr, type = l->type, frac = l->frac, cs = l->cs;
        if (a == GX_VA_POS) {
            v->pos[0] = comp(src, type, frac);
            v->pos[1] = comp(src + cs, type, frac);
            v->pos[2] = l->cnt ? comp(src + 2 * cs, type, frac) : 0.0f;
        } else if (a == GX_VA_NRM) {
            /* Normals have a fixed point: 6 bits for bytes, 14 for shorts. */
            int nf = type == GX_S8 ? 6 : type == GX_S16 ? 14 : 0;
            for (int k = 0; k < 3; k++)
                v->nrm[k] = comp(src + k * cs, type, nf);
            v->has_nrm = 1;
        } else if (a == GX_VA_CLR0 || a == GX_VA_CLR1) {
            color(src, type, v->clr[a - GX_VA_CLR0]);
            v->has_clr[a - GX_VA_CLR0] = 1;
        } else {
            int t = a - GX_VA_TEX0;
            v->tex[t][0] = comp(src, type, frac);
            v->tex[t][1] = l->cnt ? comp(src + cs, type, frac) : 0.0f;
            v->has_tex[t] = 1;
        }
    }
    *pp = p;
}
/* ---- transform and lighting -------------------------------------------- */

static const float* pos_matrix(unsigned id)
{
    unsigned i = id / 3;
    return &__gx.pos_mtx[i < GX_PNMTX_COUNT ? i : 0][0][0];
}

static void mul34(const float* m, const float* v, float w, float* out)
{
    for (int r = 0; r < 3; r++)
        out[r] = m[r * 4] * v[0] + m[r * 4 + 1] * v[1] + m[r * 4 + 2] * v[2] + m[r * 4 + 3] * w;
}

/* GX NDC to the landscape screen's NDC: the 640x480 frame, 4:3 and half
 * size in the middle of 400x240. Applied in clip space: x' = ax*x + bx*w. */
static float vp_ax, vp_bx, vp_ay, vp_by;

static void viewport_setup(void)
{
    const float* vp = __gx.viewport;
    static int logged;
    if (logged < 6 && (vp[0] != 0 || vp[2] != 640 || vp[3] != 480)) {
        logged++;
        __port_log("gx: viewport %d %d %d %d", (int)vp[0], (int)vp[1], (int)vp[2], (int)vp[3]);
    }
    float w = vp[2] ? vp[2] : 640.0f, h = vp[3] ? vp[3] : 480.0f;
    /* EFB x = left + (xn + 1) * w / 2; screen px = 40 + EFB x / 2;
     * screen NDC = px / 200 - 1. */
    vp_ax = (w / 2.0f) / 2.0f / 200.0f;
    vp_bx = (40.0f + (vp[0] + w / 2.0f) / 2.0f) / 200.0f - 1.0f;
    /* EFB y = top + (1 - yn) * h / 2; screen py = EFB y / 2; NDC = 1 - py / 120. */
    vp_ay = (h / 2.0f) / 2.0f / 120.0f;
    vp_by = 1.0f - (vp[1] + h / 2.0f) / 2.0f / 120.0f;
}

static void project(const float* e, float* clip)
{
    const float* p = __gx.projection;
    float x, y, z, w;
    if (p[0] == (float)GXENUM_PERSPECTIVE) {
        x = p[1] * e[0] + p[2] * e[2];
        y = p[3] * e[1] + p[4] * e[2];
        z = p[5] * e[2] + p[6];
        w = -e[2];
    } else {
        x = p[1] * e[0] + p[2];
        y = p[3] * e[1] + p[4];
        z = p[5] * e[2] + p[6];
        w = 1.0f;
    }
    /* With clipping off (GXSetClipMode(GX_CLIP_DISABLE)) the GameCube still
     * draws what lies on or beyond the depth range's ends; 2D overlays sit
     * exactly at the far end. The PICA always clips, so keep z just inside. */
    if (__gx.clip_mode == 1 && w > 0) {
        if (z > -1e-5f * w) z = -1e-5f * w;
        if (z < -(1.0f - 1e-5f) * w) z = -(1.0f - 1e-5f) * w;
    }
    clip[0] = vp_ax * x + vp_bx * w;
    clip[1] = vp_ay * y + vp_by * w;
    clip[2] = z;
    clip[3] = w;
}

static float light_f(const GXLightObj* l, int i) { float f; memcpy(&f, &l->data[i], 4); return f; }

/* One lighting channel's colour or alpha for a vertex (GX_COLOR0 = 0, ...). */
static void channel(int ch, const Vtx* v, const float* eye, const float* n, unsigned char* out)
{
    int slot = ch & 1, is_alpha = ch >= 2;
    const unsigned char* vc = v->has_clr[slot] ? v->clr[slot] : 0;
    unsigned char mat[4], amb[4];
    memcpy(mat, __gx.chan[ch].mat_src && vc ? vc : (const unsigned char*)&__gx.chan_mat[slot], 4);
    if (!__gx.chan[ch].enable) {
        if (is_alpha) out[3] = mat[3];
        else memcpy(out, mat, 3);
        return;
    }
    memcpy(amb, __gx.chan[ch].amb_src && vc ? vc : (const unsigned char*)&__gx.chan_amb[slot], 4);
    float acc[4] = { amb[0], amb[1], amb[2], amb[3] };
    for (int i = 0; i < GX_MAX_LIGHT; i++) {
        if (!(__gx.chan[ch].light_mask & (1u << i)))
            continue;
        const GXLightObj* l = &__gx.light[i];
        unsigned lc = l->data[3];
        float lp[3] = { light_f(l, 10), light_f(l, 11), light_f(l, 12) };
        float L[3] = { lp[0] - eye[0], lp[1] - eye[1], lp[2] - eye[2] };
        float d2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2], d = sqrtf(d2);
        if (d > 0) { L[0] /= d; L[1] /= d; L[2] /= d; }
        float diff = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        if (__gx.chan[ch].diff_fn == 0) diff = 1.0f;          /* GX_DF_NONE */
        else if (__gx.chan[ch].diff_fn == 2 && diff < 0) diff = 0;   /* GX_DF_CLAMP */
        float attn = 1.0f;
        if (__gx.chan[ch].attn_fn == 1) {                     /* GX_AF_SPOT */
            float dir[3] = { light_f(l, 13), light_f(l, 14), light_f(l, 15) };
            float cs = -(L[0] * dir[0] + L[1] * dir[1] + L[2] * dir[2]);
            float a = light_f(l, 4) + light_f(l, 5) * cs + light_f(l, 6) * cs * cs;
            float k = light_f(l, 7) + light_f(l, 8) * d + light_f(l, 9) * d2;
            attn = (a > 0 ? a : 0) / (k > 0 ? k : 1);
        }
        float s = diff * attn;
        acc[0] += ((lc >> 24) & 255) * s;
        acc[1] += ((lc >> 16) & 255) * s;
        acc[2] += ((lc >> 8) & 255) * s;
        acc[3] += (lc & 255) * s;
    }
    for (int k = 0; k < 4; k++) {
        float c = acc[k] > 255 ? 255 : acc[k] < 0 ? 0 : acc[k];
        unsigned char r = (unsigned char)(mat[k] * c / 255.0f);
        if (is_alpha) { if (k == 3) out[3] = r; }
        else if (k < 3) out[k] = r;
    }
}

static const float identity34[12] = { 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0 };

static const float* tex_matrix(unsigned id)
{
    if (id >= 60)
        return identity34;                                   /* GX_IDENTITY */
    if (id >= 30) {
        unsigned i = (id - 30) / 3;
        return &__gx.tex_mtx[i < GX_TEXMTX_COUNT ? i : 0][0][0];
    }
    return pos_matrix(id);
}

static void texgen(int i, const Vtx* v, const unsigned char* c0, float* out)
{
    int src = __gx.texgen[i].src, func = __gx.texgen[i].func;
    float in[3] = { 0, 0, 1 };
    if (src == 0) { memcpy(in, v->pos, sizeof(in)); }                   /* GX_TG_POS */
    else if (src == 1) { memcpy(in, v->nrm, sizeof(in)); }              /* GX_TG_NRM */
    else if (src >= 4 && src <= 11 && v->has_tex[src - 4]) { in[0] = v->tex[src - 4][0]; in[1] = v->tex[src - 4][1]; }
    if (func == 10) {                                                   /* GX_TG_SRTG */
        out[0] = c0[0] / 255.0f; out[1] = c0[1] / 255.0f;
        return;
    }
    if (func >= 2) {                                                    /* bump: the source as is */
        out[0] = in[0]; out[1] = in[1];
        return;
    }
    unsigned id = v->has_texmtx[i] ? v->texmtx[i] : __gx.texgen[i].mtx;
    const float* m = tex_matrix(id);
    float w = (src == 0 || src == 1) ? 1.0f : 1.0f;
    if (src >= 4) in[2] = 1.0f;
    float r[3];
    mul34(m, in, w, r);
    if (func == 0 && r[2] != 0.0f && r[2] != 1.0f) {                    /* GX_TG_MTX3x4 */
        r[0] /= r[2];
        r[1] /= r[2];
    }
    out[0] = r[0];
    out[1] = r[1];
}

/* ---- textures ---------------------------------------------------------------- */

typedef struct {
    const void* image;
    const void* tlut;
    unsigned short w, h;
    unsigned char fmt, tlut_fmt;
    unsigned tex;
    unsigned short tw, th;            /* the PICA texture's (power-of-two) size */
    unsigned bytes;                   /* size of the GX image */
    unsigned char stale;              /* written since decoded (DCFlushRange) */
} TexEntry;

#define TEX_CACHE 1024
#define TEX_HASH_BITS 11
#define TEX_HASH (1 << TEX_HASH_BITS)    /* twice TEX_CACHE, so probing ends */
static TexEntry tex_cache[TEX_CACHE];
static unsigned short tex_hash[TEX_HASH];
static int tex_count;
static unsigned char* decode_buf;     /* RGBA8, w*h*4, reused */
static unsigned tex_lo = ~0u, tex_hi;   /* address range of all cached images */
static unsigned decode_cap;

static unsigned pot(unsigned v)
{
    unsigned p = 8;
    while (p < v && p < 1024)
        p <<= 1;
    return p;
}

static void tlut_color(const unsigned char* lut, int fmt, unsigned idx, unsigned char* o);

static void rgb5a3(unsigned v, unsigned char* o)
{
    if (v & 0x8000) {
        o[0] = (unsigned char)(((v >> 10) & 31) * 255 / 31);
        o[1] = (unsigned char)(((v >> 5) & 31) * 255 / 31);
        o[2] = (unsigned char)((v & 31) * 255 / 31);
        o[3] = 255;
    } else {
        o[0] = (unsigned char)(((v >> 8) & 15) * 17);
        o[1] = (unsigned char)(((v >> 4) & 15) * 17);
        o[2] = (unsigned char)((v & 15) * 17);
        o[3] = (unsigned char)(((v >> 12) & 7) * 255 / 7);
    }
}

static void rgb565(unsigned v, unsigned char* o)
{
    o[0] = (unsigned char)((v >> 11) * 255 / 31);
    o[1] = (unsigned char)(((v >> 5) & 63) * 255 / 63);
    o[2] = (unsigned char)((v & 31) * 255 / 31);
    o[3] = 255;
}

static void tlut_color(const unsigned char* lut, int fmt, unsigned idx, unsigned char* o)
{
    unsigned v = lut ? rd16(lut + idx * 2) : 0;
    if (fmt == 0) { o[0] = o[1] = o[2] = (unsigned char)(v & 255); o[3] = (unsigned char)(v >> 8); }   /* IA8 */
    else if (fmt == 1) rgb565(v, o);
    else rgb5a3(v, o);
}

/* Decodes a GX texture into decode_buf, RGBA8, row by row from the top. */
static int decode_texture(const unsigned char* src, int w, int h, int fmt, const unsigned char* lut,
                          int lut_fmt)
{
    static const unsigned char bw[16] = { 8, 8, 8, 4, 4, 4, 4, 0, 8, 8, 4, 0, 0, 0, 8, 0 };
    static const unsigned char bh[16] = { 8, 4, 4, 4, 4, 4, 4, 0, 8, 4, 4, 0, 0, 0, 8, 0 };
    if (fmt > 15 || !bw[fmt])
        return 0;
    unsigned need = (unsigned)w * h * 4;
    if (need > decode_cap) {
        host_free(decode_buf);
        decode_buf = host_malloc(need);
        decode_cap = decode_buf ? need : 0;
        if (!decode_buf)
            return 0;
    }
    int bx = bw[fmt], by = bh[fmt];
    const unsigned char* p = src;
    for (int ty = 0; ty < h; ty += by) {
        for (int tx = 0; tx < w; tx += bx) {
            if (fmt == 6) {                                        /* RGBA8: AR plane, GB plane */
                for (int i = 0; i < 16; i++) {
                    int x = tx + (i & 3), y = ty + (i >> 2);
                    if (x < w && y < h) {
                        unsigned char* o = decode_buf + ((unsigned)y * w + x) * 4;
                        o[3] = p[i * 2]; o[0] = p[i * 2 + 1]; o[1] = p[32 + i * 2]; o[2] = p[32 + i * 2 + 1];
                    }
                }
                p += 64;
                continue;
            }
            if (fmt == 14) {                                       /* CMPR: four DXT1 blocks */
                for (int sb = 0; sb < 4; sb++) {
                    unsigned c0 = rd16(p), c1 = rd16(p + 2);
                    unsigned char pal[4][4];
                    rgb565(c0, pal[0]);
                    rgb565(c1, pal[1]);
                    for (int k = 0; k < 3; k++) {
                        if (c0 > c1) {
                            pal[2][k] = (unsigned char)((2 * pal[0][k] + pal[1][k]) / 3);
                            pal[3][k] = (unsigned char)((pal[0][k] + 2 * pal[1][k]) / 3);
                        } else {
                            pal[2][k] = (unsigned char)((pal[0][k] + pal[1][k]) / 2);
                            pal[3][k] = pal[2][k];
                        }
                    }
                    pal[2][3] = 255;
                    pal[3][3] = c0 > c1 ? 255 : 0;
                    for (int row = 0; row < 4; row++) {
                        unsigned bits = p[4 + row];
                        for (int col = 0; col < 4; col++) {
                            int x = tx + (sb & 1) * 4 + col, y = ty + (sb >> 1) * 4 + row;
                            if (x < w && y < h)
                                memcpy(decode_buf + ((unsigned)y * w + x) * 4, pal[(bits >> (6 - col * 2)) & 3], 4);
                        }
                    }
                    p += 8;
                }
                continue;
            }
            for (int y = ty; y < ty + by; y++) {
                for (int x = tx; x < tx + bx; x++) {
                    unsigned char px[4];
                    switch (fmt) {
                    case 0: {                                      /* I4 */
                        unsigned i = (x - tx) & 1 ? p[0] & 15 : p[0] >> 4;
                        px[0] = px[1] = px[2] = px[3] = (unsigned char)(i * 17);
                        if ((x - tx) & 1) p++;
                        break;
                    }
                    case 1: px[0] = px[1] = px[2] = px[3] = *p++; break;                         /* I8 */
                    case 2: px[0] = px[1] = px[2] = (unsigned char)((*p & 15) * 17);             /* IA4 */
                        px[3] = (unsigned char)((*p >> 4) * 17); p++; break;
                    case 3: px[3] = p[0]; px[0] = px[1] = px[2] = p[1]; p += 2; break;           /* IA8 */
                    case 4: rgb565(rd16(p), px); p += 2; break;
                    case 5: rgb5a3(rd16(p), px); p += 2; break;
                    case 8: {                                      /* C4 */
                        unsigned i = (x - tx) & 1 ? p[0] & 15 : p[0] >> 4;
                        tlut_color(lut, lut_fmt, i, px);
                        if ((x - tx) & 1) p++;
                        break;
                    }
                    case 9: tlut_color(lut, lut_fmt, *p++, px); break;                            /* C8 */
                    default: tlut_color(lut, lut_fmt, rd16(p) & 0x3FFF, px); p += 2; break;       /* C14X2 */
                    }
                    if (x < w && y < h)
                        memcpy(decode_buf + ((unsigned)y * w + x) * 4, px, 4);
                }
            }
        }
    }
    return 1;
}

/* RGBA8 rows to the PICA's tiled layout: 8x8 tiles, Morton order inside;
 * texel bytes A, B, G, R. Rows go in as they come: t = 0 is the first. */
static void upload(unsigned tex, int w, int h, int tw, int th)
{
    unsigned char* dst = host_gpu_tex_data(tex);
    if (!dst)
        return;
    memset(dst, 0, (unsigned)tw * th * 4);
    for (int y = 0; y < h && y < th; y++) {
        for (int x = 0; x < w && x < tw; x++) {
            /* The PICA's texture rows run bottom up: t = 0 is the last row in
             * memory order of tiles. */
            int py = th - 1 - y;
            unsigned m = (x & 1) | ((py & 1) << 1) | ((x & 2) << 1) | ((py & 2) << 2) | ((x & 4) << 2) |
                         ((py & 4) << 3);
            unsigned off = ((unsigned)(py >> 3) * (tw >> 3) + (x >> 3)) * 64 + m;
            const unsigned char* s = decode_buf + ((unsigned)y * w + x) * 4;
            unsigned char* d = dst + off * 4;
            d[0] = s[3]; d[1] = s[2]; d[2] = s[1]; d[3] = s[0];
        }
    }
    host_gpu_tex_upload(tex);
}

/* Bytes of a GX image: whole blocks of the format's block size. */
static unsigned image_bytes(int w, int h, int fmt)
{
    static const unsigned char bw[16] = { 8, 8, 8, 4, 4, 4, 4, 4, 8, 8, 4, 4, 4, 4, 8, 4 };
    static const unsigned char bh[16] = { 8, 4, 4, 4, 4, 4, 4, 4, 8, 4, 4, 4, 4, 4, 8, 4 };
    static const unsigned char bb[16] = { 32, 32, 32, 32, 32, 32, 64, 32, 32, 32, 32, 32, 32, 32, 32, 32 };
    unsigned bx = (w + bw[fmt & 15] - 1) / bw[fmt & 15], by = (h + bh[fmt & 15] - 1) / bh[fmt & 15];
    return bx * by * bb[fmt & 15];
}

/* The game wrote [addr, addr + len) and flushed it for the GPU: textures
 * decoded from there are decoded again at their next use. */
void __gx_tex_written(unsigned addr, unsigned len)
{
    if (addr >= tex_hi || addr + len <= tex_lo)
        return;
    for (int i = 0; i < tex_count; i++) {
        TexEntry* e = &tex_cache[i];
        unsigned a = (unsigned)e->image;
        if (addr < a + e->bytes && addr + len > a)
            e->stale = 1;
    }
}

/* The host texture for texture map `map`, and the scale from GX texture
 * coordinates to it (a texture padded to a power of two). */
static unsigned texture(int map, float* ss, float* st, GpuUnit* unit)
{
    const GXTexObj* o = &__gx.texmap[map];
    unsigned image0 = o->data[2];
    int w = (image0 & 0x3FF) + 1, h = ((image0 >> 10) & 0x3FF) + 1;
    int fmt = (int)(o->data[5] & 0xF);
    const void* image = (const void*)(o->data[3] << 5);
    const void* lut = 0;
    int lut_fmt = 0;
    if (fmt >= 8 && fmt <= 10) {
        unsigned name = o->data[6];
        if (name < GX_MAX_TLUT) {
            lut_fmt = (int)__gx.tlut[name].data[0];
            lut = (const void*)__gx.tlut[name].data[1];
        }
    }
    unsigned mode0 = o->data[0];
    static const unsigned char wrap[4] = { PICA_CLAMP_TO_EDGE, PICA_REPEAT, PICA_MIRRORED_REPEAT, PICA_REPEAT };
    unit->wrap_s = wrap[mode0 & 3];
    unit->wrap_t = wrap[(mode0 >> 2) & 3];
    unit->mag = (mode0 >> 4) & 1 ? PICA_LINEAR : PICA_NEAREST;
    unit->min = unit->mag;

    /* Hash on the image address, linear probing; slots hold index + 1. */
    unsigned slot = (((unsigned)image >> 5) * 2654435761u) >> (32 - TEX_HASH_BITS);
    for (;; slot = (slot + 1) & (TEX_HASH - 1)) {
        int i = tex_hash[slot] - 1;
        if (i < 0)
            break;
        TexEntry* e = &tex_cache[i];
        if (e->image == image && e->w == w && e->h == h && e->fmt == fmt && e->tlut == lut) {
            *ss = (float)w / e->tw;
            *st = (float)h / e->th;
            if (e->stale) {                 /* the game wrote the image again */
                e->stale = 0;
                if (decode_texture(image, w, h, fmt, lut, lut_fmt))
                    upload(e->tex, w < e->tw ? w : e->tw, h < e->th ? h : e->th, e->tw, e->th);
            }
            return e->tex;
        }
    }
    if (!image || tex_count >= TEX_CACHE)
        return 0;
    int tw = pot(w), th = pot(h);
    if (!decode_texture(image, w, h, fmt, lut, lut_fmt))
        return 0;
    unsigned tex = host_gpu_tex_create(tw, th);
    if (!tex)
        return 0;
    upload(tex, w < tw ? w : tw, h < th ? h : th, tw, th);
    tex_hash[slot] = (unsigned short)(tex_count + 1);
    TexEntry* e = &tex_cache[tex_count++];
    e->image = image; e->tlut = lut; e->w = w; e->h = h; e->fmt = fmt; e->tlut_fmt = lut_fmt;
    e->tex = tex; e->tw = tw; e->th = th;
    e->bytes = image_bytes(w, h, fmt);
    e->stale = 0;
    if ((unsigned)image < tex_lo) tex_lo = (unsigned)image;
    if ((unsigned)image + e->bytes > tex_hi) tex_hi = (unsigned)image + e->bytes;
    *ss = (float)w / tw;
    *st = (float)h / th;
    return tex;
}

/* ---- TEV to combiners ---------------------------------------------------------- */

typedef struct { unsigned char src, op; int konst; unsigned char k[4]; } In;

static int unit_of_stage[GX_MAX_TEVSTAGE];

static unsigned char clamp255(int v) { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); }

static void reg_color(int reg, unsigned char* out)
{
    const GXColorS10* c = &__gx.tev_color[reg];
    out[0] = clamp255(c->r); out[1] = clamp255(c->g); out[2] = clamp255(c->b); out[3] = clamp255(c->a);
}

/* A KONST selection: 0..7 are 8/8 .. 1/8, 12..15 the colours K0..K3 (as
 * colour; as alpha their alpha), 16..31 one component of K0..K3 (R, G, B, A
 * in groups of four), replicated. */
static void konst(int sel, int alpha, unsigned char* out)
{
    if (sel < 8) {
        unsigned char v = (unsigned char)((8 - sel) * 255 / 8);
        out[0] = out[1] = out[2] = out[3] = v;
        return;
    }
    if (sel >= 12 && sel <= 15) {
        const GXColor* k = &__gx.tev_kcolor[sel - 12];
        out[0] = k->r; out[1] = k->g; out[2] = k->b; out[3] = k->a;
        if (alpha)
            out[0] = out[1] = out[2] = k->a;
        return;
    }
    if (sel >= 16 && sel <= 31) {
        const GXColor* k = &__gx.tev_kcolor[(sel - 16) & 3];
        unsigned char c[4] = { k->r, k->g, k->b, k->a };
        out[0] = out[1] = out[2] = out[3] = c[(sel - 16) >> 2];
        return;
    }
    out[0] = out[1] = out[2] = out[3] = 255;
}

static int tex_src(int s) { int u = unit_of_stage[s]; return u < 0 ? PICA_SRC_PRIMARY : PICA_SRC_TEX0 + u; }

/* A GX colour argument as a PICA source. */
static In color_in(int s, int arg)
{
    In r = { PICA_SRC_PREVIOUS, PICA_OP_RGB_COLOR, 0, { 0 } };
    switch (arg) {
    case 0: break;                                                        /* CPREV */
    case 1: r.op = PICA_OP_RGB_ALPHA; break;                              /* APREV */
    case 2: case 4: case 6: r.konst = 1; reg_color(arg / 2, r.k); break;  /* C0..C2 */
    case 3: case 5: case 7: { r.konst = 1; unsigned char c[4]; reg_color(arg / 2, c);
                              r.k[0] = r.k[1] = r.k[2] = r.k[3] = c[3]; break; }
    case 8: r.src = tex_src(s); break;                                    /* TEXC */
    case 9: r.src = tex_src(s); r.op = PICA_OP_RGB_ALPHA; break;          /* TEXA */
    case 10: r.src = PICA_SRC_PRIMARY; break;                             /* RASC */
    case 11: r.src = PICA_SRC_PRIMARY; r.op = PICA_OP_RGB_ALPHA; break;   /* RASA */
    case 12: r.konst = 1; memset(r.k, 255, 4); break;                     /* ONE */
    case 13: r.konst = 1; memset(r.k, 128, 4); break;                     /* HALF */
    case 14: r.konst = 1; konst(__gx.tev[s].kcsel, 0, r.k); break;        /* KONST */
    default: r.konst = 1; break;                                          /* ZERO */
    }
    return r;
}

static In alpha_in(int s, int arg)
{
    In r = { PICA_SRC_PREVIOUS, PICA_OP_A_ALPHA, 0, { 0 } };
    switch (arg) {
    case 0: break;                                                        /* APREV */
    case 1: case 2: case 3: r.konst = 1; reg_color(arg, r.k); break;      /* A0..A2 */
    case 4: r.src = tex_src(s); break;                                    /* TEXA */
    case 5: r.src = PICA_SRC_PRIMARY; break;                              /* RASA */
    case 6: r.konst = 1; konst(__gx.tev[s].kasel, 1, r.k); break;         /* KONST */
    default: r.konst = 1; break;                                          /* ZERO */
    }
    return r;
}

/* ---- the TEV compiler --------------------------------------------------------
 * A GX stage computes, per half (colour, alpha):
 *     out = (d +- lerp(a, b, c) + bias) * scale,  lerp = a (1 - c) + b c
 * A PICA combiner stage takes up to three sources but only one constant.
 * Inputs are either variable (texture, rasterised colour, previous result)
 * or constant (registers, KONST, one, half, zero) for the whole draw, so
 * the constant arithmetic is done here, and each half becomes one or two
 * PICA operations with at most one constant each. */

typedef struct {
    unsigned char func, src[3], op[3], scale;
    float k[4];                 /* the constant, if the operation uses one */
    int has_k;
} PicaOp;

typedef struct { int is_k; In in; float v[4]; } Val;

/* A constant's value for one half: the colour half uses red, green and blue,
 * the alpha half only alpha (in all four places, so tests over all four
 * components mean the right thing). */
static Val val_of(In in, int alpha)
{
    Val v;
    v.is_k = in.konst;
    v.in = in;
    for (int i = 0; i < 4; i++)
        v.v[i] = in.k[alpha ? 3 : (i < 3 ? i : 2)] / 255.0f;
    return v;
}

static int all(const float* v, float x)
{
    for (int i = 0; i < 4; i++)
        if (fabsf(v[i] - x) > 0.002f)
            return 0;
    return 1;
}

static void set_k(PicaOp* o, const float* k)
{
    memcpy(o->k, k, sizeof(o->k));
    o->has_k = 1;
}

/* A source slot: a variable input, possibly inverted, or the constant. */
static void slot_var(PicaOp* o, int i, In in, int invert, int alpha)
{
    o->src[i] = in.src;
    unsigned char op = in.op;
    if (invert) {
        if (alpha) op = op == PICA_OP_A_ALPHA ? PICA_OP_A_1_ALPHA : PICA_OP_A_ALPHA;
        else op = op == PICA_OP_RGB_COLOR ? PICA_OP_RGB_1_COLOR
                : op == PICA_OP_RGB_ALPHA ? PICA_OP_RGB_1_ALPHA : op;
    }
    o->op[i] = op;
}

static void slot_k(PicaOp* o, int i, int alpha)
{
    o->src[i] = PICA_SRC_CONSTANT;
    o->op[i] = alpha ? PICA_OP_A_ALPHA : PICA_OP_RGB_COLOR;
}

static void slot_prev(PicaOp* o, int i, int alpha)
{
    o->src[i] = PICA_SRC_PREVIOUS;
    o->op[i] = alpha ? PICA_OP_A_ALPHA : PICA_OP_RGB_COLOR;
}

static PicaOp* new_op(PicaOp* ops, int* n)
{
    PicaOp* o = &ops[(*n)++];
    memset(o, 0, sizeof(*o));
    for (int i = 0; i < 3; i++) o->src[i] = PICA_SRC_PREVIOUS;
    return o;
}

/* "+ k" after the operations so far (k may be negative). */
static void add_k(PicaOp* ops, int* n, const float* k, int alpha)
{
    if (all(k, 0.0f))
        return;
    float pos[4], neg[4];
    int any_neg = 0;
    for (int i = 0; i < 4; i++) {
        pos[i] = k[i] > 0 ? k[i] : 0;
        neg[i] = k[i] < 0 ? -k[i] : 0;
        any_neg |= k[i] < -0.002f;
    }
    PicaOp* o = new_op(ops, n);
    o->func = any_neg && all(pos, 0.0f) ? PICA_SUBTRACT : PICA_ADD;
    slot_prev(o, 0, alpha);
    slot_k(o, 1, alpha);
    set_k(o, o->func == PICA_SUBTRACT ? neg : pos);
}

/* Compiles one half. Returns the number of operations (1..3) in ops. */
static int compile_half(In ia, In ib, In ic, In id, int sub, int bias, int scale, PicaOp* ops, int alpha)
{
    Val a = val_of(ia, alpha), b = val_of(ib, alpha), c = val_of(ic, alpha), d = val_of(id, alpha);
    float sgn = sub ? -1.0f : 1.0f, bias_v = bias == 1 ? 0.5f : bias == 2 ? -0.5f : 0.0f;
    int n = 0;

    if (c.is_k) {
        /* result = a (1 - c) + b c, +- d: linear in the variable inputs. */
        float K[4], ca[4], cb[4];
        for (int i = 0; i < 4; i++) {
            ca[i] = sgn * (1.0f - c.v[i]);
            cb[i] = sgn * c.v[i];
            K[i] = bias_v + (a.is_k ? ca[i] * a.v[i] : 0) + (b.is_k ? cb[i] * b.v[i] : 0) +
                   (d.is_k ? d.v[i] : 0);
        }
        int va = !a.is_k && !all(ca, 0.0f), vb = !b.is_k && !all(cb, 0.0f), vd = !d.is_k;
        if (va && vb && !sub) {
            PicaOp* o = new_op(ops, &n);                      /* b c + a (1 - c) */
            o->func = PICA_INTERPOLATE;
            slot_var(o, 0, b.in, 0, alpha);
            slot_var(o, 1, a.in, 0, alpha);
            slot_k(o, 2, alpha);
            set_k(o, c.v);
            if (vd) {
                PicaOp* p = new_op(ops, &n);
                p->func = PICA_ADD;
                slot_prev(p, 0, alpha);
                slot_var(p, 1, d.in, 0, alpha);
            }
            add_k(ops, &n, K, alpha);
        } else if (va || vb) {
            In v = va ? a.in : b.in;
            const float* cv = va ? ca : cb;
            int negative = cv[0] < 0 || cv[3] < 0;
            float mag[4];
            for (int i = 0; i < 4; i++) mag[i] = fabsf(cv[i]);
            PicaOp* o = new_op(ops, &n);
            if (vd && negative && all(mag, 1.0f)) {          /* d - v */
                o->func = PICA_SUBTRACT;
                slot_var(o, 0, d.in, 0, alpha);
                slot_var(o, 1, v, 0, alpha);
            } else if (all(mag, 1.0f)) {
                o->func = vd ? PICA_ADD : PICA_REPLACE;
                slot_var(o, 0, v, 0, alpha);
                if (vd) slot_var(o, 1, d.in, 0, alpha);
            } else {
                o->func = vd ? PICA_MULTIPLY_ADD : PICA_MODULATE;
                slot_var(o, 0, v, 0, alpha);
                slot_k(o, 1, alpha);
                set_k(o, mag);
                if (vd) slot_var(o, 2, d.in, 0, alpha);
            }
            add_k(ops, &n, K, alpha);
        } else if (vd) {
            PicaOp* o = new_op(ops, &n);
            o->func = PICA_REPLACE;
            slot_var(o, 0, d.in, 0, alpha);
            add_k(ops, &n, K, alpha);
        } else {
            PicaOp* o = new_op(ops, &n);                      /* constant */
            o->func = PICA_REPLACE;
            slot_k(o, 0, alpha);
            float kc[4];
            for (int i = 0; i < 4; i++) kc[i] = K[i] < 0 ? 0 : K[i];
            set_k(o, kc);
        }
    } else {
        /* c varies: lerp(a, b, c) with a, b each constant or not. */
        PicaOp* o = new_op(ops, &n);
        if (a.is_k && b.is_k) {
            if (all(a.v, 0.0f)) {
                o->func = PICA_MODULATE; slot_var(o, 0, c.in, 0, alpha); slot_k(o, 1, alpha); set_k(o, b.v);
            } else if (all(b.v, 0.0f)) {
                o->func = PICA_MODULATE; slot_var(o, 0, c.in, 1, alpha); slot_k(o, 1, alpha); set_k(o, a.v);
            } else {
                o->func = PICA_MODULATE; slot_var(o, 0, c.in, 0, alpha); slot_k(o, 1, alpha); set_k(o, b.v);
                PicaOp* p = new_op(ops, &n);                  /* + a (1 - c) */
                p->func = PICA_MULTIPLY_ADD;
                slot_var(p, 0, c.in, 1, alpha);
                slot_k(p, 1, alpha);
                set_k(p, a.v);
                slot_prev(p, 2, alpha);
            }
        } else if (a.is_k) {
            if (all(a.v, 0.0f)) { o->func = PICA_MODULATE; slot_var(o, 0, b.in, 0, alpha); slot_var(o, 1, c.in, 0, alpha); }
            else { o->func = PICA_INTERPOLATE; slot_var(o, 0, b.in, 0, alpha); slot_k(o, 1, alpha); set_k(o, a.v);
                   slot_var(o, 2, c.in, 0, alpha); }
        } else if (b.is_k) {
            if (all(b.v, 0.0f)) { o->func = PICA_MODULATE; slot_var(o, 0, a.in, 0, alpha); slot_var(o, 1, c.in, 1, alpha); }
            else { o->func = PICA_INTERPOLATE; slot_k(o, 0, alpha); set_k(o, b.v); slot_var(o, 1, a.in, 0, alpha);
                   slot_var(o, 2, c.in, 0, alpha); }
        } else {
            o->func = PICA_INTERPOLATE;
            slot_var(o, 0, b.in, 0, alpha); slot_var(o, 1, a.in, 0, alpha); slot_var(o, 2, c.in, 0, alpha);
        }
        float K[4];
        for (int i = 0; i < 4; i++) K[i] = bias_v + (d.is_k ? d.v[i] : 0);
        if (sub && n == 1 && !d.is_k) {
            /* d - lerp: only the plain case */
            PicaOp* p = new_op(ops, &n);
            p->func = PICA_SUBTRACT;
            slot_var(p, 0, d.in, 0, alpha);
            slot_prev(p, 1, alpha);
        } else if (!d.is_k) {
            PicaOp* p = new_op(ops, &n);
            p->func = PICA_ADD;
            slot_prev(p, 0, alpha);
            slot_var(p, 1, d.in, 0, alpha);
        }
        add_k(ops, &n, K, alpha);
    }
    ops[n - 1].scale = scale == 1 ? PICA_SCALE_2 : scale == 2 ? PICA_SCALE_4 : PICA_SCALE_1;
    return n;
}

static unsigned char to_byte(float v) { return (unsigned char)(v <= 0 ? 0 : v >= 1 ? 255 : v * 255.0f + 0.5f); }

static void combiners(GpuDraw* d)
{
    int stages = __gx.num_tev_stages < 1 ? 1 : __gx.num_tev_stages;
    int out = 0;
    for (int s = 0; s < stages && out < GPU_MAX_STAGES; s++) {
        const GXTevStageState* t = &__gx.tev[s];
        PicaOp rgb[4], a[4];
        int nr = compile_half(color_in(s, t->color_in[0]), color_in(s, t->color_in[1]),
                              color_in(s, t->color_in[2]), color_in(s, t->color_in[3]),
                              t->color_op == 1, t->color_bias, t->color_scale, rgb, 0);
        int na = compile_half(alpha_in(s, t->alpha_in[0]), alpha_in(s, t->alpha_in[1]),
                              alpha_in(s, t->alpha_in[2]), alpha_in(s, t->alpha_in[3]),
                              t->alpha_op == 1, t->alpha_bias, t->alpha_scale, a, 1);
        int n = nr > na ? nr : na;
        for (int i = 0; i < n && out < GPU_MAX_STAGES; i++, out++) {
            GpuStage* g = &d->stage[out];
            float kr[4] = { 0 }, ka = 0;
            if (i < nr) {
                for (int j = 0; j < 3; j++) { g->src_rgb[j] = rgb[i].src[j]; g->op_rgb[j] = rgb[i].op[j]; }
                g->func_rgb = rgb[i].func;
                g->scale_rgb = rgb[i].scale;
                if (rgb[i].has_k) memcpy(kr, rgb[i].k, sizeof(kr));
            } else {
                g->src_rgb[0] = g->src_rgb[1] = g->src_rgb[2] = PICA_SRC_PREVIOUS;
                g->func_rgb = PICA_REPLACE;
            }
            if (i < na) {
                for (int j = 0; j < 3; j++) { g->src_a[j] = a[i].src[j]; g->op_a[j] = a[i].op[j]; }
                g->func_a = a[i].func;
                g->scale_a = a[i].scale;
                if (a[i].has_k) ka = a[i].k[3];
            } else {
                g->src_a[0] = g->src_a[1] = g->src_a[2] = PICA_SRC_PREVIOUS;
                g->func_a = PICA_REPLACE;
            }
            g->color = (unsigned)to_byte(kr[0]) | (unsigned)to_byte(kr[1]) << 8 | (unsigned)to_byte(kr[2]) << 16 |
                       (unsigned)to_byte(ka) << 24;
        }
    }
    d->num_stages = (unsigned char)(out ? out : 1);
}
/* ---- pixel state ------------------------------------------------------------- */

static const unsigned char gx_func[8] = { PICA_NEVER, PICA_LESS, PICA_EQUAL, PICA_LEQUAL,
                                          PICA_GREATER, PICA_NOTEQUAL, PICA_GEQUAL, PICA_ALWAYS };

static unsigned char blend_factor(int f, int is_src)
{
    switch (f) {
    case 0: return PICA_ZERO;
    case 1: return PICA_ONE;
    case 2: return is_src ? PICA_DST_COLOR : PICA_SRC_COLOR;
    case 3: return is_src ? PICA_ONE_MINUS_DST_COLOR : PICA_ONE_MINUS_SRC_COLOR;
    case 4: return PICA_SRC_ALPHA;
    case 5: return PICA_ONE_MINUS_SRC_ALPHA;
    case 6: return PICA_DST_ALPHA;
    default: return PICA_ONE_MINUS_DST_ALPHA;
    }
}

static void pixel_state(GpuDraw* d)
{
    /* Alpha compare: the common single-test forms. */
    int c0 = __gx.alpha_comp0 & 7, c1 = __gx.alpha_comp1 & 7;
    if (c0 == 7 && c1 == 7) {
        d->alpha_test = 0;
    } else if (c1 == 7 || (__gx.alpha_op == 0 && c1 == 7)) {
        d->alpha_test = 1; d->alpha_func = gx_func[c0]; d->alpha_ref = __gx.alpha_ref0;
    } else if (c0 == 7) {
        d->alpha_test = 1; d->alpha_func = gx_func[c1]; d->alpha_ref = __gx.alpha_ref1;
    } else {
        d->alpha_test = 1; d->alpha_func = gx_func[c0]; d->alpha_ref = __gx.alpha_ref0;
    }
    d->alpha_func = d->alpha_test ? d->alpha_func : PICA_ALWAYS;

    if (__gx.blend_type == 1 || __gx.blend_type == 3) {         /* GX_BM_BLEND, GX_BM_SUBTRACT */
        d->blend = 1;
        d->blend_eq = __gx.blend_type == 3 ? PICA_BLEND_REVERSE_SUBTRACT : PICA_BLEND_ADD;
        d->blend_src = __gx.blend_type == 3 ? PICA_ONE : blend_factor(__gx.blend_src, 1);
        d->blend_dst = __gx.blend_type == 3 ? PICA_ONE : blend_factor(__gx.blend_dst, 0);
    }
    d->depth_test = __gx.z_compare ? 1 : 0;
    d->depth_func = __gx.z_compare ? gx_func[__gx.z_func & 7] : PICA_ALWAYS;
    d->write_mask = (__gx.color_update ? 0x07 : 0) | (__gx.alpha_update ? 0x08 : 0) |
                    (__gx.z_update ? PICA_WRITE_DEPTH : 0);
    d->cull = PICA_CULL_NONE;
}

/* Bring-up aid: every new TEV setup, once, to the port log. */
static void log_tev(int units, const int* unit_map)
{
    static unsigned seen[256];
    static int n_seen;
    unsigned h = 2166136261u;
    for (int s = 0; s < __gx.num_tev_stages && s < 4; s++) {
        const GXTevStageState* t = &__gx.tev[s];
        const unsigned char* b = (const unsigned char*)t;
        for (unsigned i = 0; i < 16; i++)
            h = (h ^ b[i]) * 16777619u;
    }
    h ^= (unsigned)__gx.num_tev_stages << 24 ^ (unsigned)__gx.blend_type << 20 ^ (unsigned)units << 16;
    for (int i = 0; i < n_seen; i++)
        if (seen[i] == h)
            return;
    if (n_seen >= 256)
        return;
    seen[n_seen++] = h;
    __port_log("tev: %d stages, %d chans, blend %d %d %d, alpha %d/%d %d %d/%d", __gx.num_tev_stages,
               __gx.num_chans, __gx.blend_type, __gx.blend_src, __gx.blend_dst, __gx.alpha_comp0,
               __gx.alpha_ref0, __gx.alpha_op, __gx.alpha_comp1, __gx.alpha_ref1);
    for (int s = 0; s < __gx.num_tev_stages && s < 4; s++) {
        const GXTevStageState* t = &__gx.tev[s];
        int fmt = -1;
        if (t->map < GX_MAX_TEXMAP)
            fmt = (int)(__gx.texmap[t->map].data[5] & 0xF);
        __port_log("  s%d map %d coord %d chan %d fmt %d | c %d %d %d %d op %d out %d | a %d %d %d %d op %d out %d"
                   " | k %d %d",
                   s, t->map, t->coord, t->chan, fmt, t->color_in[0], t->color_in[1], t->color_in[2],
                   t->color_in[3], t->color_op, t->color_out, t->alpha_in[0], t->alpha_in[1], t->alpha_in[2],
                   t->alpha_in[3], t->alpha_op, t->alpha_out, t->kcsel, t->kasel);
    }
    (void)unit_map;
}

/* ---- batching ------------------------------------------------------------------
 * Draws with the same state whose vertices follow each other in the buffer
 * are sent to the host as one. The pending draw goes out when the state
 * changes and before the frame ends (__gx_draw_flush). */

static GpuDraw pending;
static int have_pending;

static int same_state(const GpuDraw* a, const GpuDraw* b)
{
    unsigned off = (unsigned)((const char*)&a->unit[0] - (const char*)a);   /* after first, count */
    return memcmp((const char*)a + off, (const char*)b + off, sizeof(GpuDraw) - off) == 0;
}

void __gx_draw_flush(void)
{
    if (!have_pending)
        return;
    have_pending = 0;
    PROF_BEGIN(t_gpu);
    host_gpu_draw((const void*)&pending);
    PROF_END(PROF_GPU_SUBMIT, t_gpu);
}

static void submit(const GpuDraw* d)
{
    if (!d->count)
        return;
    if (have_pending && pending.first + pending.count == d->first && same_state(&pending, d)) {
        pending.count += d->count;
        return;
    }
    __gx_draw_flush();
    pending = *d;
    have_pending = 1;
}
/* ---- primitives ----------------------------------------------------------------- */

static GpuVertex* vbuf;
static unsigned vcap, vused;
static int warned_full;

void __gx_draw_frame_start(void)
{
    static unsigned frames, verts, draws_logged;
    verts += vused;
    if (++frames == 120 && draws_logged < 20) {
        draws_logged++;
        __port_log("gx: %u triangle vertices per frame", verts / 120);
        frames = verts = 0;
    }
    have_pending = 0;
    if (!vbuf) {
        vbuf = (GpuVertex*)host_gpu_vertices();
        vcap = host_gpu_capacity();
    }
    vused = 0;
}

/* One decoded vertex to a GPU vertex: transform, lighting, texture
 * coordinates for the units in use (scaled for padded textures). */
/* What every vertex of a draw needs, worked out once (emit_setup). */
static int emit_units, emit_normals, emit_light;

static void emit_setup(int units, const int* unit_coord)
{
    emit_units = units;
    emit_light = __gx.num_chans > 0 && (__gx.chan[0].enable || __gx.chan[2].enable);
    emit_normals = emit_light;
    for (int u = 0; u < units; u++)
        if (unit_coord[u] >= 0 && unit_coord[u] < GX_MAX_TEXCOORD && __gx.texgen[unit_coord[u]].src == 1)
            emit_normals = 1;                                   /* GX_TG_NRM */
}

static void emit(const Vtx* v, GpuVertex* out, const int* unit_coord, const float (*scale)[2])
{
    float e[3], n[3] = { 0, 0, 1 };
    unsigned mid = v->has_pnmtx ? v->pnmtx : (__gx.current_mtx & 0xFF);
    mul34(pos_matrix(mid), v->pos, 1.0f, e);
    if (v->has_nrm && emit_normals) {
        unsigned i = mid / 3;
        const float(*nm)[3] = __gx.nrm_mtx[i < GX_PNMTX_COUNT ? i : 0];
        for (int r = 0; r < 3; r++)
            n[r] = nm[r][0] * v->nrm[0] + nm[r][1] * v->nrm[1] + nm[r][2] * v->nrm[2];
        float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (l > 0) { n[0] /= l; n[1] /= l; n[2] /= l; }
    }
    float clip[4];
    project(e, clip);
    for (int k = 0; k < 4; k++)
        out->pos[k] = clip[k];          /* element by element: little-endian floats */
    unsigned char c[4] = { 255, 255, 255, 255 };
    if (__gx.num_chans > 0) {
        channel(0, v, e, n, c);
        channel(2, v, e, n, c);
    }
    memcpy(out->color, c, 4);
    for (int u = 0; u < emit_units; u++) {
        float tc[2] = { 0, 0 };
        if (unit_coord[u] >= 0 && unit_coord[u] < __gx.num_texgens)
            texgen(unit_coord[u], v, c, tc);
        out->tc[u][0] = tc[0] * scale[u][0];
        out->tc[u][1] = tc[1] * scale[u][1];
    }
}

/* Draws `count` vertices of primitive `prim` read from `data`. */
static void draw(int prim, int fmt, const unsigned char* data, unsigned count)
{
    if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS || count < 3)
        return;
    if (!__gx.color_update && !__gx.alpha_update && !__gx.z_update)
        return;
    __gx_ensure_frame();
    if (!vbuf)
        __gx_draw_frame_start();
    viewport_setup();

    /* The draw state, translated again only when a GX setter changed it
     * (matrix loads do not count, gx_state.c). */
    static GpuDraw d;
    static int unit_coord[GPU_MAX_UNITS], units, translated;
    static float scale[GPU_MAX_UNITS][2];
    if (__gx.dirty || !translated) {
        __gx.dirty = 0;
        translated = 1;
        memset(&d, 0, sizeof(d));
        /* Texture units: one per distinct (map, coordinate) the TEV stages use. */
        int unit_map[GPU_MAX_UNITS] = { -1, -1, -1 };
        for (int u = 0; u < GPU_MAX_UNITS; u++) {
            unit_coord[u] = -1;
            scale[u][0] = scale[u][1] = 1.0f;
        }
        units = 0;
        for (int s = 0; s < GX_MAX_TEVSTAGE; s++) {
            unit_of_stage[s] = -1;
            if (s >= __gx.num_tev_stages)
                continue;
            int map = __gx.tev[s].map, coord = __gx.tev[s].coord;
            if (map >= GX_MAX_TEXMAP || !__gx.texmap_loaded[map])
                continue;
            int u;
            for (u = 0; u < units; u++)
                if (unit_map[u] == map && unit_coord[u] == coord)
                    break;
            if (u == units) {
                if (units == GPU_MAX_UNITS)
                    continue;
                unsigned tex = texture(map, &scale[u][0], &scale[u][1], &d.unit[u]);
                if (!tex)
                    continue;
                d.unit[u].tex = tex;
                unit_map[u] = map;
                unit_coord[u] = coord;
                units++;
            }
            unit_of_stage[s] = u;
        }
        combiners(&d);
        pixel_state(&d);
        log_tev(units, unit_map);
    }
    layout_setup(fmt);
    emit_setup(units, unit_coord);

    /* Triangles from the primitive, decoded a vertex at a time. */
    const unsigned char* p = data;
    unsigned tris = prim == GX_QUADS ? count / 4 * 2 : prim == GX_TRIANGLES ? count / 3 : count - 2;
    unsigned need = tris * 3;
    if (vused + need > vcap) {
        if (!warned_full++)
            __port_log("gx: vertex buffer full (%u)", vcap);
        return;
    }
    GpuVertex* out = vbuf + vused;
    PROF_BEGIN(t_vtx);
    GpuVertex a, b, cur, q[4];                 /* a, b: the two vertices before cur */
    unsigned written = 0;
    for (unsigned i = 0; i < count; i++) {
        Vtx v;
        read_vertex(&p, fmt, &v);
        emit(&v, &cur, unit_coord, (const float(*)[2])scale);
        switch (prim) {
        case GX_QUADS:
            q[i & 3] = cur;
            if ((i & 3) == 3) {
                out[written++] = q[0]; out[written++] = q[1]; out[written++] = q[2];
                out[written++] = q[0]; out[written++] = q[2]; out[written++] = q[3];
            }
            break;
        case GX_TRIANGLES:
            out[written++] = cur;
            break;
        case GX_TRIANGLESTRIP:
            /* Triangle k = i - 2 is (a, b, cur), with every other one turned
             * around to keep the winding. */
            if (i >= 2) {
                if ((i - 2) & 1) { out[written++] = b; out[written++] = a; }
                else { out[written++] = a; out[written++] = b; }
                out[written++] = cur;
            }
            a = b;
            b = cur;
            break;
        case GX_TRIANGLEFAN:
            /* a is the centre, b the previous rim vertex. */
            if (i == 0) a = cur;
            else if (i >= 2) { out[written++] = a; out[written++] = b; out[written++] = cur; }
            b = cur;
            break;
        }
    }
    if (prim == GX_TRIANGLES)
        written -= written % 3;
    /* Bring-up aid: the first draw whose x is not finite, in detail. */
    static int badx_logged;
    if (written && badx_logged < 2 && !(fabsf(out[0].pos[0] / out[0].pos[3]) < 1e6f)) {
        badx_logged++;
        const unsigned char* q = data;
        Vtx v0;
        layout_setup(fmt);
        read_vertex(&q, fmt, &v0);
        unsigned mid = v0.has_pnmtx ? v0.pnmtx : (__gx.current_mtx & 0xFF);
        const float* m = pos_matrix(mid);
        __port_log("bad x: pos %d %d %d (x1000) pnmtx %d/%u fmt %d pos cnt %d type %d frac %d desc %d; clip %d %d %d %d",
                   (int)(v0.pos[0] * 1000), (int)(v0.pos[1] * 1000), (int)(v0.pos[2] * 1000), v0.has_pnmtx, mid, fmt,
                   __gx.vtx_fmt[fmt][GX_VA_POS].cnt, __gx.vtx_fmt[fmt][GX_VA_POS].type,
                   __gx.vtx_fmt[fmt][GX_VA_POS].frac, __gx.vtx_desc[GX_VA_POS], (int)out[0].pos[0], (int)out[0].pos[1],
                   (int)out[0].pos[2], (int)out[0].pos[3]);
        for (int r = 0; r < 3; r++)
            __port_log("  mtx row %d: %d %d %d %d (x1000)", r, (int)(m[r * 4] * 1000), (int)(m[r * 4 + 1] * 1000),
                       (int)(m[r * 4 + 2] * 1000), (int)(m[r * 4 + 3] * 1000));
        __port_log("  proj %d: %d %d %d %d %d %d (x1000); viewport %d %d %d %d", (int)__gx.projection[0],
                   (int)(__gx.projection[1] * 1000), (int)(__gx.projection[2] * 1000), (int)(__gx.projection[3] * 1000),
                   (int)(__gx.projection[4] * 1000), (int)(__gx.projection[5] * 1000), (int)(__gx.projection[6] * 1000),
                   (int)__gx.viewport[0], (int)__gx.viewport[1], (int)__gx.viewport[2], (int)__gx.viewport[3]);
    }
    /* Bring-up aid: the translucent untextured draws of frame 1000, with
     * what they became, and how many draws came before and after. */
    static unsigned f1000_draws;
    if (__port_frame_number_peek() == 1000) {
        f1000_draws++;
        if (written && !d.unit[0].tex && out[0].color[3] < 255) {
            const GpuStage* g = &d.stage[0];
            __port_log("f1000 draw %u (vertex %u): %u verts proj %d color %02x%02x%02x%02x | blend %d eq %d %d/%d"
                       " | mask %02x depth %d/%d | stage0 rgb %d %d %d f%d a %d %d %d f%d | stages %d",
                       f1000_draws, vused, written, (int)__gx.projection[0], out[0].color[0], out[0].color[1],
                       out[0].color[2], out[0].color[3], d.blend, d.blend_eq, d.blend_src, d.blend_dst, d.write_mask,
                       d.depth_test, d.depth_func, g->src_rgb[0], g->src_rgb[1], g->src_rgb[2], g->func_rgb,
                       g->src_a[0], g->src_a[1], g->src_a[2], g->func_a, d.num_stages);
        }
    } else if (f1000_draws && __port_frame_number_peek() == 1001) {
        __port_log("f1000: %u draws in all", f1000_draws);
        f1000_draws = 0;
    }
    /* Bring-up aid: the first orthographic (2D) draws, with their extent. */
    static int ortho_logged;
    if (__gx.projection[0] == (float)GXENUM_ORTHOGRAPHIC && ortho_logged < 40 && written && d.unit[0].tex) {
        ortho_logged++;
        float mn[3] = { 1e9f, 1e9f, 1e9f }, mx[3] = { -1e9f, -1e9f, -1e9f };
        for (unsigned i = 0; i < written; i++)
            for (int k = 0; k < 3; k++) {
                float v = out[i].pos[k] / out[i].pos[3];
                if (v < mn[k]) mn[k] = v;
                if (v > mx[k]) mx[k] = v;
            }
        __port_log("ortho draw: %u verts x %d..%d y %d..%d z %d..%d (x1000) tex %u z %d/%d blend %d stages %d clip %d",
                   written, (int)(mn[0] * 1000), (int)(mx[0] * 1000), (int)(mn[1] * 1000), (int)(mx[1] * 1000),
                   (int)(mn[2] * 1000), (int)(mx[2] * 1000), (unsigned)d.unit[0].tex, __gx.z_compare,
                   __gx.z_update, __gx.blend_type, d.num_stages, __gx.clip_mode);
        if (mx[0] - mn[0] > 100.0f || mn[1] != mn[1]) {
            __port_log("  bad: %s fmt %d size %d count %u | pos desc %d cnt %d type %d frac %d | clr0 %d %d | tex0 %d %d %d %d"
                       " | pnmtx %d | first bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x",
                       data == __gx.fifo_base ? "immediate" : "list", fmt, vertex_size(fmt), count,
                       __gx.vtx_desc[GX_VA_POS], __gx.vtx_fmt[fmt][GX_VA_POS].cnt, __gx.vtx_fmt[fmt][GX_VA_POS].type,
                       __gx.vtx_fmt[fmt][GX_VA_POS].frac, __gx.vtx_desc[GX_VA_CLR0],
                       __gx.vtx_fmt[fmt][GX_VA_CLR0].type, __gx.vtx_desc[GX_VA_TEX0],
                       __gx.vtx_fmt[fmt][GX_VA_TEX0].cnt, __gx.vtx_fmt[fmt][GX_VA_TEX0].type,
                       __gx.vtx_fmt[fmt][GX_VA_TEX0].frac, __gx.vtx_desc[GX_VA_PNMTXIDX], data[0], data[1],
                       data[2], data[3], data[4], data[5], data[6], data[7], data[8], data[9], data[10], data[11]);
            const float* m = pos_matrix(__gx.current_mtx);
            __port_log("  mtx %u: %d %d %d %d / %d %d %d %d (x1000); proj %d %d %d %d %d %d", (unsigned)__gx.current_mtx,
                       (int)(m[0] * 1000), (int)(m[1] * 1000), (int)(m[2] * 1000), (int)(m[3] * 1000),
                       (int)(m[4] * 1000), (int)(m[5] * 1000), (int)(m[6] * 1000), (int)(m[7] * 1000),
                       (int)(__gx.projection[1] * 1000), (int)(__gx.projection[2] * 1000),
                       (int)(__gx.projection[3] * 1000), (int)(__gx.projection[4] * 1000),
                       (int)(__gx.projection[5] * 1000), (int)(__gx.projection[6] * 1000));
        }
    }
    PROF_END(PROF_VERTICES, t_vtx);
    d.first = vused;
    d.count = written;
    vused += written;
    submit(&d);
}

/* ---- immediate mode ------------------------------------------------------------ */

unsigned char* __GXFifoEnd;            /* where the current primitive's data ends */

void GXBegin(int type, int fmt, gx_u16 nverts)
{
    __gx_ensure_frame();
    __gx.prim_type = (gx_u8)type;
    __gx.prim_fmt = (gx_u8)fmt;
    __gx.prim_count = nverts;
    __GXFifoPtr = __gx.fifo_base;
    __GXFifoEnd = __GXFifoPtr + (unsigned)vertex_size(fmt & 7) * nverts;
    if (__GXFifoEnd == __GXFifoPtr)
        __GXFifoEnd = 0;
}

/* Called by the FIFO writers when the primitive's last byte is in. */
void __GXFifoDone(void)
{
    __GXFifoEnd = 0;
    draw(__gx.prim_type, __gx.prim_fmt & 7, __gx.fifo_base, __gx.prim_count);
}

/* ---- display lists -------------------------------------------------------------- */

static void load_indexed(int array, unsigned v)
{
    unsigned idx = v >> 16, size = ((v >> 12) & 15) + 1, addr = v & 0xFFF;
    const unsigned char* base = __gx.array[array].base;
    if (!base)
        return;
    const unsigned char* src = base + idx * __gx.array[array].stride;
    float f[16];
    for (unsigned i = 0; i < size && i < 16; i++) {
        unsigned u = rd32(src + i * 4);
        memcpy(&f[i], &u, 4);
    }
    if (array == GX_VA_POS_MTX_ARRAY || array == GX_VA_TEX_MTX_ARRAY) {
        unsigned id = addr / 4;
        float* m = id < 30 ? &__gx.pos_mtx[(id / 3) % GX_PNMTX_COUNT][0][0]
                           : &__gx.tex_mtx[((id - 30) / 3) % GX_TEXMTX_COUNT][0][0];
        memcpy(m, f, (size > 12 ? 12 : size) * 4);
    } else if (array == GX_VA_NRM_MTX_ARRAY && addr >= 0x400) {
        unsigned id = (addr - 0x400) / 3;
        float* m = &__gx.nrm_mtx[(id / 3) % GX_PNMTX_COUNT][0][0];
        memcpy(m, f, (size > 9 ? 9 : size) * 4);
    }
}

static int dl_warned;

static void run_list(const unsigned char* p, const unsigned char* end, int depth)
{
    while (p < end) {
        unsigned op = *p++;
        if (op >= 0x80 && op < 0xC0) {
            if (p + 2 > end)
                return;
            unsigned count = rd16(p);
            p += 2;
            int fmt = op & 7;
            unsigned bytes = (unsigned)vertex_size(fmt) * count;
            if (p + bytes > end)
                return;
            draw(op & 0xF8, fmt, p, count);
            p += bytes;
            continue;
        }
        switch (op) {
        case 0x00: case 0x48: break;                                 /* NOP, invalidate VC */
        case 0x08: p += 5; break;                                    /* CP register */
        case 0x10: { unsigned v = rd32(p); p += 4 + ((v >> 16) + 1) * 4; break; }   /* XF */
        case 0x20: load_indexed(GX_VA_POS_MTX_ARRAY, rd32(p)); p += 4; break;
        case 0x28: load_indexed(GX_VA_NRM_MTX_ARRAY, rd32(p)); p += 4; break;
        case 0x30: load_indexed(GX_VA_TEX_MTX_ARRAY, rd32(p)); p += 4; break;
        case 0x38: p += 4; break;                                    /* light, not used */
        case 0x40:
            if (depth < 4)
                run_list((const unsigned char*)rd32(p), (const unsigned char*)rd32(p) + rd32(p + 4), depth + 1);
            p += 8;
            break;
        case 0x61: p += 4; break;                                    /* BP register */
        default:
            if (!dl_warned++)
                __port_log("gx: unknown display list command 0x%02x", op);
            return;
        }
    }
}

void GXCallDisplayList(const void* list, gx_u32 nbytes)
{
    __gx_ensure_frame();
    run_list(list, (const unsigned char*)list + nbytes, 0);
}
