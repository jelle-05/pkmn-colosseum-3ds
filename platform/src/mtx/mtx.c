/*
 * mtx.c - Dolphin SDK matrix and vector functions in plain C.
 *
 * The SDK implements most of these with paired-single assembly (the PS*
 * names), and the decomp keeps that assembly, so those units are not reused.
 * These follow the SDK's C reference versions (C_MTX*, C_VEC*), which define
 * the same results. Mtx is float[3][4], row-major, column vectors.
 */
#include <math.h>
#include <string.h>

typedef float Mtx[3][4];
typedef float Mtx44[4][4];
typedef struct { float x, y, z; } Vec;
typedef struct { float x, y, z, w; } Quaternion;

/* ---- matrices -------------------------------------------------------------- */

void PSMTXIdentity(Mtx m)
{
    memset(m, 0, sizeof(Mtx));
    m[0][0] = m[1][1] = m[2][2] = 1.0f;
}

void PSMTXCopy(const Mtx src, Mtx dst)
{
    if (src != dst)
        memmove(dst, src, sizeof(Mtx));
}

void PSMTXConcat(const Mtx a, const Mtx b, Mtx ab)
{
    Mtx t;
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 4; c++)
            t[r][c] = a[r][0] * b[0][c] + a[r][1] * b[1][c] + a[r][2] * b[2][c];
        t[r][3] += a[r][3];
    }
    memcpy(ab, t, sizeof(Mtx));
}

void PSMTXTranspose(const Mtx src, Mtx xpose)
{
    Mtx t;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++)
            t[r][c] = src[c][r];
    t[0][3] = t[1][3] = t[2][3] = 0.0f;
    memcpy(xpose, t, sizeof(Mtx));
}

static float det3(const Mtx m)
{
    return m[0][0] * m[1][1] * m[2][2] + m[0][1] * m[1][2] * m[2][0] + m[0][2] * m[1][0] * m[2][1]
         - m[2][0] * m[1][1] * m[0][2] - m[1][0] * m[0][1] * m[2][2] - m[0][0] * m[2][1] * m[1][2];
}

unsigned long PSMTXInverse(const Mtx src, Mtx inv)
{
    float det = det3(src);
    if (det == 0.0f)
        return 0;
    det = 1.0f / det;
    Mtx t;
    t[0][0] = (src[1][1] * src[2][2] - src[2][1] * src[1][2]) * det;
    t[0][1] = -(src[0][1] * src[2][2] - src[2][1] * src[0][2]) * det;
    t[0][2] = (src[0][1] * src[1][2] - src[1][1] * src[0][2]) * det;
    t[1][0] = -(src[1][0] * src[2][2] - src[2][0] * src[1][2]) * det;
    t[1][1] = (src[0][0] * src[2][2] - src[2][0] * src[0][2]) * det;
    t[1][2] = -(src[0][0] * src[1][2] - src[1][0] * src[0][2]) * det;
    t[2][0] = (src[1][0] * src[2][1] - src[2][0] * src[1][1]) * det;
    t[2][1] = -(src[0][0] * src[2][1] - src[2][0] * src[0][1]) * det;
    t[2][2] = (src[0][0] * src[1][1] - src[1][0] * src[0][1]) * det;
    for (int r = 0; r < 3; r++)
        t[r][3] = -(t[r][0] * src[0][3] + t[r][1] * src[1][3] + t[r][2] * src[2][3]);
    memcpy(inv, t, sizeof(Mtx));
    return 1;
}

/* Inverse transpose of the 3x3 part, for normals; translation is zero. */
unsigned long PSMTXInvXpose(const Mtx src, Mtx invx)
{
    float det = det3(src);
    if (det == 0.0f)
        return 0;
    det = 1.0f / det;
    Mtx t;
    t[0][0] = (src[1][1] * src[2][2] - src[2][1] * src[1][2]) * det;
    t[0][1] = -(src[1][0] * src[2][2] - src[2][0] * src[1][2]) * det;
    t[0][2] = (src[1][0] * src[2][1] - src[2][0] * src[1][1]) * det;
    t[1][0] = -(src[0][1] * src[2][2] - src[2][1] * src[0][2]) * det;
    t[1][1] = (src[0][0] * src[2][2] - src[2][0] * src[0][2]) * det;
    t[1][2] = -(src[0][0] * src[2][1] - src[2][0] * src[0][1]) * det;
    t[2][0] = (src[0][1] * src[1][2] - src[1][1] * src[0][2]) * det;
    t[2][1] = -(src[0][0] * src[1][2] - src[1][0] * src[0][2]) * det;
    t[2][2] = (src[0][0] * src[1][1] - src[1][0] * src[0][1]) * det;
    t[0][3] = t[1][3] = t[2][3] = 0.0f;
    memcpy(invx, t, sizeof(Mtx));
    return 1;
}

