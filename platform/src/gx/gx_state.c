/*
 * gx_state.c - GX state setters.
 *
 * Each function stores its arguments in __gx (see gx_state.h) with the
 * GameCube meaning. Nothing is drawn here. Signatures follow the Dolphin SDK;
 * enum arguments arrive as int, as they do from game code.
 */
#include <string.h>

#include "gx_state.h"

GXState __gx;

#define DIRTY() (__gx.dirty = 1)

/* ---- matrices -------------------------------------------------------------- */

void GXLoadPosMtxImm(const GXMtx m, gx_u32 id)
{
    if (id / 3 < GX_PNMTX_COUNT)
        memcpy(__gx.pos_mtx[id / 3], m, sizeof(GXMtx));
    DIRTY();
}

void GXLoadNrmMtxImm(const GXMtx m, gx_u32 id)
{
    if (id / 3 < GX_PNMTX_COUNT)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 3; c++)
                __gx.nrm_mtx[id / 3][r][c] = m[r][c];
    DIRTY();
}

void GXLoadTexMtxImm(const gx_f32 m[][4], gx_u32 id, int type)
{
    int rows = type == 1 ? 2 : 3;           /* GX_MTX2x4 : GX_MTX3x4 */
    if (id >= 64) {
        gx_u32 i = (id - 64) / 3;
        if (i < GX_PTTEXMTX_COUNT) {
            memset(__gx.post_mtx[i], 0, sizeof(GXMtx));
            memcpy(__gx.post_mtx[i], m, (size_t)rows * sizeof(m[0]));
        }
    } else if (id >= 30) {
        gx_u32 i = (id - 30) / 3;
        if (i < GX_TEXMTX_COUNT) {
            memset(__gx.tex_mtx[i], 0, sizeof(GXMtx));
            memcpy(__gx.tex_mtx[i], m, (size_t)rows * sizeof(m[0]));
            __gx.tex_mtx_type[i] = (gx_u8)type;
        }
    }
    DIRTY();
}

void GXSetCurrentMtx(gx_u32 id) { __gx.current_mtx = id; DIRTY(); }

void GXSetProjection(const GXMtx44 m, int type)
{
    __gx.projection[0] = (gx_f32)type;
    __gx.projection[1] = m[0][0];
    __gx.projection[3] = m[1][1];
    __gx.projection[5] = m[2][2];
    __gx.projection[6] = m[2][3];
    if (type == GXENUM_PERSPECTIVE) {
        __gx.projection[2] = m[0][2];
        __gx.projection[4] = m[1][2];
    } else {
        __gx.projection[2] = m[0][3];
        __gx.projection[4] = m[1][3];
    }
    DIRTY();
}

void GXGetProjectionv(gx_f32* p) { memcpy(p, __gx.projection, sizeof(__gx.projection)); }

void GXSetViewportJitter(gx_f32 left, gx_f32 top, gx_f32 wd, gx_f32 ht, gx_f32 nearz, gx_f32 farz,
                         gx_u32 field)
{
    __gx.viewport[0] = left;
    __gx.viewport[1] = top;
    __gx.viewport[2] = wd;
    __gx.viewport[3] = ht;
    __gx.viewport[4] = nearz;
    __gx.viewport[5] = farz;
    __gx.viewport_field = field;
    DIRTY();
}

void GXSetViewport(gx_f32 left, gx_f32 top, gx_f32 wd, gx_f32 ht, gx_f32 nearz, gx_f32 farz)
{
    GXSetViewportJitter(left, top, wd, ht, nearz, farz, 1);
}

void GXGetViewportv(gx_f32* vp) { memcpy(vp, __gx.viewport, sizeof(__gx.viewport)); }

