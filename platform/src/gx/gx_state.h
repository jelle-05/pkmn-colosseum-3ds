/*
 * gx_state.h - the GX state the platform keeps instead of GPU registers.
 *
 * Every GXSet* call stores its arguments here, in GameCube terms. The
 * renderer (to come) reads this state when it draws and translates it to
 * the PICA200. Keeping the GameCube meaning here, rather than translating
 * on every call, keeps the setters trivial and puts all PICA knowledge in
 * one place.
 */
#ifndef PLATFORM_GX_STATE_H
#define PLATFORM_GX_STATE_H

#include "port/gx_types.h"

typedef struct {
    gx_u8 color_in[4], alpha_in[4];
    gx_u8 color_op, color_bias, color_scale, color_clamp, color_out;
    gx_u8 alpha_op, alpha_bias, alpha_scale, alpha_clamp, alpha_out;
    gx_u8 coord, map, chan;          /* GXSetTevOrder */
    gx_u8 kcsel, kasel;
    gx_u8 ras_swap, tex_swap;
    /* GXSetTevIndirect */
    gx_u8 ind_stage, ind_format, ind_bias, ind_mtx, ind_wrap_s, ind_wrap_t;
    gx_u8 ind_add_prev, ind_utc_lod, ind_alpha;
} GXTevStageState;

typedef struct {
    /* memory */
    void* fifo_base;
    gx_u32 fifo_size;

    /* EFB and copies */
    GXColor copy_clear_color;
    gx_u32 copy_clear_z;
    gx_u16 disp_src[4];              /* left, top, width, height */
    gx_u16 disp_dst_w, disp_dst_h;
    gx_f32 disp_yscale;
    gx_u8 disp_gamma, copy_clamp, copy_aa, copy_vf;
    gx_u16 tex_src[4];
    gx_u16 tex_dst_w, tex_dst_h;
    gx_u8 tex_dst_fmt, tex_dst_mipmap;
    gx_u8 pixel_fmt, z_fmt, field_mode, half_aspect;

    /* transform */
    gx_f32 viewport[6];              /* left, top, width, height, near, far */
    gx_u32 viewport_field;
    gx_u32 scissor[4];
    gx_f32 projection[7];            /* GXGetProjectionv layout */
    GXMtx pos_mtx[GX_PNMTX_COUNT];
    gx_f32 nrm_mtx[GX_PNMTX_COUNT][3][3];
    GXMtx tex_mtx[GX_TEXMTX_COUNT];
    gx_u8 tex_mtx_type[GX_TEXMTX_COUNT];
    GXMtx post_mtx[GX_PTTEXMTX_COUNT];
    gx_u32 current_mtx;
    gx_u8 clip_mode, cull_mode, coplanar;
    gx_u8 line_width, point_size, line_tex_offset, point_tex_offset;

    /* vertex format */
    gx_u8 vtx_desc[GX_MAX_ATTR];
    struct { gx_u8 cnt, type, frac; } vtx_fmt[GX_MAX_VTXFMT][GX_MAX_ATTR];
    struct { const void* base; gx_u8 stride; } array[GX_MAX_ATTR];

    /* texture coordinate generation */
    gx_u8 num_texgens;
    struct { gx_u8 func, src, normalize; gx_u32 mtx, postmtx; } texgen[GX_MAX_TEXCOORD];

    /* lighting */
    gx_u8 num_chans;
    struct { gx_u8 enable, amb_src, mat_src, diff_fn, attn_fn; gx_u32 light_mask; } chan[4];
    GXColor chan_amb[2], chan_mat[2];
    GXLightObj light[GX_MAX_LIGHT];

    /* textures */
    GXTexObj texmap[GX_MAX_TEXMAP];
    gx_u8 texmap_loaded[GX_MAX_TEXMAP];
    GXTlutObj tlut[GX_MAX_TLUT];
    gx_u8 ztex_op, ztex_fmt;
    gx_u32 ztex_bias;

    /* TEV */
    gx_u8 num_tev_stages;
    GXTevStageState tev[GX_MAX_TEVSTAGE];
    GXColorS10 tev_color[4];
    GXColor tev_kcolor[4];
    gx_u8 swap_table[4][4];
    gx_u8 alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;

    /* indirect texturing */
    gx_u8 num_ind_stages;
    struct { gx_u8 coord, map, scale_s, scale_t; } ind[GX_MAX_INDSTAGE];
    struct { gx_f32 m[2][3]; gx_s8 scale_exp; } ind_mtx[3];

    /* pixel engine */
    gx_u8 blend_type, blend_src, blend_dst, blend_op;
    gx_u8 color_update, alpha_update;
    gx_u8 z_compare, z_func, z_update, z_before_tex;
    gx_u8 dither, dst_alpha_enable, dst_alpha;
    gx_u8 fog_type;
    gx_f32 fog_start, fog_end, fog_near, fog_far;
    GXColor fog_color;
    gx_u8 fog_range_enable;
    gx_u16 fog_range_center;
    GXFogAdjTable fog_range_table;

    /* primitive being written to the FIFO (GXBegin) */
    gx_u8 prim_type, prim_fmt;
    gx_u16 prim_count;

    /* synchronisation */
    GXDrawDoneCallback draw_done_cb;
    GXDrawSyncCallback draw_sync_cb;
    gx_u16 draw_sync_token;
    gx_u8 draw_done_pending, draw_sync_pending;

    gx_u32 dirty;                    /* set by every setter; the renderer clears it */
} GXState;

extern GXState __gx;

#endif /* PLATFORM_GX_STATE_H */