void PSMTXRotTrig(Mtx m, char axis, float s, float c)
{
    PSMTXIdentity(m);
    switch (axis) {
    case 'x': case 'X':
        m[1][1] = c; m[1][2] = -s; m[2][1] = s; m[2][2] = c;
        break;
    case 'y': case 'Y':
        m[0][0] = c; m[0][2] = s; m[2][0] = -s; m[2][2] = c;
        break;
    case 'z': case 'Z':
        m[0][0] = c; m[0][1] = -s; m[1][0] = s; m[1][1] = c;
        break;
    }
}

void PSMTXRotRad(Mtx m, char axis, float rad)
{
    PSMTXRotTrig(m, axis, sinf(rad), cosf(rad));
}

void PSMTXRotAxisRad(Mtx m, const Vec* axis, float rad)
{
    float s = sinf(rad), c = cosf(rad), t = 1.0f - c;
    float x = axis->x, y = axis->y, z = axis->z;
    float len = sqrtf(x * x + y * y + z * z);
    if (len != 0.0f) {
        x /= len; y /= len; z /= len;
    }
    m[0][0] = t * x * x + c;     m[0][1] = t * x * y - s * z; m[0][2] = t * x * z + s * y; m[0][3] = 0.0f;
    m[1][0] = t * x * y + s * z; m[1][1] = t * y * y + c;     m[1][2] = t * y * z - s * x; m[1][3] = 0.0f;
    m[2][0] = t * x * z - s * y; m[2][1] = t * y * z + s * x; m[2][2] = t * z * z + c;     m[2][3] = 0.0f;
}

void PSMTXTrans(Mtx m, float x, float y, float z)
{
    PSMTXIdentity(m);
    m[0][3] = x; m[1][3] = y; m[2][3] = z;
}

void PSMTXTransApply(const Mtx src, Mtx dst, float x, float y, float z)
{
    if (src != dst)
        memmove(dst, src, sizeof(Mtx));
    dst[0][3] = src[0][3] + x;
    dst[1][3] = src[1][3] + y;
    dst[2][3] = src[2][3] + z;
}

void PSMTXScale(Mtx m, float x, float y, float z)
{
    memset(m, 0, sizeof(Mtx));
    m[0][0] = x; m[1][1] = y; m[2][2] = z;
}

void PSMTXQuat(Mtx m, const Quaternion* q)
{
    float s = 2.0f / (q->x * q->x + q->y * q->y + q->z * q->z + q->w * q->w);
    float xs = q->x * s, ys = q->y * s, zs = q->z * s;
    float wx = q->w * xs, wy = q->w * ys, wz = q->w * zs;
    float xx = q->x * xs, xy = q->x * ys, xz = q->x * zs;
    float yy = q->y * ys, yz = q->y * zs, zz = q->z * zs;
    m[0][0] = 1.0f - (yy + zz); m[0][1] = xy - wz;          m[0][2] = xz + wy;          m[0][3] = 0.0f;
    m[1][0] = xy + wz;          m[1][1] = 1.0f - (xx + zz); m[1][2] = yz - wx;          m[1][3] = 0.0f;
    m[2][0] = xz - wy;          m[2][1] = yz + wx;          m[2][2] = 1.0f - (xx + yy); m[2][3] = 0.0f;
}

void PSMTXMultVec(const Mtx m, const Vec* src, Vec* dst)
{
    Vec v = *src;
    dst->x = m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z + m[0][3];
    dst->y = m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z + m[1][3];
    dst->z = m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z + m[2][3];
}