/* The SDK's projection of a point to screen coordinates. */
void GXProject(gx_f32 x, gx_f32 y, gx_f32 z, const GXMtx mtx, const gx_f32* pm, const gx_f32* vp,
               gx_f32* sx, gx_f32* sy, gx_f32* sz)
{
    gx_f32 ex = mtx[0][0] * x + mtx[0][1] * y + mtx[0][2] * z + mtx[0][3];
    gx_f32 ey = mtx[1][0] * x + mtx[1][1] * y + mtx[1][2] * z + mtx[1][3];
    gx_f32 ez = mtx[2][0] * x + mtx[2][1] * y + mtx[2][2] * z + mtx[2][3];
    gx_f32 xc, yc, zc, wc;

    if (pm[0] == (gx_f32)GXENUM_PERSPECTIVE) {
        xc = ex * pm[1] + ez * pm[2];
        yc = ey * pm[3] + ez * pm[4];
        zc = pm[6] + ez * pm[5];
        wc = 1.0f / -ez;
    } else {
        xc = ex * pm[1] + pm[2];
        yc = ey * pm[3] + pm[4];
        zc = pm[6] + ez * pm[5];
        wc = 1.0f;
    }
    *sx = vp[2] / 2.0f + vp[0] + wc * xc * vp[2] / 2.0f;
    *sy = vp[3] / 2.0f + vp[1] - wc * yc * vp[3] / 2.0f;
    *sz = vp[5] + wc * zc * (vp[5] - vp[4]);
}

void GXSetScissor(gx_u32 left, gx_u32 top, gx_u32 wd, gx_u32 ht)
{
    __gx.scissor[0] = left;
    __gx.scissor[1] = top;
    __gx.scissor[2] = wd;
    __gx.scissor[3] = ht;
    DIRTY();
}

void GXSetClipMode(int mode) { __gx.clip_mode = (gx_u8)mode; DIRTY(); }
void GXSetCullMode(int mode) { __gx.cull_mode = (gx_u8)mode; DIRTY(); }
void GXSetCoPlanar(int enable) { __gx.coplanar = (gx_u8)enable; DIRTY(); }

void GXSetLineWidth(gx_u8 width, int tex_offsets)
{
    __gx.line_width = width;
    __gx.line_tex_offset = (gx_u8)tex_offsets;
    DIRTY();
}

void GXSetPointSize(gx_u8 size, int tex_offsets)
{
    __gx.point_size = size;
    __gx.point_tex_offset = (gx_u8)tex_offsets;
    DIRTY();
}

void GXEnableTexOffsets(int coord, int line_enable, int point_enable)
{
    (void)coord; (void)line_enable; (void)point_enable;   /* lines and points only */
}

/* ---- vertex format ----------------------------------------------------------- */

void GXClearVtxDesc(void) { memset(__gx.vtx_desc, 0, sizeof(__gx.vtx_desc)); DIRTY(); }

void GXSetVtxDesc(int attr, int type)
{
    if (attr >= 0 && attr < GX_MAX_ATTR)
        __gx.vtx_desc[attr] = (gx_u8)type;
    DIRTY();
}

void GXSetVtxAttrFmt(int fmt, int attr, int cnt, int type, gx_u8 frac)
{
    if (fmt >= 0 && fmt < GX_MAX_VTXFMT && attr >= 0 && attr < GX_MAX_ATTR) {
        __gx.vtx_fmt[fmt][attr].cnt = (gx_u8)cnt;
        __gx.vtx_fmt[fmt][attr].type = (gx_u8)type;
        __gx.vtx_fmt[fmt][attr].frac = frac;
    }
    DIRTY();
}

void GXSetArray(int attr, const void* base, gx_u8 stride)
{
    if (attr >= 0 && attr < GX_MAX_ATTR) {
        __gx.array[attr].base = base;
        __gx.array[attr].stride = stride;
    }
    DIRTY();
}

void GXInvalidateVtxCache(void) {}

/* ---- texture coordinate generation ------------------------------------------- */

void GXSetNumTexGens(gx_u8 n) { __gx.num_texgens = n; DIRTY(); }

void GXSetTexCoordGen2(int dst, int func, int src, gx_u32 mtx, int normalize, gx_u32 postmtx)
{
    if (dst >= 0 && dst < GX_MAX_TEXCOORD) {
        __gx.texgen[dst].func = (gx_u8)func;
        __gx.texgen[dst].src = (gx_u8)src;
        __gx.texgen[dst].mtx = mtx;
        __gx.texgen[dst].normalize = (gx_u8)normalize;
        __gx.texgen[dst].postmtx = postmtx;
    }
    DIRTY();
}

/* ---- lighting channels -------------------------------------------------------- */

void GXSetNumChans(gx_u8 n) { __gx.num_chans = n; DIRTY(); }

