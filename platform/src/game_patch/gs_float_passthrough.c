/*
 * gs_float_passthrough.c - wrappers that pass float arguments on in registers.
 *
 * Like the projection setters (gs_gfx_projection.c): the decomp writes
 * these without arguments because on the GameCube the caller's values are
 * still in r3-r10 and f1-f8 when the call is made. Here they pass them.
 */
typedef float f32;
typedef unsigned int u32;

extern void C_QUATRotAxisRad(void* quat, const void* axis, f32 rad);
extern f32 fn_801C5F6C(u32 arg0, void* texture, f32 a2, f32 a3, f32 a4, f32 a5);

/* src/game/gs_math_quat.c: a quaternion from an axis and an angle
 * (lightning and leaf effects, src/game/effect_visual.c). */
void fn_800E0718(void* quat, const void* axis, f32 rad) { C_QUATRotAxisRad(quat, axis, rad); }

/* src/game/effect/fade_effect.c: a fade hook, called through the fade
 * table with fn_801C5F6C's arguments. */
f32 fadeEffectHookFunction_carde(u32 arg0, void* texture, f32 a2, f32 a3, f32 a4, f32 a5)
{
    return fn_801C5F6C(arg0, texture, a2, a3, a4, a5);
}