/* Scale and rotation only: no translation. */
void PSMTXMultVecSR(const Mtx m, const Vec* src, Vec* dst)
{
    Vec v = *src;
    dst->x = m[0][0] * v.x + m[0][1] * v.y + m[0][2] * v.z;
    dst->y = m[1][0] * v.x + m[1][1] * v.y + m[1][2] * v.z;
    dst->z = m[2][0] * v.x + m[2][1] * v.y + m[2][2] * v.z;
}

/* ---- vectors ------------------------------------------------------------------ */

void PSVECAdd(const Vec* a, const Vec* b, Vec* ab)
{
    ab->x = a->x + b->x; ab->y = a->y + b->y; ab->z = a->z + b->z;
}

void PSVECSubtract(const Vec* a, const Vec* b, Vec* a_b)
{
    a_b->x = a->x - b->x; a_b->y = a->y - b->y; a_b->z = a->z - b->z;
}

void PSVECScale(const Vec* src, Vec* dst, float scale)
{
    dst->x = src->x * scale; dst->y = src->y * scale; dst->z = src->z * scale;
}

float PSVECDotProduct(const Vec* a, const Vec* b) { return a->x * b->x + a->y * b->y + a->z * b->z; }
float PSVECSquareMag(const Vec* v) { return PSVECDotProduct(v, v); }
float PSVECMag(const Vec* v) { return sqrtf(PSVECSquareMag(v)); }

void PSVECNormalize(const Vec* src, Vec* unit)
{
    float m = 1.0f / sqrtf(PSVECSquareMag(src));
    unit->x = src->x * m; unit->y = src->y * m; unit->z = src->z * m;
}

void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* axb)
{
    Vec t;
    t.x = a->y * b->z - a->z * b->y;
    t.y = a->z * b->x - a->x * b->z;
    t.z = a->x * b->y - a->y * b->x;
    *axb = t;
}

float PSVECSquareDistance(const Vec* a, const Vec* b)
{
    Vec d;
    PSVECSubtract(a, b, &d);
    return PSVECSquareMag(&d);
}

float PSVECDistance(const Vec* a, const Vec* b) { return sqrtf(PSVECSquareDistance(a, b)); }

/* ---- view and light projection ---------------------------------------------- */

void C_MTXLookAt(Mtx m, const Vec* cam, const Vec* up, const Vec* target)
{
    Vec look, right, up2;
    PSVECSubtract(cam, target, &look);
    PSVECNormalize(&look, &look);
    PSVECCrossProduct(up, &look, &right);
    PSVECNormalize(&right, &right);
    PSVECCrossProduct(&look, &right, &up2);
    m[0][0] = right.x; m[0][1] = right.y; m[0][2] = right.z; m[0][3] = -PSVECDotProduct(cam, &right);
    m[1][0] = up2.x;   m[1][1] = up2.y;   m[1][2] = up2.z;   m[1][3] = -PSVECDotProduct(cam, &up2);
    m[2][0] = look.x;  m[2][1] = look.y;  m[2][2] = look.z;  m[2][3] = -PSVECDotProduct(cam, &look);
}

void C_MTXLightFrustum(Mtx m, float t, float b, float l, float r, float n,
                       float scaleS, float scaleT, float transS, float transT)
{
    float tmp = 1.0f / (r - l);
    m[0][0] = (2.0f * n * tmp) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = ((r + l) * tmp) * scaleS - transS;
    m[0][3] = 0.0f;
    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = (2.0f * n * tmp) * scaleT;
    m[1][2] = ((t + b) * tmp) * scaleT - transT;
    m[1][3] = 0.0f;
    m[2][0] = m[2][1] = 0.0f;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void C_MTXLightPerspective(Mtx m, float fovY, float aspect, float scaleS, float scaleT,
                           float transS, float transT)
{
    float angle = fovY * 0.5f * 3.14159265358979323846f / 180.0f;
    float cot = 1.0f / tanf(angle);
    m[0][0] = (cot / aspect) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = -transS;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot * scaleT;
    m[1][2] = -transT;
    m[1][3] = 0.0f;
    m[2][0] = m[2][1] = 0.0f;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}