static void set_chan(int i, int enable, int amb, int mat, gx_u32 mask, int diff, int attn)
{
    __gx.chan[i].enable = (gx_u8)enable;
    __gx.chan[i].amb_src = (gx_u8)amb;
    __gx.chan[i].mat_src = (gx_u8)mat;
    __gx.chan[i].light_mask = mask;
    __gx.chan[i].diff_fn = (gx_u8)diff;
    __gx.chan[i].attn_fn = (gx_u8)attn;
}

/* GX_COLOR0 0, GX_COLOR1 1, GX_ALPHA0 2, GX_ALPHA1 3, GX_COLOR0A0 4, GX_COLOR1A1 5 */
void GXSetChanCtrl(int chan, int enable, int amb_src, int mat_src, gx_u32 light_mask, int diff_fn,
                   int attn_fn)
{
    if (chan == 4 || chan == 5) {
        set_chan(chan - 4, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn);
        set_chan(chan - 2, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn);
    } else if (chan >= 0 && chan < 4) {
        set_chan(chan, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn);
    }
    DIRTY();
}

/* The colour setters take GX_COLOR0/1, GX_ALPHA0/1 or GX_COLOR0A0/1A1. */
static void set_chan_color(GXColor* slot, int chan, GXColor c)
{
    switch (chan) {
    case 0: case 1: slot[chan].r = c.r; slot[chan].g = c.g; slot[chan].b = c.b; break;
    case 2: case 3: slot[chan - 2].a = c.a; break;
    case 4: case 5: slot[chan - 4] = c; break;
    }
    DIRTY();
}

void GXSetChanAmbColor(int chan, GXColor c) { set_chan_color(__gx.chan_amb, chan, c); }
void GXSetChanMatColor(int chan, GXColor c) { set_chan_color(__gx.chan_mat, chan, c); }

/* GX_LIGHT0 = 1 << 0 ... GX_LIGHT7 = 1 << 7 */
void GXLoadLightObjImm(const GXLightObj* lt, gx_u32 light)
{
    for (int i = 0; i < GX_MAX_LIGHT; i++)
        if (light == (1u << i))
            __gx.light[i] = *lt;
    DIRTY();
}

/* ---- textures ------------------------------------------------------------------ */

void GXLoadTexObj(const GXTexObj* obj, int id)
{
    if (id >= 0 && id < GX_MAX_TEXMAP) {
        __gx.texmap[id] = *obj;
        __gx.texmap_loaded[id] = 1;
    }
    DIRTY();
}

void GXInitTlutObj(GXTlutObj* tlut, void* lut, int fmt, gx_u16 n_entries)
{
    tlut->data[0] = (gx_u32)fmt;
    tlut->data[1] = (gx_u32)lut;
    tlut->data[2] = n_entries;
}

void GXLoadTlut(const GXTlutObj* tlut, gx_u32 name)
{
    if (name < GX_MAX_TLUT)
        __gx.tlut[name] = *tlut;
    DIRTY();
}

void GXInvalidateTexAll(void) {}

void GXSetZTexture(int op, int fmt, gx_u32 bias)
{
    __gx.ztex_op = (gx_u8)op;
    __gx.ztex_fmt = (gx_u8)fmt;
    __gx.ztex_bias = bias;
    DIRTY();
}

/* ---- TEV ----------------------------------------------------------------------- */

#define STAGE(s) if ((s) < 0 || (s) >= GX_MAX_TEVSTAGE) return; GXTevStageState* t = &__gx.tev[s]

void GXSetNumTevStages(gx_u8 n) { __gx.num_tev_stages = n; DIRTY(); }

void GXSetTevColorIn(int stage, int a, int b, int c, int d)
{
    STAGE(stage);
    t->color_in[0] = (gx_u8)a; t->color_in[1] = (gx_u8)b; t->color_in[2] = (gx_u8)c; t->color_in[3] = (gx_u8)d;
    DIRTY();
}

void GXSetTevAlphaIn(int stage, int a, int b, int c, int d)
{
    STAGE(stage);
    t->alpha_in[0] = (gx_u8)a; t->alpha_in[1] = (gx_u8)b; t->alpha_in[2] = (gx_u8)c; t->alpha_in[3] = (gx_u8)d;
    DIRTY();
}

