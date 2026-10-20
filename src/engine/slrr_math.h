// Reimplementation of the exe's small math library (matrix/quaternion helpers).
// Layout: Mat34 = 12 floats, 3 rows of 4 (row stride 4, 4th column unused/translation-free).
// Quaternion = {x, y, z, w}. Every function documents the original address.
#pragma once
#include <math.h>

typedef struct { float x, y, z, w; } SlrrQuat;
typedef struct { float m[12]; } SlrrMat34;   // m[row*4 + col]

// 0x5665c0 FUN_005665c0: quaternion -> rotation matrix (writes the 9 used entries only)
static inline void slrr_quat_to_mat(SlrrMat34 *o, const SlrrQuat *q)
{
    float x = q->x, y = q->y, z = q->z, w = q->w;
    float z2 = z + z, y2 = y + y, x2xx = x * (x + x), w2x = w * (x + x);
    o->m[0]  = 1.0f - (z * z2 + y * y2);
    o->m[1]  = x * y2 - w * z2;
    o->m[4]  = w * z2 + x * y2;
    o->m[2]  = w * y2 + x * z2;
    o->m[5]  = 1.0f - (z * z2 + x2xx);
    o->m[10] = 1.0f - (y * y2 + x2xx);
    o->m[6]  = y * z2 - w2x;
    o->m[9]  = w2x + y * z2;
    o->m[8]  = x * z2 - w * y2;
}

// 0x5680d0 FUN_005680d0: normalize; zero quaternion becomes identity (0,0,0,1)
static inline void slrr_quat_normalize(SlrrQuat *q)
{
    float n = sqrtf(q->y * q->y + q->x * q->x + q->z * q->z + q->w * q->w);
    if (n == 0.0f) { q->x = q->y = q->z = 0.0f; q->w = 1.0f; return; }
    n = 1.0f / n;
    q->x *= n; q->y *= n; q->z *= n; q->w *= n;
}

// 0x567b40 FUN_00567b40: o = a * b (Hamilton product, the exe's 7-multiplication form)
static inline void slrr_quat_mul(SlrrQuat *o, const SlrrQuat *a, const SlrrQuat *b)
{
    float a0 = a->x, a1 = a->y, a2 = a->z, a3 = a->w;
    float b0 = b->x, b1 = b->y, b2 = b->z, b3 = b->w;
    float t13 = (b0 + b1) * (a0 + a2);
    float t14 = (b0 - b1) * (a0 - a2);
    float t15 = (b3 - b2) * (a3 + a1);
    float t12 = (t14 + t13) * 0.5f;
    float t11 = (b3 + b2) * (a3 - a1);
    float t10 = t11 * 0.5f;
    o->x = (b0 + b3) * (a0 + a3) - (t15 * 0.5f + t12 + t10);
    t13 = t13 - t14;
    o->y = ((t13 + t15) - t11) * 0.5f + (a3 - a0) * (b2 + b1);
    o->z = (t13 - t15) * 0.5f + t10 + (b3 - b0) * (a1 + a2);
    o->w = (t15 * 0.5f - t12) + t10 + (b1 - b2) * (a2 - a1);
}

// 0x567ea0 FUN_00567ea0: rotation matrix -> quaternion (largest-diagonal branch method)
static inline void slrr_mat_to_quat(SlrrQuat *o, const SlrrMat34 *mm)
{
    const float *p = mm->m;
    static const int nxt[3] = {1, 2, 0};
    float tr = p[0] + p[5] + p[10];
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f), k = 0.5f / s;
        o->w = s * 0.5f;
        o->x = (p[9] - p[6]) * k;
        o->y = (p[2] - p[8]) * k;
        o->z = (p[4] - p[1]) * k;
        return;
    }
    unsigned i = p[0] < p[5];
    if (p[i * 5] <= p[10] && p[10] != p[i * 5]) i = 2;
    int j = nxt[i], k2 = nxt[j];
    float s = sqrtf((p[i * 5] - (p[k2 * 5] + p[j * 5])) + 1.0f), q[4];
    q[i] = s * 0.5f;
    if (s != 0.0f) s = 0.5f / s;
    q[3] = (p[j + k2 * 4] - p[k2 + j * 4]) * s;
    q[j] = (p[i + j * 4] + p[j + i * 4]) * s;
    q[k2] = (p[i + k2 * 4] + p[k2 + i * 4]) * s;
    o->x = q[0]; o->y = q[1]; o->z = q[2]; o->w = q[3];
}

// 0x566c10 FUN_00566c10: o = a * b (3x3 part, row-vector convention as stored)
static inline void slrr_mat_mul(SlrrMat34 *o, const SlrrMat34 *a, const SlrrMat34 *b)
{
    const float *A = a->m, *B = b->m;
    o->m[0]  = A[0] * B[0] + B[4] * A[1] + A[2] * B[8];
    o->m[1]  = B[5] * A[1] + B[1] * A[0] + B[9] * A[2];
    o->m[2]  = B[6] * A[1] + B[2] * A[0] + B[10] * A[2];
    o->m[4]  = A[5] * B[4] + A[4] * B[0] + A[6] * B[8];
    o->m[5]  = A[5] * B[5] + A[4] * B[1] + A[6] * B[9];
    o->m[6]  = A[5] * B[6] + A[4] * B[2] + A[6] * B[10];
    o->m[8]  = B[4] * A[9] + B[0] * A[8] + A[10] * B[8];
    o->m[9]  = B[5] * A[9] + B[1] * A[8] + B[9] * A[10];
    o->m[10] = B[6] * A[9] + B[2] * A[8] + B[10] * A[10];
}

// 0x567380 FUN_00567380: transpose of the 3x3 part (in place if o == src)
static inline void slrr_mat_transpose(SlrrMat34 *o, const SlrrMat34 *s)
{
    if (o == s) {
        float t;
        t = o->m[4]; o->m[4] = o->m[1]; o->m[1] = t;
        t = o->m[8]; o->m[8] = o->m[2]; o->m[2] = t;
        t = o->m[9]; o->m[9] = o->m[6]; o->m[6] = t;
        return;
    }
    o->m[0] = s->m[0]; o->m[1] = s->m[4]; o->m[2]  = s->m[8];
    o->m[4] = s->m[1]; o->m[5] = s->m[5]; o->m[6]  = s->m[9];
    o->m[8] = s->m[2]; o->m[9] = s->m[6]; o->m[10] = s->m[10];
}
