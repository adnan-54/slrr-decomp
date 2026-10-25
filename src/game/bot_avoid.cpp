#include "game/bot_avoid.h"

#include <math.h>

static const float kPi = 3.1415927f, k2Pi = 6.2831855f;  // 0x5935a4/0x5935a8

BotAvoid &BotAvoid::shared()
{
    static BotAvoid s;  // DAT_005fe1f4
    return s;
}

// 0x40a7d0 angle(e, p)
static float angle2(const float e[2], float px, float pz)
{
    float g = px * e[0] + pz * e[1], cr = e[0] * pz - px * e[1];
    if (cr <= 0.0f) return g > 1.0f ? -0.0f : g < -1.0f ? -kPi : -acosf(g);
    return g > 1.0f ? 0.0f : g < -1.0f ? kPi : acosf(g);
}

// 0x40ab10
void BotAvoid::reset(float fx, float fz, float vx, float vz, float mg, float minRange)
{
    circles.clear();
    intervals.clear();
    F[0] = fx; F[1] = fz;
    dir[0] = vx; dir[1] = vz;
    margin = mg;
    len = sqrtf(vz * vz + vx * vx);
    range = len;
    if (len < minRange) range = minRange;
    if (len > 0.0f) { float k = 1.0f / len; dir[0] *= k; dir[1] *= k; }
}

// 0x40a6f0
void BotAvoid::enclose(Interval &I, float px, float pz, float r)
{
    float d = sqrtf((I.cx - px) * (I.cx - px) + (I.cz - pz) * (I.cz - pz));
    if (d >= 0.001f) {
        float e = (d + r) - I.R;
        if (e > 0.0f) {
            e *= 0.5f;
            float t = e / d;
            I.R += e;
            I.cx = px * t + I.cx * (1.0f - t);
            I.cz = I.cz * (1.0f - t) + pz * t;
        }
    } else if (r > I.R) I.R = r;
    if (r > I.maxr) I.maxr = r;
}

// inlined unite() in 0x409fa0
static void unite(BotAvoid::Interval &I, float aa, float bb)
{
    float x0 = I.a;
    if (x0 > aa && bb >= x0) I.a = aa;
    else if (x0 + k2Pi > aa && bb >= x0 + k2Pi) { I.b += k2Pi; I.a = aa; }
    float y = I.b;
    if (bb > y && y >= aa) I.b = bb;
    else if (bb + k2Pi > y && y >= aa + k2Pi) I.b = bb + k2Pi;
}

// 0x409fa0
void BotAvoid::add(float x, float z, float vx, float vz, float r)
{
    int ci = (int)circles.size();
    circles.emplace_back();
    {
        Circle &c = circles[ci];
        c.x = x; c.z = z; c.vx = vx; c.vz = vz;
        c.r = (margin + r) * 1.05f;
        c.dist = sqrtf((F[0] - x) * (F[0] - x) + (F[1] - z) * (F[1] - z));
        float ux = x - F[0], uz = z - F[1];
        float l = sqrtf(ux * ux + uz * uz);
        if ((double)l > 1e-5) { float il = 1.0f / l; ux *= il; uz *= il; } else { ux = 0; uz = 0; }  // 0x593588
        c.ang = angle2(dir, ux, uz);
    }
    float gap = circles[ci].dist - circles[ci].r;
    if (!(gap > 0.0f)) {
        circles[ci].r = circles[ci].dist * 0.95f;
        gap = circles[ci].dist - circles[ci].r;
        if (!(gap > 0.0f)) { circles.pop_back(); return; }
    }
    if (gap > range) { circles.pop_back(); return; }
    float cr = circles[ci].r, cdist = circles[ci].dist;
    float h = asinf(cr / cdist);
    float a = circles[ci].ang - h, b = circles[ci].ang + h;
    int merged = -1;
    for (int k = 0; k < (int)intervals.size(); k++) {
        Interval I = intervals[k];
        float q = I.maxr * 3.0f + cr * 3.0f + I.R - I.maxr;
        if (!((I.cx - x) * (I.cx - x) + (I.cz - z) * (I.cz - z) < q * q)) continue;
        bool overlap = false;
        for (int j = I.head; j != -1; j = circles[j].link) {
            const Circle m = circles[j];
            float s = m.r + cr;
            float is = 1.0f / s;  // 0x409fa0 multiplies by a precomputed reciprocal
            float Px = m.x * (cr * is) + x * (m.r * is) - F[0];
            float Pz = m.z * (cr * is) + z * (m.r * is) - F[1];
            float L = sqrtf(Px * Px + Pz * Pz);
            if (L > 0.0f) { float iL = 1.0f / L; Px *= iL; Pz *= iL; }
            float Dx = x - m.x, Dz = z - m.z, d = sqrtf(Dx * Dx + Dz * Dz);
            float thr;
            if (2.0f * L > 3.0f * s && d > s) {
                float w = (Dz * Pz + Dx * Px) / d;
                float t = 1.0f - w * w;
                thr = (t > 0.0f) ? s * (3.0f / (2.0f * sqrtf(t) + 1.0f)) : 3.0f * s;
            } else thr = s;
            if (thr >= d) {
                if (merged == -1) {
                    float aa = a, bb = b;
                    while (aa < 0) { aa += k2Pi; bb += k2Pi; }
                    unite(intervals[k], aa, bb);
                    if (gap < intervals[k].gap) intervals[k].gap = gap;
                    enclose(intervals[k], x, z, cr);
                    circles[ci].link = m.link;
                    circles[j].link = ci;
                    merged = k;
                } else {
                    Interval &M = intervals[merged];
                    float aa = a, bb = b;
                    while (aa < 0) { aa += k2Pi; bb += k2Pi; }
                    unite(M, aa, bb);
                    aa = I.a; bb = I.b;
                    while (aa < 0) { aa += k2Pi; bb += k2Pi; }
                    unite(M, aa, bb);
                    if (gap < M.gap) M.gap = gap;
                    if (I.gap < M.gap) M.gap = I.gap;
                    enclose(M, I.cx, I.cz, I.R);
                    int t = ci;
                    while (circles[t].link != -1) t = circles[t].link;
                    circles[t].link = I.head;
                    intervals[k] = intervals.back();
                    intervals.pop_back();
                    if (k < (int)intervals.size()) k--;
                }
                overlap = true;
                break;
            }
        }
        (void)overlap;
    }
    if (merged != -1) return;
    while (a < 0) { a += k2Pi; b += k2Pi; }
    Interval n{ci, a, b, gap, x, z, cr, cr};
    intervals.push_back(n);
    circles[ci].link = -1;
}