void GXSetTevColorOp(int stage, int op, int bias, int scale, int clamp, int out)
{
    STAGE(stage);
    t->color_op = (gx_u8)op; t->color_bias = (gx_u8)bias; t->color_scale = (gx_u8)scale;
    t->color_clamp = (gx_u8)clamp; t->color_out = (gx_u8)out;
    DIRTY();
}

void GXSetTevAlphaOp(int stage, int op, int bias, int scale, int clamp, int out)
{
    STAGE(stage);
    t->alpha_op = (gx_u8)op; t->alpha_bias = (gx_u8)bias; t->alpha_scale = (gx_u8)scale;
    t->alpha_clamp = (gx_u8)clamp; t->alpha_out = (gx_u8)out;
    DIRTY();
}

/* The SDK's preset modes. Stage 0 combines with the rasterised colour,
 * later stages with the previous stage's result. */
void GXSetTevOp(int stage, int mode)
{
    enum { CPREV = 0, APREV_C = 1, TEXC = 8, TEXA_C = 9, RASC = 10, ONE = 12, ZEROC = 15 };
    enum { APREV = 0, TEXA = 4, RASA = 5, ZEROA = 7 };
    int carg = stage == 0 ? RASC : CPREV;
    int aarg = stage == 0 ? RASA : APREV;

    switch (mode) {
    case 0: /* GX_MODULATE */
        GXSetTevColorIn(stage, ZEROC, TEXC, carg, ZEROC);
        GXSetTevAlphaIn(stage, ZEROA, TEXA, aarg, ZEROA);
        break;
    case 1: /* GX_DECAL */
        GXSetTevColorIn(stage, carg, TEXC, TEXA_C, ZEROC);
        GXSetTevAlphaIn(stage, ZEROA, ZEROA, ZEROA, aarg);
        break;
    case 2: /* GX_BLEND */
        GXSetTevColorIn(stage, carg, ONE, TEXC, ZEROC);
        GXSetTevAlphaIn(stage, ZEROA, TEXA, aarg, ZEROA);
        break;
    case 3: /* GX_REPLACE */
        GXSetTevColorIn(stage, ZEROC, ZEROC, ZEROC, TEXC);
        GXSetTevAlphaIn(stage, ZEROA, ZEROA, ZEROA, TEXA);
        break;
    case 4: /* GX_PASSCLR */
        GXSetTevColorIn(stage, ZEROC, ZEROC, ZEROC, carg);
        GXSetTevAlphaIn(stage, ZEROA, ZEROA, ZEROA, aarg);
        break;
    }
    (void)APREV_C;
    GXSetTevColorOp(stage, 0, 0, 0, 1, 0);   /* ADD, bias 0, scale 1, clamp, PREV */
    GXSetTevAlphaOp(stage, 0, 0, 0, 1, 0);
}

void GXSetTevColor(int id, GXColor c)
{
    if (id >= 0 && id < 4) {
        __gx.tev_color[id].r = c.r; __gx.tev_color[id].g = c.g;
        __gx.tev_color[id].b = c.b; __gx.tev_color[id].a = c.a;
    }
    DIRTY();
}

void GXSetTevColorS10(int id, GXColorS10 c)
{
    if (id >= 0 && id < 4)
        __gx.tev_color[id] = c;
    DIRTY();
}

void GXSetTevKColor(int id, GXColor c)
{
    if (id >= 0 && id < 4)
        __gx.tev_kcolor[id] = c;
    DIRTY();
}

void GXSetTevKColorSel(int stage, int sel) { STAGE(stage); t->kcsel = (gx_u8)sel; DIRTY(); }
void GXSetTevKAlphaSel(int stage, int sel) { STAGE(stage); t->kasel = (gx_u8)sel; DIRTY(); }

void GXSetTevSwapMode(int stage, int ras_sel, int tex_sel)
{
    STAGE(stage);
    t->ras_swap = (gx_u8)ras_sel;
    t->tex_swap = (gx_u8)tex_sel;
    DIRTY();
}

