/*
 * gpu.h - what the GX layer hands the host's renderer.
 *
 * The GX layer (platform/src/gx, big-endian) decodes the game's vertices,
 * transforms them to clip space and translates the GX state; the host
 * (platform/host/gpu.c, little-endian) draws with citro3d. Vertices and
 * draws are written by the big-endian side and read by the host and the
 * GPU, so they are stored little-endian: GCC's scalar_storage_order does
 * the byte order on the big-endian side and nothing on the host.
 *
 * Values are the PICA200's (libctru's GPU_* enums), copied here because the
 * big-endian side does not include <3ds.h>.
 */
#ifndef PORT_GPU_H
#define PORT_GPU_H

#define GPU_LE __attribute__((scalar_storage_order("little-endian")))

/* A vertex as the GPU reads it: clip-space position, colour, three
 * texture coordinate sets. 44 bytes. */
typedef struct GPU_LE {
    float pos[4];
    unsigned char color[4];         /* r, g, b, a */
    float tc[3][2];
} GpuVertex;

/* An index into the frame's vertex buffer, as the GPU reads it. */
typedef struct GPU_LE { unsigned short v; } GpuIndex;

#define GPU_MAX_UNITS 3
#define GPU_MAX_STAGES 6

typedef struct GPU_LE {
    unsigned int tex;               /* host texture handle, 0 = none */
    unsigned char wrap_s, wrap_t;   /* GPU_TEXTURE_WRAP_PARAM */
    unsigned char mag, min;         /* GPU_TEXTURE_FILTER_PARAM */
} GpuUnit;

/* One combiner stage, as C3D_TexEnv takes it. */
typedef struct GPU_LE {
    unsigned char src_rgb[3], op_rgb[3], func_rgb, scale_rgb;
    unsigned char src_a[3], op_a[3], func_a, scale_a;
    unsigned int color;             /* constant, 0xAABBGGRR */
} GpuStage;

typedef struct GPU_LE {
    unsigned int first, count;      /* triangles: indices in the frame's index buffer */
    unsigned int vertices;          /* vertices used in the frame's buffer so far */
    GpuUnit unit[GPU_MAX_UNITS];
    unsigned char num_stages;
    unsigned char alpha_test, alpha_func, alpha_ref;
    unsigned char blend, blend_eq, blend_src, blend_dst;
    unsigned char depth_test, depth_func, write_mask, cull;
    unsigned short scissor[4];      /* left, top, right, bottom in 3DS pixels; 0,0,0,0 = off */
    GpuStage stage[GPU_MAX_STAGES];
} GpuDraw;

/* PICA values used by the GX layer. */
enum {
    PICA_SRC_PRIMARY = 0x0, PICA_SRC_TEX0 = 0x3, PICA_SRC_TEX1 = 0x4, PICA_SRC_TEX2 = 0x5,
    PICA_SRC_CONSTANT = 0xE, PICA_SRC_PREVIOUS = 0xF,

    PICA_OP_RGB_COLOR = 0x0, PICA_OP_RGB_1_COLOR = 0x1, PICA_OP_RGB_ALPHA = 0x2,
    PICA_OP_RGB_1_ALPHA = 0x3,
    PICA_OP_A_ALPHA = 0x0, PICA_OP_A_1_ALPHA = 0x1,

    PICA_REPLACE = 0x0, PICA_MODULATE = 0x1, PICA_ADD = 0x2, PICA_ADD_SIGNED = 0x3,
    PICA_INTERPOLATE = 0x4, PICA_SUBTRACT = 0x5, PICA_MULTIPLY_ADD = 0x8, PICA_ADD_MULTIPLY = 0x9,

    PICA_SCALE_1 = 0x0, PICA_SCALE_2 = 0x1, PICA_SCALE_4 = 0x2,

    PICA_NEVER = 0, PICA_ALWAYS = 1, PICA_EQUAL = 2, PICA_NOTEQUAL = 3, PICA_LESS = 4,
    PICA_LEQUAL = 5, PICA_GREATER = 6, PICA_GEQUAL = 7,

    PICA_BLEND_ADD = 0, PICA_BLEND_SUBTRACT = 1, PICA_BLEND_REVERSE_SUBTRACT = 2,

    PICA_ZERO = 0, PICA_ONE = 1, PICA_SRC_COLOR = 2, PICA_ONE_MINUS_SRC_COLOR = 3,
    PICA_DST_COLOR = 4, PICA_ONE_MINUS_DST_COLOR = 5, PICA_SRC_ALPHA = 6,
    PICA_ONE_MINUS_SRC_ALPHA = 7, PICA_DST_ALPHA = 8, PICA_ONE_MINUS_DST_ALPHA = 9,

    PICA_CULL_NONE = 0, PICA_CULL_FRONT_CCW = 1, PICA_CULL_BACK_CCW = 2,

    PICA_CLAMP_TO_EDGE = 0, PICA_REPEAT = 2, PICA_MIRRORED_REPEAT = 3,
    PICA_NEAREST = 0, PICA_LINEAR = 1,

    PICA_WRITE_COLOR = 0x0F, PICA_WRITE_DEPTH = 0x10,
};

#endif