// 0x46b5e0
void BotAvoid::heap_push(HeapEnt e)
{
    size_t n = heap.size();
    heap.push_back(e);
    while (n != 0) {
        size_t p = (n - 1) / 2;
        if (heap[p].key <= e.key) break;
        heap[n] = heap[p];
        n = p;
    }
    heap[n] = e;
}

// 0x40abe0
void BotAvoid::heap_pop()
{
    size_t size = heap.size() - 1;
    if (size == 0) { heap.pop_back(); return; }
    HeapEnt last = heap[size];
    size_t i = 0, c = 1;
    while (c < size) {
        size_t r = 2 * i + 2;
        if (r < size && !(heap[c].key <= heap[r].key)) {
            if (last.key <= heap[r].key) break;
            heap[i] = heap[r]; i = r;
        } else {
            if (last.key <= heap[c].key) break;
            heap[i] = heap[c]; i = c;
        }
        c = 2 * i + 1;
    }
    heap[i] = last;
    heap.pop_back();
}

// 0x40a8a0
void BotAvoid::resolve(float out[2])
{
    heap.clear();
    for (int i = 0; i < (int)intervals.size(); i++) heap_push({i, intervals[i].gap});
    float lo = 0, loLim = -kPi, hi = 0, hiLim = kPi;
    while (!heap.empty()) {
        int i = heap[0].index;
        heap_pop();
        float a = intervals[i].a, b = intervals[i].b;
        while (a < -k2Pi) { a += k2Pi; b += k2Pi; }
        while (a > 0) { a -= k2Pi; b -= k2Pi; }
        if (lo > a && b >= lo) {
            if (a > loLim || lo == 0) lo = a;
            else if (lo > loLim) lo = loLim;
        }
        if (lo > b) loLim = b > loLim ? b : loLim;
        while (b > k2Pi) { b -= k2Pi; a -= k2Pi; }
        while (b < 0) { b += k2Pi; a += k2Pi; }
        if (b > hi && hi >= a) {
            if (hiLim > b || hi == 0) hi = b;
            else if (hiLim > hi) hi = hiLim;
        }
        if (a > hi) hiLim = a < hiLim ? a : hiLim;
    }
    float ang = (fabsf(hi) > fabsf(lo)) ? lo : hi;
    float c = cosf(ang), s = sinf(ang), dx = dir[0], dz = dir[1];
    dir[0] = c * dx - s * dz;
    dir[1] = s * dx + c * dz;
    out[0] = len * dir[0];
    out[1] = len * dir[1];
}