void GXSetTevSwapModeTable(int table, int r, int g, int b, int a)
{
    if (table >= 0 && table < 4) {
        __gx.swap_table[table][0] = (gx_u8)r; __gx.swap_table[table][1] = (gx_u8)g;
        __gx.swap_table[table][2] = (gx_u8)b; __gx.swap_table[table][3] = (gx_u8)a;
    }
    DIRTY();
}

void GXSetTevOrder(int stage, int coord, int map, int color)
{
    STAGE(stage);
    t->coord = (gx_u8)coord;
    t->map = (gx_u8)map;
    t->chan = (gx_u8)color;
    DIRTY();
}

void GXSetTevIndirect(int stage, int ind_stage, int format, int bias, int mtx, int wrap_s, int wrap_t,
                      int add_prev, int utc_lod, int alpha)
{
    STAGE(stage);
    t->ind_stage = (gx_u8)ind_stage; t->ind_format = (gx_u8)format; t->ind_bias = (gx_u8)bias;
    t->ind_mtx = (gx_u8)mtx; t->ind_wrap_s = (gx_u8)wrap_s; t->ind_wrap_t = (gx_u8)wrap_t;
    t->ind_add_prev = (gx_u8)add_prev; t->ind_utc_lod = (gx_u8)utc_lod; t->ind_alpha = (gx_u8)alpha;
    DIRTY();
}

/* No indirect lookup: GX_INDTEXSTAGE0, format 8 bit, no bias, no matrix,
 * no wrap, no add, no LOD, no alpha. */
void GXSetTevDirect(int stage) { GXSetTevIndirect(stage, 0, 0, 0, 0, 0, 0, 0, 0, 0); }

void GXSetNumIndStages(gx_u8 n) { __gx.num_ind_stages = n; DIRTY(); }

void GXSetIndTexOrder(int ind_stage, int coord, int map)
{
    if (ind_stage >= 0 && ind_stage < GX_MAX_INDSTAGE) {
        __gx.ind[ind_stage].coord = (gx_u8)coord;
        __gx.ind[ind_stage].map = (gx_u8)map;
    }
    DIRTY();
}

void GXSetIndTexCoordScale(int ind_stage, int scale_s, int scale_t)
{
    if (ind_stage >= 0 && ind_stage < GX_MAX_INDSTAGE) {
        __gx.ind[ind_stage].scale_s = (gx_u8)scale_s;
        __gx.ind[ind_stage].scale_t = (gx_u8)scale_t;
    }
    DIRTY();
}

/* GX_ITM_0..2 = 1..3 (and the S/T variants above, which carry no matrix) */
void GXSetIndTexMtx(int id, const gx_f32 offset[2][3], gx_s8 scale_exp)
{
    int i = id >= 1 && id <= 3 ? id - 1 : -1;
    if (i >= 0) {
        memcpy(__gx.ind_mtx[i].m, offset, sizeof(__gx.ind_mtx[i].m));
        __gx.ind_mtx[i].scale_exp = scale_exp;
    }
    DIRTY();
}

void GXSetAlphaCompare(int comp0, gx_u8 ref0, int op, int comp1, gx_u8 ref1)
{
    __gx.alpha_comp0 = (gx_u8)comp0; __gx.alpha_ref0 = ref0; __gx.alpha_op = (gx_u8)op;
    __gx.alpha_comp1 = (gx_u8)comp1; __gx.alpha_ref1 = ref1;
    DIRTY();
}

/* ---- pixel engine ----------------------------------------------------------- */

void GXSetBlendMode(int type, int src, int dst, int op)
{
    __gx.blend_type = (gx_u8)type; __gx.blend_src = (gx_u8)src;
    __gx.blend_dst = (gx_u8)dst; __gx.blend_op = (gx_u8)op;
    DIRTY();
}

void GXSetColorUpdate(int enable) { __gx.color_update = (gx_u8)enable; DIRTY(); }
void GXSetAlphaUpdate(int enable) { __gx.alpha_update = (gx_u8)enable; DIRTY(); }

void GXSetZMode(int compare, int func, int update)
{
    __gx.z_compare = (gx_u8)compare; __gx.z_func = (gx_u8)func; __gx.z_update = (gx_u8)update;
    DIRTY();
}

void GXSetZCompLoc(int before_tex) { __gx.z_before_tex = (gx_u8)before_tex; DIRTY(); }
void GXSetDither(int enable) { __gx.dither = (gx_u8)enable; DIRTY(); }

void GXSetDstAlpha(int enable, gx_u8 alpha)
{
    __gx.dst_alpha_enable = (gx_u8)enable;
    __gx.dst_alpha = alpha;
    DIRTY();
}

void GXSetPixelFmt(int pix_fmt, int z_fmt)
{
    __gx.pixel_fmt = (gx_u8)pix_fmt;
    __gx.z_fmt = (gx_u8)z_fmt;
    DIRTY();
}

void GXSetFieldMode(int field_mode, int half_aspect)
{
    __gx.field_mode = (gx_u8)field_mode;
    __gx.half_aspect = (gx_u8)half_aspect;
}

void GXSetFog(int type, gx_f32 startz, gx_f32 endz, gx_f32 nearz, gx_f32 farz, GXColor color)
{
    __gx.fog_type = (gx_u8)type;
    __gx.fog_start = startz; __gx.fog_end = endz;
    __gx.fog_near = nearz; __gx.fog_far = farz;
    __gx.fog_color = color;
    DIRTY();
}

void GXSetFogRangeAdj(int enable, gx_u16 center, const GXFogAdjTable* table)
{
    __gx.fog_range_enable = (gx_u8)enable;
    __gx.fog_range_center = center;
    if (table)
        __gx.fog_range_table = *table;
    DIRTY();
}

/* The SDK's range-adjustment table: distance from the screen centre to the
 * edge of each of ten bands, scaled by the projection's x factor. */
void GXInitFogAdjTable(GXFogAdjTable* table, gx_u16 width, const gx_f32 proj[4][4])
{
    gx_f32 xi, iw, rangeVal, nearZ, sideX;
    if (proj[3][3] == 0.0f) {
        nearZ = proj[2][3] / (proj[2][2] - 1.0f);
        sideX = nearZ / proj[0][0];
    } else {
        sideX = 1.0f / proj[0][0];
        nearZ = 1.73205f * sideX;
    }
    iw = 2.0f / width;
    for (int i = 0; i < 10; i++) {
        xi = (i + 1) << 5;
        xi *= iw;
        xi *= sideX;
        rangeVal = __builtin_sqrtf(1.0f + (xi * xi) / (nearZ * nearZ));
        table->r[i] = (gx_u16)((gx_u32)(256.0f * rangeVal) & 0xFFF);
    }
}

/* ---- defaults, as __GXInitGX sets them -------------------------------------- */

void __gx_state_defaults(void)
{
    memset(&__gx, 0, sizeof(__gx));
    __gx.copy_clear_color = (GXColor){ 64, 64, 64, 255 };
    __gx.copy_clear_z = 0xFFFFFF;
    GXSetViewport(0.0f, 0.0f, 640.0f, 480.0f, 0.0f, 1.0f);
    GXSetScissor(0, 0, 640, 480);
    __gx.cull_mode = 2;                       /* GX_CULL_BACK */
    GXSetZMode(1, 3, 1);                      /* compare, GX_LEQUAL, update */
    __gx.color_update = __gx.alpha_update = 1;
    __gx.num_chans = 0;
    __gx.num_tev_stages = 1;
    for (int t = 0; t < 4; t++) {
        static const gx_u8 swaps[4][4] = { { 0, 1, 2, 3 }, { 0, 0, 0, 3 }, { 1, 1, 1, 3 }, { 2, 2, 2, 3 } };
        GXSetTevSwapModeTable(t, swaps[t][0], swaps[t][1], swaps[t][2], swaps[t][3]);
    }
    for (int s = 0; s < GX_MAX_TEVSTAGE; s++) {
        GXSetTevOp(s, 3);                      /* GX_REPLACE */
        GXSetTevOrder(s, 0xFF, 0xFF, 0xFF);    /* GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR_NULL */
    }
    for (int i = 0; i < GX_PNMTX_COUNT; i++) {
        __gx.pos_mtx[i][0][0] = __gx.pos_mtx[i][1][1] = __gx.pos_mtx[i][2][2] = 1.0f;
        __gx.nrm_mtx[i][0][0] = __gx.nrm_mtx[i][1][1] = __gx.nrm_mtx[i][2][2] = 1.0f;
    }
    __gx.dirty = 1;
}
