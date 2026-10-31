#include "game/traffic.h"
#include "game/pedestrians.h"

#include <algorithm>
#include <math.h>
#include <string.h>

// 0x571110 MSVC CRT rand, shared with java.lang.Math.random (natives.cpp).
uint32_t msvc_rand();

namespace {

struct Reader {
    const uint8_t *data;
    size_t size;
    size_t off = 0;
    std::string *err;
    std::string ctx;   // record kind + index for truncation errors

    bool need(size_t n)
    {
        if (off + n <= size) return true;
        if (err) *err = "truncated in " + ctx + " at byte " + std::to_string(off);
        return false;
    }
    bool u8(uint8_t *v)
    {
        if (!need(1)) return false;
        *v = data[off];
        off += 1;
        return true;
    }
    bool u16(uint16_t *v)
    {
        if (!need(2)) return false;
        *v = (uint16_t)((uint16_t)data[off] | ((uint16_t)data[off + 1] << 8));
        off += 2;
        return true;
    }
    bool u32(uint32_t *v)
    {
        if (!need(4)) return false;
        *v = (uint32_t)data[off] | ((uint32_t)data[off + 1] << 8) |
             ((uint32_t)data[off + 2] << 16) | ((uint32_t)data[off + 3] << 24);
        off += 4;
        return true;
    }
    bool f32(float *v)
    {
        uint32_t bits;
        if (!u32(&bits)) return false;
        memcpy(v, &bits, sizeof *v);
        return true;
    }
};

void set_ctx(Reader &r, const char *kind, size_t i)
{
    r.ctx = std::string(kind) + " " + std::to_string(i);
}

// 0x414d50 ctSegment__load — 57 file bytes.
bool read_segment(Reader &r, traffic::Segment &s)
{
    for (int i = 0; i < 3; i++)
        if (!r.f32(&s.p0[i])) return false;
    for (int i = 0; i < 3; i++)
        if (!r.f32(&s.p1[i])) return false;
    for (int i = 0; i < 3; i++)
        if (!r.f32(&s.m0[i])) return false;
    for (int i = 0; i < 3; i++)
        if (!r.f32(&s.m1[i])) return false;
    if (!r.f32(&s.len)) return false;
    if (!r.f32(&s.flat_len)) return false;
    uint8_t curved;
    if (!r.u8(&curved)) return false;
    s.curved = curved != 0;
    s.k40 = 1.0f;
    return true;
}

void normalize(float v[3])
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l != 0.0f) {
        float inv = 1.0f / l;
        v[0] *= inv;
        v[1] *= inv;
        v[2] *= inv;
    }
}

// 0x4124d0 ctSegment__arcLength: 101 Hermite samples at t = 0, 0.01 .. 1.00,
// chord sum in two float accumulators. segment_pos maps d to t = d/len, so the
// probe runs with len = 1.
void segment_arc_length(traffic::Segment &s)
{
    s.len = 1.0f;
    float prev[3];
    traffic::segment_pos(s, 0.0f, prev);
    float sum1 = 0.0f, sum2 = 0.0f;
    for (int i = 1; i <= 100; i++) {
        float p[3];
        traffic::segment_pos(s, (float)i * 0.01f, p);
        float dx = p[0] - prev[0], dy = p[1] - prev[1], dz = p[2] - prev[2];
        float chord = sqrtf(dx * dx + dy * dy + dz * dz);
        if (i <= 50)
            sum1 = sum1 + chord;
        else
            sum2 = sum2 + chord;
        prev[0] = p[0];
        prev[1] = p[1];
        prev[2] = p[2];
    }
    s.len = sum2 + sum1;
}

// 0x414b80 ctPath__load — 0x3c-byte Path.
bool read_path(Reader &r, traffic::Path &p)
{
    uint32_t n;
    if (!r.u32(&n)) return false;
    p.segs.resize(n);
    const std::string base = r.ctx;
    for (size_t i = 0; i < p.segs.size(); i++) {
        r.ctx = base + " segment " + std::to_string(i);
        if (!read_segment(r, p.segs[i])) return false;
        p.length = p.segs[i].len + p.length;
    }
    r.ctx = base;
    // The exe reads segs[0] and segs[n-1] even when n == 0.
    // INVENTED: leave start/end/dirs zero for an empty path.
    if (!p.segs.empty()) {
        const traffic::Segment &first = p.segs.front();
        const traffic::Segment &last = p.segs.back();
        for (int i = 0; i < 3; i++) {
            p.start[i] = first.p0[i];
            p.end[i] = last.p1[i];
            p.start_dir[i] = first.m0[i];
            p.end_dir[i] = last.curved ? last.m1[i] : -last.m0[i];
        }
        normalize(p.start_dir);
        normalize(p.end_dir);
    }
    return true;
}

// 0x40e850 ctCross__load.
bool read_cross(Reader &r, traffic::Cross &c)
{
    uint32_t n;
    if (!r.u32(&n)) return false;
    c.links.resize(n);
    for (size_t i = 0; i < c.links.size(); i++)
        if (!r.u32(&c.links[i].node) || !r.u8(&c.links[i].flag)) return false;

    if (!r.u32(&n)) return false;
    c.paths.resize(n);
    const std::string base = r.ctx;
    for (size_t i = 0; i < c.paths.size(); i++) {
        traffic::CrossPath &p = c.paths[i];
        r.ctx = base + " path " + std::to_string(i);
        if (!r.u8(&p.a) || !r.u8(&p.b) || !r.u16(&p.c) || !r.u32(&p.k58) ||
            !r.u32(&p.k5c))
            return false;
        if (!read_segment(r, p.seg)) return false;
    }
    r.ctx = base;

    for (int i = 0; i < 3; i++)
        if (!r.f32(&c.pos[i])) return false;
    return r.u32(&c.k30);
}

// 0x40eb20 ctRoad__load.
bool read_road(Reader &r, traffic::Road &road)
{
    for (int i = 0; i < 3; i++)
        if (!r.u32(&road.node[i])) return false;
    for (int i = 0; i < 3; i++)
        if (!r.f32(&road.v10[i])) return false;
    for (int i = 0; i < 3; i++)
        if (!r.f32(&road.v1c[i])) return false;
    // k64 is compared signed (CMP/JLE 0x417f2c, CMP/JG 0x4183b3).
    uint32_t k64;
    if (!r.u32(&k64)) return false;
    road.k64 = (int32_t)k64;
    return read_path(r, road.path);
}

// 0x40e9b0 ctLane__load.
bool read_lane(Reader &r, traffic::Lane &lane)
{
    if (!r.u32(&lane.road) || !r.u32(&lane.node14) || !r.u8(&lane.k18)) return false;
    for (int i = 0; i < 3; i++)
        if (!r.f32(&lane.v1c[i])) return false;
    for (int i = 0; i < 3; i++)
        if (!r.f32(&lane.v28[i])) return false;
    if (!r.u32(&lane.k34) || !r.u32(&lane.k38)) return false;
    if (!r.f32(&lane.index_min_x) || !r.f32(&lane.index_run_max_x) ||
        !r.f32(&lane.index_max_x) || !r.u32(&lane.index_lane))
        return false;
    for (int i = 0; i < 4; i++)
        if (!r.u32(&lane.node88[i])) return false;
    if (!r.f32(&lane.k98) || !r.f32(&lane.k9c) || !r.u32(&lane.ka0)) return false;
    return read_path(r, lane.path);
}

}  // namespace

namespace traffic {

// 0x413360/0x412ed0 and the segment getters share this walk.
size_t path_seg_at(const Path &p, float *d, float *local)
{
    if (*d < 0) {
        float L = p.length;
        if (L != 0) *d = *d - (float)(int)(*d / L) * L;
        *d = L + *d;
    }
    size_t i = 0;
    while (i + 1 < p.segs.size()) {
        if (*d < p.segs[i].len) break;
        *d = *d - p.segs[i].len;
        i++;
    }
    if (local) *local = *d;
    return i;
}

// 0x4134e0 ctSegment__pos.
void segment_pos(const Segment &s, float d, float out[3])
{
    if (!s.curved) {
        for (int i = 0; i < 3; i++)
            out[i] = s.m0[i] * d + s.p0[i];
        return;
    }
    float t = d / s.len, t2 = t * t, t3 = t2 * t;
    float h00 = (t3 + t3) - t2 * 3 + 1;
    float h01 = t2 * 3 - (t3 + t3);
    float h10 = (t3 - (t2 + t2)) + t;
    float h11 = t3 - t2;
    for (int i = 0; i < 3; i++)
        out[i] = s.m1[i] * h11 + s.m0[i] * h10 + s.p1[i] * h01 + s.p0[i] * h00;
}

// 0x412f60 ctSegment__dir.
void segment_dir(const Segment &s, float d, float out[3])
{
    if (!s.curved) {
        for (int i = 0; i < 3; i++)
            out[i] = s.m0[i];
        return;
    }
    float t = d / s.len, t2 = t * t;
    float a = (t2 - t) * 6, b = (t - t2) * 6;
    float c = (t2 * 3 - t * 4) + 1, e = t2 * 3 - (t + t);
    float v[3];
    for (int i = 0; i < 3; i++)
        v[i] = s.m1[i] * e + s.m0[i] * c + s.p1[i] * b + s.p0[i] * a;
    float l = sqrtf(v[1] * v[1] + v[0] * v[0] + v[2] * v[2]);
    if (l != 0.0f) {
        float inv = 1.0f / l;
        for (int i = 0; i < 3; i++)
            v[i] *= inv;
    }
    for (int i = 0; i < 3; i++)
        out[i] = v[i];
}

// 0x4131a0 ctSegment__side.
void segment_side(const Segment &s, float d, float out[3])
{
    if (!s.curved) {
        for (int i = 0; i < 3; i++)
            out[i] = s.m1[i];
        return;
    }
    float t = d / s.len, t2 = t * t;
    float a = (t2 - t) * 6, b = (t - t2) * 6;
    float c = (t2 * 3 - t * 4) + 1, e = t2 * 3 - (t + t);
    float dx = s.m1[0] * e + c * s.m0[0] + s.p1[0] * b + s.p0[0] * a;
    float nz = -(s.m1[2] * e + s.m0[2] * c + s.p1[2] * b + s.p0[2] * a);
    out[0] = nz;
    out[1] = 0.0f;
    out[2] = dx;
    float l = sqrtf(dx * dx + nz * nz);
    if (l != 0.0f) {
        float inv = 1.0f / l;
        out[0] = inv * nz;
        out[1] = inv * 0.0f;
        out[2] = inv * dx;
    }
}

// 0x413660 ctSegment__posOffset (ECX = Segment): pos at d shifted by off.
void segment_pos_offset(const Segment &s, float d, float off, float out[3])
{
    segment_pos(s, d, out);
    float side[3];
    segment_side(s, d, side);
    for (int i = 0; i < 3; i++)
        out[i] = side[i] * off + out[i];
}

// 0x413360 ctPath__posAt.
void path_pos_at(const Path &p, float d, float out[3])
{
    // INVENTED: the exe reads segs[0] out of bounds when n == 0.
    if (p.segs.empty()) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    size_t i = path_seg_at(p, &d, nullptr);
    segment_pos(p.segs[i], d, out);
}

// 0x412ed0 ctPath__dirAt.
void path_dir_at(const Path &p, float d, float out[3])
{
    // INVENTED: the exe reads segs[0] out of bounds when n == 0.
    if (p.segs.empty()) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    size_t i = path_seg_at(p, &d, nullptr);
    segment_dir(p.segs[i], d, out);
}

// Curved branch shared by 0x414480 and 0x413b00: Newton solve of the foot point.
// `dir` is the already normalized m0 xz span; d and the last pos/side come back.
static bool segment_curved_project(const Segment &s, float x, float z, const float dir[2],
                                   float *d, float pos[3], float side[3])
{
    float nrm[2] = {-s.m1[0], -s.m1[2]};
    float n = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1]);
    if ((double)n > 1e-5) {
        float inv = 1.0f / n;
        nrm[0] *= inv;
        nrm[1] *= inv;
    }
    float a = (z - s.p0[2]) * dir[1] + (x - s.p0[0]) * dir[0];
    float b = (z - s.p1[2]) * nrm[1] + (x - s.p1[0]) * nrm[0];
    // !(x >= 0) also catches NaN, matching the exe's carry-flag test.
    if (!(a >= 0.0f) || !(b >= 0.0f)) return false;
    *d = s.len * a / (b + a);
    float prev_err = 0.0f, k = 1.0f;
    float d3[3];
    segment_pos(s, *d, pos);
    segment_dir(s, *d, d3);
    float err = (x - pos[0]) * d3[0] + (z - pos[2]) * d3[2];
    while (!((double)fabsf(err) < 0.01)) {
        if (0.0f > err * prev_err) {
            k = k * 0.9f;
            if ((double)k < 0.1) break;
        }
        float old = *d;
        prev_err = err;
        *d = err * k + old;
        if (*d < 0) {
            *d = 0;
            if (old == 0) break;
        }
        if (*d > s.len) {
            *d = s.len;
            if (old == s.len) break;
        }
        segment_pos(s, *d, pos);
        segment_dir(s, *d, d3);
        err = (x - pos[0]) * d3[0] + (z - pos[2]) * d3[2];
    }
    segment_side(s, *d, side);
    return true;
}

// 0x414480 ctSegment__project — xz projection; d in [0, len], lat = signed side.
bool segment_project(const Segment &s, float x, float z, float *d, float *lat)
{
    float dir[2] = {s.m0[0], s.m0[2]};
    float n = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
    if ((double)n > 1e-5) {
        float inv = 1.0f / n;
        dir[0] *= inv;
        dir[1] *= inv;
    }
    if (!s.curved) {
        float dx = x - s.p0[0], dz = z - s.p0[2];
        float proj = dir[1] * dz + dir[0] * dx;
        if (proj < 0.0f) return false;
        if (s.flat_len < proj) return false;
        *d = proj / s.flat_len * s.len;
        *lat = s.m1[2] * dz + s.m1[0] * dx;
        return true;
    }
    float pos[3], side[3];
    if (!segment_curved_project(s, x, z, dir, d, pos, side)) return false;
    *lat = (x - pos[0]) * side[0] + (z - pos[2]) * side[2];   // pos from the last evaluation
    return true;
}

// 0x413b00 ctSegment__heightAt — road-surface y and |lateral| at (x, z).
bool segment_height(const Segment &s, float x, float z, float *y, float *lat)
{
    float dir[2] = {s.m0[0], s.m0[2]};
    if (!s.curved) {
        // 0x413de6: no 1e-5 guard on this normalization.
        float n = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
        float inv = 1.0f / n;
        float dx = dir[0] * inv, dz = dir[1] * inv;
        float t = dz * (z - s.p0[2]) + dx * (x - s.p0[0]);
        float L = s.len * n;                      // len (+0x30), not flat_len
        // !(x >= 0) also catches NaN, matching the exe's carry-flag test.
        if (!(t >= 0.0f) || !(L >= t)) return false;
        t = t / L;
        *y = (1.0f - t) * s.p0[1] + s.p1[1] * t;
        *lat = fabsf(dx * (z - s.p0[2]) + (-dz) * (x - s.p0[0]));
        return true;
    }
    float n = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
    if ((double)n > 1e-5) {
        float inv = 1.0f / n;
        dir[0] *= inv;
        dir[1] *= inv;
    }
    float d, pos[3], side[3];
    if (!segment_curved_project(s, x, z, dir, &d, pos, side)) return false;
    *lat = (x - pos[0]) * side[0] + (z - pos[2]) * side[2];   // signed
    *y = pos[1];
    return true;
}

// 0x4136e0 ctPath__heightAt — best segment by |lat|; no endpoint fallback.
bool path_height(const Path &p, float x, float z, float *y, float *lat)
{
    int best = -1;
    for (size_t i = 0; i < p.segs.size(); i++) {
        float yi = 0, lati = 0;
        if (segment_height(p.segs[i], x, z, &yi, &lati) &&
            (best == -1 || fabsf(lati) < fabsf(*lat))) {
            *lat = lati;
            *y = yi;
            best = (int)i;
        }
    }
    return best != -1;
}

// 0x414830 ctPath__project — nearest segment by |lat|; d is path length so far + sd.
bool path_project(const Path &p, float x, float z, float *d, float *lat)
{
    int best = -1;
    float acc = 0;
    for (size_t i = 0; i < p.segs.size(); i++) {
        const Segment &s = p.segs[i];
        float sd = 0, sl = 0;
        if (!segment_project(s, x, z, &sd, &sl)) {
            float d0 = (s.p0[2] - z) * (s.p0[2] - z) + (s.p0[0] - x) * (s.p0[0] - x);
            float d1 = (s.p1[2] - z) * (s.p1[2] - z) + (s.p1[0] - x) * (s.p1[0] - x);
            if (d1 <= d0) {
                sl = sqrtf(d1);
                sd = s.len;
            } else {
                sl = sqrtf(d0);
                sd = 0;
            }
        }
        if (best == -1 || fabsf(sl) < fabsf(*lat)) {
            *lat = sl;
            *d = sd + acc;
            best = (int)i;
        }
        acc = s.len + acc;
    }
    return best != -1;
}

// 0x414fe0 ctSegment__buildHermite + 0x4124d0 arc length iteration.
void segment_build_hermite(Segment &s, const float p0[3], const float d0[3],
                           const float p1[3], const float d1[3])
{
    for (int i = 0; i < 3; i++) {
        s.p0[i] = p0[i];
        s.p1[i] = p1[i];
        s.m0[i] = d0[i];
        s.m1[i] = d1[i];
    }
    s.curved = true;
    float c[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
    float L = sqrtf(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
    normalize(c);
    normalize(s.m0);
    normalize(s.m1);
    // |m| is 1 after normalize, except for a zero input direction.
    float l0 = sqrtf(s.m0[0] * s.m0[0] + s.m0[1] * s.m0[1] + s.m0[2] * s.m0[2]);
    float l1 = sqrtf(s.m1[0] * s.m1[0] + s.m1[1] * s.m1[1] + s.m1[2] * s.m1[2]);
    float f0 = sqrtf(l0 / L), f1 = sqrtf(l1 / L);
    float a = (s.m0[0] * c[0] + s.m0[1] * c[1] + s.m0[2] * c[2] + 1.0f) * 0.5f;
    float b = (s.m1[0] * c[0] + s.m1[1] * c[1] + s.m1[2] * c[2] + 1.0f) * 0.5f;
    float s0 = a <= 0.0f ? 0.0f : sqrtf(a);
    float s1 = b <= 0.0f ? 0.0f : sqrtf(b);
    float len = L / ((s1 * f1 + 1.0f - f1) * (s0 * f0 + 1.0f - f0));
    for (int i = 0; i < 3; i++) {
        s.m0[i] = s.m0[i] * len;
        s.m1[i] = s.m1[i] * len;
    }
    s.len = len;
    float old = len, ch;
    do {
        segment_arc_length(s);
        float k = s.len / old;
        for (int i = 0; i < 3; i++) {
            s.m0[i] = s.m0[i] * k;
            s.m1[i] = s.m1[i] * k;
        }
        ch = (s.len - old) / old;
        old = s.len;
    } while (fabsf(ch) > 0.01f);
    s.flat_len = s.len;   // +0x34
    s.k40 = 1.0f;         // overwritten by path_add_segment
    s.i1 = 1;
    s.f2 = 10000.0f;
    s.f3 = 10000.0f;
    s.i2 = 0;
}

// 0x4122b0 ctPath__addSegment.
void path_add_segment(Path &p, const float p0[3], const float d0[3],
                      const float p1[3], const float d1[3], float w, float f1,
                      int32_t i1, float f2, float f3, int32_t i2)
{
    Segment s;
    segment_build_hermite(s, p0, d0, p1, d1);
    s.width = w;
    s.k40 = f1;
    s.i1 = i1;
    s.f2 = f2;
    s.f3 = f3;
    s.i2 = i2;
    p.segs.push_back(s);
    p.length = s.len + p.length;
    if (p.segs.size() == 1) {
        for (int i = 0; i < 3; i++) {
            p.start[i] = p0[i];
            p.start_dir[i] = s.m0[i];
        }
        normalize(p.start_dir);
    }
    for (int i = 0; i < 3; i++) {
        p.end[i] = p1[i];
        p.end_dir[i] = s.m1[i];
    }
    normalize(p.end_dir);
}

// 0x414050 ctPath__projectStrict — no endpoint fallback.
bool path_project_strict(const Path &p, float x, float z, float *d, float *lat)
{
    int best = -1;
    float acc = 0;
    for (size_t i = 0; i < p.segs.size(); i++) {
        const Segment &s = p.segs[i];
        float sd = 0, sl = 0;
        if (segment_project(s, x, z, &sd, &sl) &&
            (best == -1 || fabsf(sl) < fabsf(*lat))) {
            *lat = sl;
            *d = sd + acc;
            best = (int)i;
        }
        acc = s.len + acc;
    }
    return best != -1;
}

// 0x4133f0 ctPath__posOffsetAt.
void path_pos_offset_at(const Path &p, float d, float off, float out[3])
{
    float local = 0;
    size_t i = path_seg_at(p, &d, &local);
    // INVENTED: the exe reads segs[0] out of bounds when n == 0.
    if (i >= p.segs.size()) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    float side[3];
    segment_pos(p.segs[i], local, out);
    segment_side(p.segs[i], local, side);
    for (int k = 0; k < 3; k++) out[k] = side[k] * off + out[k];
}

// 0x413110 ctPath__sideAt.
void path_side_at(const Path &p, float d, float out[3])
{
    float local = 0;
    size_t i = path_seg_at(p, &d, &local);
    // INVENTED: the exe reads segs[0] out of bounds when n == 0.
    if (i >= p.segs.size()) {
        out[0] = out[1] = out[2] = 0.0f;
        return;
    }
    segment_side(p.segs[i], local, out);
}

// 0x414a80 ctPath__width.
float path_width_at(const Path &p, float d)
{
    float local = 0;
    size_t i = path_seg_at(p, &d, &local);
    // INVENTED: the exe reads segs[0] out of bounds when n == 0.
    return i < p.segs.size() ? p.segs[i].width : 0.0f;
}

// 0x414980 ctPath__f1.
float path_f1_at(const Path &p, float d)
{
    float local = 0;
    size_t i = path_seg_at(p, &d, &local);
    // INVENTED: the exe reads segs[0] out of bounds when n == 0.
    return i < p.segs.size() ? p.segs[i].k40 : 0.0f;
}

// 0x414a00 ctPath__i1.
int32_t path_i1_at(const Path &p, float d)
{
    float local = 0;
    size_t i = path_seg_at(p, &d, &local);
    // INVENTED: the exe reads segs[0] out of bounds when n == 0.
    return i < p.segs.size() ? p.segs[i].i1 : 0;
}

// 0x413fd0 ctPath__i2.
int32_t path_i2_at(const Path &p, float d)
{
    float local = 0;
    size_t i = path_seg_at(p, &d, &local);
    // INVENTED: the exe reads segs[0] out of bounds when n == 0.
    return i < p.segs.size() ? p.segs[i].i2 : 0;
}

// 0x413f30 ctPath__lerp: (1 - t) * f3 + t * f2 with t = local / seg.len.
float path_lerp_at(const Path &p, float d)
{
    float local = 0;
    size_t i = path_seg_at(p, &d, &local);
    // INVENTED: the exe reads segs[0] out of bounds when n == 0.
    if (i >= p.segs.size()) return 0.0f;
    const Segment &s = p.segs[i];
    float t = local / s.len;
    return (1.0f - t) * s.f3 + t * s.f2;
}

size_t Graph::node_count() const
{
    return cross_eq.size() + cross_light.size() + roads.size() + lanes.size();
}

const Lane *Graph::lane_by_node(uint32_t id) const
{
    size_t base = cross_eq.size() + cross_light.size() + roads.size();
    if (id <= base) return nullptr;
    size_t i = id - 1 - base;
    return i < lanes.size() ? &lanes[i] : nullptr;
}

// 0x419050 Traffic__nearestCross.
const Cross *Graph::nearest_cross(float x, float y, float z, float dist, bool yflag) const
{
    const Cross *best = nullptr;
    float best_key = 0.0f;   // the yflag slot before the first hit; unread then
    for (int pass = 0; pass < 2; pass++) {
        const std::vector<Cross> &list = pass == 0 ? cross_eq : cross_light;
        for (size_t i = 0; i < list.size(); i++) {
            const Cross &e = list[i];
            if (yflag && !(fabsf(e.pos[1] - y) < 5.0f)) continue;
            float dx = e.pos[0] - x, dy = e.pos[1] - y, dz = e.pos[2] - z;
            float key = fabsf(sqrtf(dx * dx + dy * dy + dz * dz) - dist);
            if (!best || key < best_key) {
                best = &e;
                best_key = key;
            }
        }
    }
    return best;
}

// 0x417c50 Traffic__clearHalts.
void Graph::clear_halts()
{
    for (size_t i = 0; i < lanes.size(); i++) lanes[i].halted = false;
}

// 0x4191d0 Traffic__nearestLanePos.
const Lane *Graph::nearest_lane_pos(float x, float y, float z, float *d, float *lat) const
{
    int lo = 0, hi = (int)lanes.size() - 1, mid = -1;
    while (lo <= hi) {
        mid = (lo + hi) / 2;   // signed, truncating
        if (x + 20.0f > lanes[(size_t)mid].index_min_x) lo = mid + 1;
        else hi = mid - 1;
    }
    const Lane *best = nullptr;
    float best_d2 = 0, best_d = 0, best_lat = 0;
    for (int i = mid; i >= 0; i--) {
        const Lane &e = lanes[(size_t)i];
        if (!(e.index_run_max_x > x - 20.0f)) break;
        if (!(e.index_max_x > x - 20.0f)) continue;
        // INVENTED: the exe indexes the lane array unchecked.
        if (e.index_lane >= lanes.size()) continue;
        const Lane &l = lanes[e.index_lane];
        float sd = 0, sl = 0;
        if (!path_project(l.path, x, z, &sd, &sl)) continue;
        float pos[3];
        path_pos_at(l.path, sd, pos);
        float dy = pos[1] - y;
        if (5.0f > fabsf(dy)) {
            float d2 = (pos[0] - x) * (pos[0] - x) + dy * dy + (pos[2] - z) * (pos[2] - z);
            if (!best || best_d2 > d2) {
                best_d = sd;
                best = &l;
                best_d2 = d2;
                best_lat = sl;
            }
        }
    }
    if (best) {
        if (d) *d = best_d;
        if (lat) *lat = best_lat;
    }
    return best;
}

// 0x419410 Traffic__alignToRoad.
bool Graph::align_to_road(float pos[3], float dir[3]) const
{
    const Lane *l = nearest_lane_pos(pos[0], pos[1], pos[2], nullptr, nullptr);
    if (!l) return false;
    // INVENTED: a lane whose road id is not a road returns false (the exe would
    // dereference the empty table entry).
    if (l->road < 1 + cross_eq.size() + cross_light.size()) return false;
    size_t ri = l->road - 1 - cross_eq.size() - cross_light.size();
    if (ri >= roads.size()) return false;
    const Path &p = roads[ri].path;
    // INVENTED: d = 0 before the projection; the exe keeps a stale stack slot.
    // With >= 1 segment the projection cannot fail anyway.
    float d = 0.0f, lat = 0;
    path_project(p, pos[0], pos[2], &d, &lat);
    if (dir) {
        float v[3];
        path_dir_at(p, d, v);
        for (int i = 0; i < 3; i++) dir[i] = p.length * 0.5f > d ? v[i] : -v[i];
    }
    path_pos_at(p, d, pos);
    return true;
}

namespace {

// Node ids are 1-based into [cross_eq, cross_light, roads, lanes]; 0 = null.
// INVENTED: the exe dereferences the node table unchecked; a missing or
// non-lane id yields null here.
Lane *lane_at(Graph &g, uint32_t id)
{
    return const_cast<Lane *>(g.lane_by_node(id));
}

// 0x4116d3: lane index -> node id, the inverse of Graph::lane_by_node.
// INVENTED: the exe stores a lane pointer.
uint32_t lane_node_id(const Graph &g, size_t li)
{
    return (uint32_t)(1 + li + g.cross_eq.size() + g.cross_light.size() +
                      g.roads.size());
}

// INVENTED: node id -> cross with a bounds check (the exe dereferences the table).
// INVENTED: takes const Graph& so read-only callers bind without a cast; the exe
// stores mutable Cross*, so the located element is const_cast back for writers.
Cross *cross_by_node(const Graph &g, uint32_t id, bool *is_light)
{
    if (id == 0) return nullptr;
    if (id <= g.cross_eq.size()) {
        if (is_light) *is_light = false;
        return const_cast<Cross *>(&g.cross_eq[id - 1]);
    }
    size_t i = id - 1 - g.cross_eq.size();
    if (i < g.cross_light.size()) {
        if (is_light) *is_light = true;
        return const_cast<Cross *>(&g.cross_light[i]);
    }
    return nullptr;
}

// INVENTED: node id -> road with a bounds check (the exe dereferences the table).
Road *road_by_node(Graph &g, uint32_t id)
{
    size_t base = g.cross_eq.size() + g.cross_light.size();
    if (id <= base) return nullptr;
    size_t i = id - 1 - base;
    return i < g.roads.size() ? &g.roads[i] : nullptr;
}

// 0x415650 ctCross init (0x40e0d0 inlined).
void cross_init(Graph &g, Cross &c)
{
    c.live_refs = 0;
    c.radius = 0.0f;
    for (const CrossLink &link : c.links) {
        Lane *l = lane_at(g, link.node);
        if (!l) continue;                        // INVENTED guard
        const float *e = link.flag ? l->v28 : l->v1c;
        float dx = e[0] - c.pos[0], dy = e[1] - c.pos[1], dz = e[2] - c.pos[2];
        float d = sqrtf(dx * dx + dy * dy + dz * dz);
        if (d > c.radius) c.radius = d;          // strict >
    }
    c.path_state.assign(c.paths.size(), PathState{});   // 0x415650: realloc(+0x20 * 8), paths count
    uint32_t mx = 0;
    uint16_t mn = c.paths.empty() ? 0 : c.paths[0].c;  // INVENTED: no paths -> 0
    for (const CrossPath &p : c.paths) {
        if (p.c > mx) mx = p.c;
        if (p.c < mn) mn = p.c;
    }
    mx -= mx % 10;
    if (mx == (uint32_t)(mn / 10) * 10) mx += 10;
    c.cycle = mx;
}

// 0x415700 ctCrossLight init.
void light_init(Graph &g, Cross &c)
{
    cross_init(g, c);
    c.phase_time = g.sim_time;
    c.phase.assign(c.links.size(), 0);
    int32_t cnt = 0;
    // INVENTED: lane_at guard; the exe reads links[0] unchecked.
    Lane *first = c.links.empty() ? nullptr : lane_at(g, c.links[0].node);
    uint32_t prev = first ? first->road : 0;
    for (size_t i = 0; i < c.links.size(); i++) {
        if (!c.links[i].flag) {
            c.phase[i] = -1;                     // prev unchanged
            continue;
        }
        Lane *l = lane_at(g, c.links[i].node);
        if (!l) continue;                        // INVENTED guard
        uint32_t r = l->road;
        if (r != prev) cnt++;
        c.phase[i] = cnt;
        prev = r;
    }
    c.phase_count = cnt + 1;
    c.cycle = 0;
}

// 0x415b40 ctCross__claimPath: path i claims every path whose (a, b) span it
// crosses; count stays 0 until the first claim.
void cross_claim_path(Cross &c, size_t i)
{
    PathState &si = c.path_state[i];
    if (si.count == 0) {
        const CrossPath &pi = c.paths[i];
        const uint8_t a = pi.a, b = pi.b;
        const int32_t prio = pi.c;
        for (size_t j = 0; j < c.paths.size(); j++) {
            PathState &sj = c.path_state[j];
            if (sj.block < prio) {
                const uint8_t aj = c.paths[j].a, bj = c.paths[j].b;
                if (aj != a) {
                    if (bj == b) {
                        sj.block = prio;
                    } else {
                        const bool in_a = a < b ? (a < aj && aj < b) : (a < aj || aj < b);
                        const bool in_b = a < b ? (a < bj && bj < b) : (a < bj || bj < b);
                        if (in_a != in_b) sj.block = prio;
                    }
                }
            }
        }
    }
    si.count += 10;
}

// 0x415b00 ctCross__pathBlocked: true while another path claims the block. Used
// by the live-car step (card 233).
[[maybe_unused]] bool cross_path_blocked(Cross &c, size_t i)
{
    if (c.path_state[i].block != 0) return true;
    cross_claim_path(c, i);
    return false;
}

// 0x415c40 ctCrossEq__step: decay the claim counts while the cross goes
// inactive, else re-claim; false unlinks the cross from the active list.
bool cross_eq_step(Cross &c)
{
    for (PathState &s : c.path_state) s.block = 0;
    const int32_t r = c.live_refs;                    // +0x38
    if (r < 0) {
        bool any = false;
        for (size_t i = 0; i < c.paths.size(); i++) {
            PathState &s = c.path_state[i];
            if (s.count > 4) s.count = 5;
            s.count -= 1;
            if (s.count < 1) {
                s.count = 0;
            } else {
                any = true;
                for (size_t j = 0; j < c.paths.size(); j++)
                    if (c.paths[i].b == c.paths[j].b) c.path_state[j].block = 0x3ea;
            }
        }
        if (any) return true;
        c.live_refs = 0;
        return false;
    }
    if (r > 0) {
        for (size_t i = 0; i < c.paths.size(); i++) {
            PathState &s = c.path_state[i];
            if (s.count > 4) s.count = 5;
            const int32_t cnt = s.count;
            s.count = 0;
            if (cnt > 1) {
                cross_claim_path(c, i);
                s.count = cnt - 1;
            }
        }
        return true;
    }
    return false;
}

// 0x415d60 ctCrossLight__step: eq counters, then the phase timer; the amber
// phase blocks every path.
bool cross_light_step(Graph &g, Cross &c)
{
    cross_eq_step(c);                                 // result ignored
    const int32_t r = c.live_refs;
    if (r < 1) return r != 0;
    if (c.light_remain <= 0) {
        float a = 0.1f, b = 0.1f;
        int na = 0, nb = 0;
        for (size_t k = 0; k < c.links.size(); k++) {
            const Lane *l = lane_at(g, c.links[k].node);
            if (!l) continue;                         // INVENTED: a non-lane link is skipped
            const float f = l->kc8 / l->kcc;          // +0xc8, +0xcc
            if (c.phase[k] == c.light_cur) { a += f; na++; }
            else { b += f; nb++; }
        }
        if (na > 1) a /= (float)na;
        if (nb > 1) b /= (float)nb;
        if ((a + a) / b + c.phase_time < g.sim_time) {
            c.phase_time = g.sim_time;
            c.light_remain = 4.0f;
            // shortcut: Game__queueEvent(cross+0x44, 0x10, "change_lamp") not delivered (no sink in src).
        }
    } else {
        c.light_remain -= g.sim_time - c.phase_time;
        c.phase_time = g.sim_time;
        if (c.light_remain <= 0) {
            c.light_remain = 0;
            c.light_cur = (c.light_cur + 1) % c.phase_count;
            // shortcut: Game__queueEvent(cross+0x44, 0x10, "change_lamp") not delivered (no sink in src).
        }
    }
    if (c.light_remain <= 0) {
        for (size_t i = 0; i < c.paths.size(); i++) {
            const uint8_t a = c.paths[i].a;
            // INVENTED: a path entry link outside the phase table counts as
            // matching (the exe indexes the array unchecked).
            if (a < c.phase.size() && c.phase[a] != c.light_cur)
                c.path_state[i].block = 0x3ec;
        }
    } else {
        for (PathState &s : c.path_state) s.block = 0x3eb;
    }
    return c.live_refs != 0;
}

// 0x415f20 ctCrossEq__vf10 / 0x415f40 ctCrossLight__vf10: move the cross to the
// head of its kind's active list.
void cross_activate(Graph &g, Cross &c, bool light)
{
    uint32_t &head = light ? g.active_light : g.active_eq;
    std::vector<Cross> &list = light ? g.cross_light : g.cross_eq;
    if (c.active_next == 0) {
        c.active_next = head;
        head = (uint32_t)(&c - list.data()) + 1;
    }
}

// 0x40e260 ctRoad init (consts 1.1, 0.9090909, 1.75).
void road_init(Graph &g, Road &road)
{
    // The exe walks the road's lane chain: road.node[0] then lane+0x14.
    std::vector<Lane *> ls;
    Lane *l = lane_at(g, road.node[0]);
    // INVENTED: cap the pointer walk with the lane count (the exe follows a cycle).
    for (size_t guard = 0; l && guard < g.lanes.size(); guard++) {
        ls.push_back(l);
        l = lane_at(g, l->node14);
    }

    int n = 0;
    for (const Lane *lane : ls)
        n += 1 - (lane->node88[0] != 0) - (lane->node88[1] != 0);

    for (int dir = 0; dir < 2; dir++) {
        float f = 1.0f;
        for (const Lane *lane : ls)
            if (lane->k18 == dir && !lane->node88[0] && !lane->node88[1]) f *= 1.1f;
        for (Lane *lane : ls)
            if (lane->k18 == dir && !lane->node88[0] && !lane->node88[1]) {
                f *= 0.9090909f;
                lane->k98 *= f;
            }
    }

    float w = (float)n * 1.75f;
    for (Segment &s : road.path.segs) s.width = w;
    if (w > g.max_road_width) g.max_road_width = w;
}

// 0x40e150 ctLane init, no-car branch.
void lane_init(Graph &g, Lane &l)
{
    l.kc8 = 0.0f;
    // shortcut: the car branch (0x4107c0 per car, 0x417050 timed event) is not
    // ported; the pool is empty at init.
    l.kc0 = g.sim_time;
    l.kcc = l.path.length;
    l.halted = false;
}

// 0x40eef0 TrafficCarPool__alloc: free list first, else a new stable slot.
uint32_t car_alloc(Graph &g)
{
    if (!g.free_cars.empty()) {
        uint32_t i = g.free_cars.back();
        g.free_cars.pop_back();
        return i;
    }
    g.cars.emplace_back();
    return (uint32_t)(g.cars.size() - 1);
}

// 0x40c400 TrafficCar__ctor (+ 0x40e040): serial, constants, everything else 0.
void car_ctor(Graph &g, uint32_t i)
{
    Car &c = g.cars[i];
    c = Car();
    c.serial = ++g.car_serial;
    c.k1c = 0.5f;
    c.k28 = 1.2f;
    c.k34 = 0.06f;
    c.k38 = 6.0f;
    c.k3c = 0.01f;
    c.k40 = -1;
    c.e0 = c.e4 = c.e8 = g.sim_time;
}

// 0x41180d..0x4118b0, inlined in TrafficCar__step step 8 (defined below).
void obstacle_unlink(Graph &g, uint32_t ci);

// 0x40c730 TrafficCar__release: unlink the refs and clear the serial; the
// caller pushes the slot on the free list.
void car_release(Graph &g, uint32_t i)
{
    // 0x40c74b -> 0x40d3c0 third block: drop the obstacle node.
    obstacle_unlink(g, i);
    g.cars[i].serial = 0;
    // INVENTED: the +0x130 instance and +0x144 type link unlinks are not modelled.
}

// 0x40dd00 ctCross__vf6: max of the two link lanes' +0x98 for path idx.
// INVENTED: missing index/node guards (the exe indexes and dereferences unchecked).
float cross_vf6(const Graph &g, const Cross &c, size_t idx)
{
    if (idx >= c.paths.size()) return 0.0f;
    const CrossPath &p = c.paths[idx];
    if (p.a >= c.links.size() || p.b >= c.links.size()) return 0.0f;
    const Lane *a = g.lane_by_node(c.links[p.a].node);
    const Lane *b = g.lane_by_node(c.links[p.b].node);
    const float va = a ? a->k98 : 0.0f;
    const float vb = b ? b->k98 : 0.0f;
    return vb < va ? va : vb;   // COMISS + JBE: keep vb on NaN
}

// 0x40dc00 TrafficCar__targetSpeed: lane speed limit and next-lane/cross objects.
float car_target_speed(Graph &g, const Car &car)
{
    float s = car.max_speed;
    if (car.path) {                    // INVENTED proxy for car+0xc4 != 0
        float v = s;
        bool have = false;
        if (car.dir < 0) {             // lane vf6 0x40dd50
            // INVENTED: bounds check (the exe dereferences the lane pointer).
            if (car.path_lane < g.lanes.size()) {
                v = g.lanes[car.path_lane].k98;
                have = true;
            }
        } else {                       // cross vf6 0x40dd00
            Cross *c = cross_by_node(g, car.path_lane, nullptr);
            // INVENTED: null/path-index guard (the exe dereferences the cross).
            if (c && (size_t)car.dir < c->paths.size()) {
                v = cross_vf6(g, *c, (size_t)car.dir);
                have = true;
            }
        }
        if (have && v < s) s = v;
    }
    float f = 0.0f;
    if (!car.next_lane_b) {            // +0xbc, next lane
        if (car.k124) {
            // 0x40dc00: +0x124 target lane vf6 (0x40dd50 = lane+0x98), averaged with s.
            // INVENTED: null guard (the exe dereferences a non-null target lane pointer).
            const Lane *target = g.lane_by_node(car.k124);
            if (target) s = (target->k98 + s) * 0.5f;
        }
    } else {
        f = car.max_speed;
        float v = 0.0f;
        bool have = false;
        if (car.kc0 < 0) {             // lane next owner = lane index
            // INVENTED: bounds check (the exe dereferences the lane pointer).
            if (car.next_lane_a < g.lanes.size()) {
                v = g.lanes[car.next_lane_a].k98;   // ctLane vf6 0x40dd50
                have = true;
            }
        } else {                       // cross next owner = cross node id
            Cross *c = cross_by_node(g, car.next_lane_a, nullptr);
            // INVENTED: null/path-index guard (the exe dereferences the cross).
            if (c && (size_t)car.kc0 < c->paths.size()) {
                v = cross_vf6(g, *c, (size_t)car.kc0);
                have = true;
            }
        }
        if (have && v < f) f = v;
    }
    return f <= s ? s : f;
}

// 0x4107c0 TrafficCar__step; defined after lane_detach (mutual recursion:
// step 6 -> lane_attach/lane_detach -> car_time_to_lane_end -> car_step).
void car_step(Graph &g, uint32_t ci);
// 0x40d1e0 ctLane__insertCar; defined after car_step.
void lane_insert(Graph &g, size_t li, uint32_t ci);

// 0x40d990 TrafficCar__timeToLaneEnd: step the car first, then 0 on a lane
// change, else the 2v braking time to the stop offset (min speed 10).
float car_time_to_lane_end(Graph &g, uint32_t ci)
{
    car_step(g, ci);
    const Car &car = g.cars[ci];
    float stop = car.next_lane_b ? 0.0f : g.cross_stop_offset;
    float v = car_target_speed(g, car);
    if (v <= 10.0f) v = 10.0f;
    float t = ((car.pos - car.k14) - stop) * (2.0f / v);
    return t < 0.0f ? 0.0f : t;
}

// 0x417050 Traffic__scheduleLane -> 0x41a950 TrafficHeap__push: min-heap by time.
void schedule_lane(Graph &g, float dt, uint32_t lane)
{
    const Graph::HeapEntry e = {g.sim_time + dt, lane};
    size_t i = g.heap.size();
    g.heap.push_back(e);
    while (i) {
        size_t p = (i - 1) / 2;
        if (g.heap[p].time <= e.time) break;
        g.heap[i] = g.heap[p];
        i = p;
    }
    g.heap[i] = e;
}

// 0x40f5e0 TrafficCar__setLive (ECX = car, char on): live flag (+0xdc), the
// live count and the +0x44 live list. Shared by 0x40cbd0 and 0x40f950.
void car_set_live(Graph &g, uint32_t ci, bool on)
{
    Car &car = g.cars[ci];
    if (car.live == (on ? 1 : 0)) return;            // +0xdc
    if (on) {
        g.live_count++;
        if (!car.type_id && car.inst_id && car.notify) {
            // shortcut: the notify sink (T+0x3130) is not modelled (notes/29 §5)
        }
        car.live = 1;
        if (car.live_next == 0) {                    // head insert into the live list
            car.live_next = g.live_cars;
            g.live_cars = ci + 1;
        }
    } else {
        g.live_count--;
        if (!car.type_id && car.inst_id && car.notify) {
            // shortcut: the notify sink (T+0x3130) is not modelled (notes/29 §5)
        }
        car.live = 0;
    }
    if (car.live == 0 && car.k128 != 0) car.k128 = 0;   // +0x128
}

// 0x41a500 Traffic__queueSpawn (ECX = T): append a WeakCarRef for car to the
// spawn queue (+0x310c). 0x40ca10 writes the serial and the car only.
static void queue_spawn(Graph &g, uint32_t ci)
{
    LeaderSlot e;
    e.serial = g.cars[ci].serial;                // 0x40ca10: e+0 = car+0
    e.car = ci + 1;                              // e+4 = car (INVENTED: 1-based index)
    // INVENTED: the exe leaves the realloc'd +8/+0xc bytes unwritten; LeaderSlot
    // value-initializes flag/dist to 0.
    g.spawn_queue.push_back(e);
}

// 0x410680 TrafficCar__syncInstance (ECX = car, arg ResRefLink *link): bring the
// instance link, the spawn queue and the park side in step with the owner link.
// bound = link+8 != 0 (our object id != 0). Returns nothing in every branch.
void car_sync_instance(Graph &g, uint32_t ci, uint32_t link_object)
{
    Car &car = g.cars[ci];
    const uint32_t inst = car.inst_id;              // +0x138
    const uint32_t type = car.type_id;              // +0x14c
    const bool bound = link_object != 0;
    if (type == 0) {                                // 0x41069c
        if (!inst) return;
        if (!car.live || !bound) {
            // shortcut: 0x4ec210 link_reparent(&car+0x130, T+0x311c) not ported.
        } else {
            // shortcut: 0x4ec210 link_reparent(&car+0x130, link) not ported.
        }
        return;
    }
    if (!car.live || !bound) {                      // 0x4106d0 -> 0x410713
        if (!inst) return;
        if (car.notify) {
            // shortcut: the T+0x3130 notify sink (0x5935fc "car_rem %d %d") is
            // not modelled.
        }
        car.e8 = g.sim_time;                        // +0xe8 = T+0x84
        // shortcut: Traffic__parkInstance 0x417080(type, &car+0x130) not ported.
        car.inst_id = 0;                            // 0x41077e ResRefLink__dtor clears +0x138
        g.inst_count -= 1;                          // 0x410783
        g.parked_count += 1;                        // 0x410789
        return;
    }
    if (!inst) {                                    // 0x4106dd
        g.inst_count += 1;                          // 0x4106e7
        g.spawn_count += 1;                         // 0x4106ed
        queue_spawn(g, ci);                         // 0x4106f4
        return;
    }
    // shortcut: 0x4ec210 link_reparent(&car+0x130, link) not ported.
}

// 0x40cbd0 ctLane__vf3: live/not-live bookkeeping, first-event scheduling and
// occupancy. The exe's tail-append branch is a no-op: insert already set car.lane.
void lane_attach(Graph &g, size_t li, uint32_t ci)
{
    Lane &lane = g.lanes[li];
    Car &car = g.cars[ci];

    if (lane.blocker.car == ci + 1 && lane.blocker.serial == car.serial)
        lane.blocker = Lane::Blocker();

    car_set_live(g, ci, lane.live != 0);             // 0x40f5e0
    if (!lane.live) {
        if (!lane.cars.empty() && lane.cars.front() == ci) {
            float t = car_time_to_lane_end(g, ci);
            if (lane.pending_events == 0) {
                schedule_lane(g, t, (uint32_t)li);
                lane.pending_events++;
            }
        }
    }
    // 0x410680 with the road's bound object (+0x68).
    Road *road = road_by_node(g, lane.road);
    car_sync_instance(g, ci, road ? road->object : 0);
    if (lane.k34 == 0xffffffffu)
        car.k124 = lane.node88[2] ? lane.node88[2] : lane.node88[3];
    lane.kc8 += car.k24 + car.k14 + car.k20;
}

// 0x40fa50 TrafficCar__dropLive.
void car_drop_live(Graph &g, Car &car)
{
    if (car.next_lane_b == 0 && car.live) {   // +0xbc, +0xdc
        g.live_count--;
        if (!car.type_id && car.inst_id && car.notify) {
            // shortcut: the notify sink (T+0x3130) is not modelled (notes/29 §5)
        }
        car.live = 0;
        if (car.k128) car.k128 = 0;
    }
}

// CarPathRef at car+0xc4 = {lane, path, dir}; src keeps path_lane/path/dir.

// 0x40dde0 CarPathRef__posOffset (ECX = ref): path point at d shifted by off.
void car_ref_pos_offset(const Path *path, int32_t dir, float d, float off, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!path) return;
    if (dir >= 0) {
        // Cross ref: +0xbc holds the raw &CrossPath::seg (Segment), not a Path.
        segment_pos_offset(*reinterpret_cast<const Segment *>(path), d, off, out);  // 0x413660
        return;
    }
    path_pos_offset_at(*path, d, off, out);      // 0x4133f0 ctPath__posOffsetAt
}

// 0x40da20 CarPathRef__heightAt (ECX = ref): road-surface y at (x, z); *y stays
// written when the caller's |lat| < 10 test then fails.
bool car_ref_height(const Path *path, int32_t dir, float x, float z, float *y)
{
    if (!path) return false;
    float lat = 0.0f;
    bool ok = dir < 0 ? path_height(*path, x, z, y, &lat)      // 0x4136e0
                      : segment_height(*reinterpret_cast<const Segment *>(path), x, z, y, &lat);
    return ok && fabsf(lat) < 10.0f;             // 0x59373c = 10.0f
}

// 0x40dd60 CarPathRef__dir (ECX = ref): path direction at d.
void car_ref_dir(const Path *path, int32_t dir, float d, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!path) return;
    if (dir >= 0) {
        // Cross ref: &CrossPath::seg read as a Segment.
        segment_dir(*reinterpret_cast<const Segment *>(path), d, out);  // 0x412f60
        return;
    }
    path_dir_at(*path, d, out);                  // 0x412ed0 ctPath__dirAt
}

// 0x40e810 CarPathRef__length (ECX = ref): current path length.
float car_ref_length(const Path *path, int32_t dir)
{
    if (!path) return 0.0f;
    if (dir >= 0) {
        // Cross ref: Segment::len (+0x30).
        return reinterpret_cast<const Segment *>(path)->len;
    }
    return path->length;                         // +0x38
}

// 0x4102d0 TrafficCar__advance (ECX = car): world position from the path
// position, heading (+0xa0/+0xa4) and front point (+0x98/+0x9c).
void car_advance(Graph &g, Car &car)
{
    if (!car.path) return;                       // +0xc
    const float t0 = car.e4, t = g.sim_time;     // +0xe4, +0x84
    if (!(t0 < t)) return;                       // JBE
    car.e4 = t;
    const float dt = t - t0;
    // shortcut: the exe calls TrafficCar__step 0x4107c0 here; car_step (card 235)
    // takes a pool index this card does not thread through, so the pose below is
    // computed from the pos the active-lane walk left (notes/37 §4/§5).

    const float oldx = car.world[0], oldz = car.world[2];
    float p[3];
    car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral, p);   // 0x40dde0
    for (int k = 0; k < 3; k++) car.world[k] = p[k];    // +0x8c/+0x90/+0x94
    const float dx = car.world[0] - oldx;
    const float dz = car.world[2] - oldz;
    const float dist = sqrtf(dx * dx + dz * dz);
    if (dist == 0.0f) return;
    const float L = car.k18;                     // +0x18
    float tx, tz;
    // 0x4103d0: dt and L are promoted to double and (dt + 1) is a double.
    if ((double)L / ((double)dt + 1.0) <= (double)dist) {  // snapped: front from the path
        float dir[3];
        car_ref_dir(car.path, car.dir, car.pos, dir);   // 0x40dd60
        car.heading_x = -dir[0];                 // +0xa0 (dir[1] unused)
        car.heading_z = -dir[2];                 // +0xa4
        const float d = car.pos + L;
        const float dl = car_ref_length(car.path, car.dir);   // 0x40e810
        const float d2 = d - dl;
        float q[3];
        // 0x4105cc (d2 > 0) reads the prev ref (+0xd4/+0xd8); 0x4105ba reads
        // the cur ref at pos + L.
        if (d2 > 0.0f)
            car_ref_pos_offset(car.prev_path, car.prev_dir, d2, car.lateral, q);
        else
            car_ref_pos_offset(car.path, car.dir, d, car.lateral, q);
        tx = q[0];
        tz = q[2];
    } else {
        if (0.01f < dist) {                      // 0x5935d0
            const float inv = 1.0f / dist;       // 0x410400: DIVSS then MULSS
            car.heading_x = dx * inv;
            car.heading_z = dz * inv;
        }
        float fx = car.front_x - oldx;           // +0x98
        float fz = car.front_z - oldz;           // +0x9c
        const float h = sqrtf(fx * fx + fz * fz);
        if (1e-05 < (double)h) {                 // 0x410461: double compare
            const float inv = 1.0f / h;
            fx = fx * inv; fz = fz * inv;
        }
        const float dot = fx * dx + fz * dz;
        // 0x410493: (dot*dot) * (dist*dist) then + L*L - dist*dist.
        const float disc = ((dot * dot) * (dist * dist) + L * L) - dist * dist;
        const float root = disc > 0.0f ? sqrtf(disc) : 0.0f;
        float f = dot * dist + root;
        if (g.front_mode == 1)                   // global 0x5e6518
            f = f + (L - f) / (dist + 1.0f);
        else if (g.front_mode == 2)
            f = L;
        tx = fx * f + oldx;
        tz = fz * f + oldz;
    }
    float nx = tx - car.world[0];
    float nz = tz - car.world[2];
    const float n = sqrtf(nx * nx + nz * nz);
    if (1e-05 < (double)n) {                     // 0x41061f: double compare
        const float inv = 1.0f / n;
        nx = nx * inv; nz = nz * inv;
    }
    car.front_x = car.world[0] + nx * L;         // +0x98
    car.front_z = car.world[2] + nz * L;         // +0x9c (the y of the front point
                                                 // is never stored)
}

// 0x4118f0 TrafficCar__pushInstance (ECX = car): per-frame advance plus the
// instance pose send.
void car_push_instance(Graph &g, Car &car)
{
    if (!car.path || !car.live) return;          // +0xc, +0xdc
    const float t = g.sim_time;                  // +0x84
    if (!(car.e8 < t)) return;                   // +0xe8
    car.e8 = t;                                  // dt is only read by the tail below
    // shortcut: first-instance init 0x411975..0x411a53 (getInfo 0x47c640 codes
    // 0x40/0x46/0x41 -> +0x18/+0x1c/+0x14/+0x20/+0x28/+0x120) needs Game__getInfo on
    // a car instance; src has none, and the exe skips the block when a getInfo fails.
    car_advance(g, car);                         // 0x4102d0
    if (car.inst_id == 0) return;                // 0x411c49
    // shortcut: instance pose tail 0x411c4f..0x411ef6 (path y 0x40da20, Mat34
    // 0x567250, euler 0x569970, 0x56a000, typed vf7 0x4eb3f0) - SERVICE MISSING:
    // src has no traffic_car typed object, so nothing observes the push.
}

// 0x40e380 ctCross__vf1 (vtable +0x04; ECX = cross): CMP [ECX+0x38],0;
// SETNZ AL; RET. Reads only live_refs (+0x38); no writes, no calls.
// -1 (dying) is active, only 0 is not.
bool cross_active(const Cross &c) { return c.live_refs != 0; }

// 0x40f350 TrafficCar__liveStep: a live car stays listed while its current ref
// owner is live; the cross-claim branches (idx >= 0) have no src state.
bool car_live_step(Graph &g, Car &car)
{
    if (!car.live) {              // +0xdc
        car_push_instance(g, car);    // 0x4118f0 -> 0x4102d0
        return false;
    }
    // 0x40f3f3: vf1 dispatched on the object at *(Car+0xc4). The owner kind is
    // tracked by cross_node (set by cross_attach 0x40cb60, cleared by
    // lane_unlink 0x40ca40), not by dir: 0x4107c0 step 7 rewrites +0xc4 to a
    // lane without touching +0xcc, so dir can stay >= 0 on a lane owner.
    bool cur_live;
    if (car.cross_node != 0) {
        const Cross *c = cross_by_node(g, car.cross_node, nullptr);  // INVENTED null guard
        cur_live = c != nullptr && cross_active(*c);                 // cross vf1 0x40e380
    } else {
        // lane vf1 0x40e390 = lane+0xa8 live
        cur_live = car.path_lane < g.lanes.size() && g.lanes[car.path_lane].live;
    }
    // 0x40f402: drop the car when the owner is not live and there is no next ref.
    if (!car.next_lane_b && !cur_live && car.live) {
        g.live_count--;
        if (!car.type_id && car.inst_id && car.notify) {
            // shortcut: the notify sink (T+0x3130) is not modelled (notes/29 §5)
        }
        car.live = 0;
        if (car.k128) car.k128 = 0;
    }
    car_push_instance(g, car);    // 0x4118f0 -> 0x4102d0
    return true;
}

// 0x40ca40 TrafficCar__unlinkLane: unlink from the owner's car list (a lane, or
// a cross's +8/+0xc list).
// INVENTED: the vector erase replaces the exe's intrusive prev/next pointers, and
// the exe has no lane bounds check (it trusts the lane pointer).
void lane_unlink(Graph &g, uint32_t ci)
{
    Car &car = g.cars[ci];
    if (car.cross_node != 0) {
        Cross *c = cross_by_node(g, car.cross_node, nullptr);   // INVENTED guard
        if (c) {
            std::vector<uint32_t>::iterator it = std::find(c->cars.begin(), c->cars.end(), ci);
            if (it != c->cars.end()) c->cars.erase(it);
        }
    } else if (car.lane < g.lanes.size()) {
        std::vector<uint32_t> &list = g.lanes[car.lane].cars;
        std::vector<uint32_t>::iterator it = std::find(list.begin(), list.end(), ci);
        if (it != list.end()) list.erase(it);
    }
    car.prev = car.next = car.lane = 0;
    car.cross_node = 0;
}

// 0x40e710 ctLane__vf4: detach a removed car from its lane (dir unused).
void lane_detach(Graph &g, size_t li, uint32_t ci)
{
    Lane &lane = g.lanes[li];
    Car &car = g.cars[ci];
    lane.kc8 -= car.k24 + car.k14 + car.k20;
    if (lane.kc8 < 0.0f) lane.kc8 = 0.0f;
    if (!lane.cars.empty()) {   // f = the new first car (car is already unlinked)
        // 0x4107c0 through car_time_to_lane_end 0x40d990, like 0x40e710.
        float t = car_time_to_lane_end(g, lane.cars.front());
        if (lane.pending_events == 0) {
            schedule_lane(g, t, (uint32_t)li);
            lane.pending_events++;
        }
    }
    if (lane.k34 == 0xffffffffu) car.k124 = 0;
}

// 0x40d2c0 TrafficCar__noteLeader: record a lane leader (flag 0) or a
// target-lane car (flag 1) in one of the four slots. INVENTED: the leader is a
// 1-based car index, 0 = none (the exe stores a Car*).
void car_note_leader(Graph &g, uint32_t ci, uint32_t leader, uint8_t flag)
{
    LeaderSlot *slot = g.cars[ci].leaders;
    for (int i = 0; i < 4; i++) {                          // 0x40d2d0..0x40d2e8
        if (slot[i].car == leader &&
            (leader == 0 || slot[i].serial == g.cars[leader - 1].serial)) {
            slot[i].flag = flag;                           // 0x40d308
            return;
        }
    }
    int i;
    for (i = 0; i < 3; i++) {                              // 0x40d2f0..0x40d304
        if (slot[i].car == 0) break;                       // 0x40d2f3
        if (slot[i].serial != g.cars[slot[i].car - 1].serial) {   // 0x40d2f9
            slot[i].serial = slot[i].car = 0; slot[i].flag = 0;   // 0x40d319..0x40d326
            break;
        }
    }
    if (leader) { slot[i].serial = g.cars[leader - 1].serial; slot[i].car = leader; }  // 0x40d359/0x40d361
    else        { slot[i].serial = 0; slot[i].car = 0; slot[i].flag = 0; }            // 0x40d339..0x40d347
    slot[i].flag = flag;                                   // 0x40d36a
}

// 0x411f00 ctLane__vf2 (ECX = lane, arg = Car*): cache the lane's tail car and
// note the previous tail (or the still-valid cached blocker) as this car's leader.
void lane_cache_tail(Graph &g, size_t li, uint32_t ci)
{
    Lane &lane = g.lanes[li];
    const uint32_t cached = lane.blocker.car;        // +0xb4 (1-based; 0 = null)
    // INVENTED: bounds check the exe does not have on the cached index.
    if (cached != 0 && cached <= g.cars.size() &&
        lane.blocker.serial == g.cars[cached - 1].serial) {   // +0xb0 == car+0
        car_note_leader(g, ci, cached, 0);           // 0x40d2c0
    } else {
        if (cached != 0) lane.blocker = Lane::Blocker();   // +0xb0/+0xb4/+0xb8
        if (!lane.cars.empty())                      // +0xc cars tail
            car_note_leader(g, ci, lane.cars.back() + 1, 0);
    }
    // The exe clears the blocker when arg is null; every src call passes a car.
    lane.blocker.car = ci + 1;                       // +0xb4; +0xb8 (k) untouched
    lane.blocker.serial = g.cars[ci].serial;         // +0xb0
}

// 0x40f950 CarPathRef__set (ECX = car, args: owner lane, path, dir): hand the
// car's current ref to a lane, or only record it as the next ref when live.
void car_set_ref(Graph &g, uint32_t ci, size_t li, const Path *path, int32_t dir)
{
    Car &car = g.cars[ci];
    if (!car.live) {                                 // +0xdc
        if (g.lanes[li].live) car_set_live(g, ci, true);   // owner vf1 0x40e390
        if (!car.live) {                             // destination lane not live
            lane_unlink(g, ci);                      // 0x40ca40
            if (car.path && car.dir < 0) {           // +0xc8 (lane owner)
                // INVENTED: bounds check (the exe dereferences the lane pointer).
                if (car.path_lane < g.lanes.size())
                    lane_detach(g, car.path_lane, ci);    // 0x40e710 (dir ignored)
            }
            // 0x4883e0 ctCross__vf4: no-op RET 8 (writes and calls nothing);
            // the car left the cross list in lane_unlink 0x40ca40 above.
            car.path_lane = (uint32_t)li;            // +0xc4
            car.path = path;                         // +0xc8
            car.dir = dir;                           // +0xcc
            car.kd0 = 0; car.prev_path = nullptr;    // +0xd0, +0xd4
            float len = 0.0f;
            if (path) {
                if (dir < 0) len = path->length;     // +0x38
                // shortcut: a cross owner (dir >= 0) would read Segment::len
                // (+0x30); every src call here passes a lane and dir -1.
            }
            const float pos = len - car.k20;         // +0x20
            car.pos = pos;                           // +0x88
            if (car.k14 > pos) car.pos = car.k14;    // +0x14
            if (path) {
                lane_cache_tail(g, li, ci);          // owner vf2 0x411f00
                g.lanes[li].cars.push_back(ci);      // vf3 append branch of 0x40cbd0
                car.lane = (uint32_t)li;
                lane_attach(g, li, ci);              // 0x40cbd0
            }
            return;
        }
    }
    lane_cache_tail(g, li, ci);                      // owner vf2 0x411f00
    car.next_lane_a = (uint32_t)li;                  // +0xb8
    car.next_lane_b = path;                          // +0xbc
    car.kc0 = dir;                                   // +0xc0
}

/* Cross-vtable slots (image bytes at 0x5936bc ctCross, 0x59377c ctCrossEq,
 * 0x5937c0 ctCrossLight; all three share +0x0c..+0x18):
 *   +0x00 vf0 0x40dbd0  not traced here
 *   +0x04 vf1 0x40e380  live_refs test (notes/40 §4.6)
 *   +0x08 vf2 0x477320  empty RET 4 (not re-verified)
 *   +0x0c vf3 0x40cb60  tail append -> cross_attach
 *   +0x10 vf4 0x4883e0  no-op RET 8 (writes and calls nothing)
 *   +0x14 vf5 0x46acd0  XOR EAX,EAX; RET 8 -> returns int 0
 *   +0x18 vf6 0x40dd00  max of the two link lanes' +0x98 -> cross_vf6
 * The base ctor slots +0x1c/+0x20/+0x28 are the purecall stub 0x56ee6f; ctCrossEq
 * overrides +0x1c..+0x2c with the route vfs and its dtor, ctCrossLight with
 * 0x415f40/0x415570 (class names are Ghidra's, the addresses are from the image).
 */
// 0x40cb60 ctCross__vf3 (ECX = cross; args Car *car, uint32_t path): tail-append
// the car to the cross's +8/+0xc list, hand it to the exit lane with 0x40f950,
// then sync its instance with the cross's bound object (+0x44).
void cross_attach(Graph &g, uint32_t cross_node, uint32_t ci, size_t idx)
{
    Cross *c = cross_by_node(g, cross_node, nullptr);
    // INVENTED: a node id that is not a cross returns (the exe calls a vtable).
    if (!c) return;
    Car &car = g.cars[ci];
    c->cars.push_back(ci);                        // tail append; empty: head = car
    car.cross_node = cross_node;                  // +0xc owner
    // INVENTED: the exe indexes paths and links unchecked.
    if (idx >= c->paths.size()) return;
    const uint32_t b = c->paths[idx].b;           // +0x1c + idx*0x60 + 1
    if (b >= c->links.size()) return;
    Lane *lane = lane_at(g, c->links[b].node);    // +0x14 + b*8
    if (lane)                                     // INVENTED null guard
        car_set_ref(g, ci, (size_t)(lane - g.lanes.data()), &lane->path, -1);  // 0x40f950
    car_sync_instance(g, ci, c->object);          // 0x410680, cross+0x44
}

// 0x40daa0 TrafficCar__distanceOnRef (defined below lane_end_move).
bool car_distance_on_ref(Graph &g, uint32_t ci, const Path *ref1, const Path *ref2,
                         float *out);

// 0x40e3a0 ctLane__room (ECX = lane; arg float need): room at the lane tail.
bool lane_room(Graph &g, const Lane &lane, float need)
{
    if (lane.cars.empty()) return true;                  // +0xc tail, no car
    const uint32_t tail = lane.cars.back();
    float dist = 0.0f;
    if (!car_distance_on_ref(g, tail, &lane.path, nullptr, &dist))   // 0x40daa0
        return false;
    const float room = lane.path.length - (g.cars[tail].k20 + dist);  // +0x74, +0x20
    return !(need > room);                               // COMISS; JBE -> 1
}

// 0x40f900 ctCross__setPathRef -> 0x40f950 car_set_ref with a cross owner.
void cross_path_ref(Graph &g, uint32_t ci, uint32_t cross_node, Cross &c, size_t i)
{
    Car &car = g.cars[ci];
    if (car.live == 0) {
        if (c.live_refs != 0) car_set_live(g, ci, true);   // cross vf1 0x40e380
        // shortcut: 0x40f950's immediate branch for a cross owner (cross vf2
        // 0x477320 is an empty RET 4; vf3 0x40cb60 is cross_attach; vf4 0x4883e0
        // is a no-op RET 8). Unreachable from 0x4157a0, which runs live_refs > 0.
    }
    car.next_lane_a = cross_node;                        // +0xb8 owner
    car.next_lane_b = reinterpret_cast<const Path *>(&c.paths[i].seg);  // +0xbc
    car.kc0 = (int32_t)i;                                // +0xc0 idx
}

// 0x4157a0 ctCross__routeLive (ECX = cross): live_refs > 0; the car takes a
// random pick among the best-scored cross paths.
bool cross_route_live(Graph &g, uint32_t cross_node, Cross &c, uint32_t ci, float t,
                      uint32_t link)
{
    if (t > 5.2f) return false;                          // 0x593800
    Car &car = g.cars[ci];
    const float need = car.k20 + car.k14;
    const bool behaviour1 = car.behaviour == 1;          // +0x160
    const int32_t prio = (double)t >= 0.1 ? (int32_t)c.cycle : 0;   // 0x593568 double 0.1
    std::vector<size_t> cand;                            // candidates[128] in the exe
    int32_t best = 0;
    for (size_t i = 0; i < c.paths.size(); i++) {
        if (c.path_state[i].block != 0) continue;        // +4
        if (c.paths[i].a != link) continue;              // byte vs the full 32-bit link
        if ((int32_t)c.paths[i].c < prio) continue;      // ushort, signed compare
        // INVENTED: link-index and node bounds (the exe indexes both unchecked).
        if (c.paths[i].b >= c.links.size()) continue;
        Lane *lane = lane_at(g, c.links[c.paths[i].b].node);
        if (!lane) continue;
        if (lane->halted) continue;                      // +0xc4
        const float cap = lane->kcc * lane->k9c - lane->kc8;  // +0xcc*+0x9c-+0xc8
        if (!(cap > need)) {                             // MULSS/SUBSS then COMISS
            c.path_state[i].block = 0x3e9;               // 1001
            continue;
        }
        int32_t score = lane_room(g, *lane, need) ? 1 : 0;     // 0x40e3a0
        if (lane->live == behaviour1) score += 1;        // lane vf1 0x40e390 = +0xa8
        if (((uint32_t)car.k40 & lane->ka0) != 0) score += 2;
        if (cand.empty() || score > best) {              // CMOVGE: higher score resets
            best = score;
            cand.assign(1, i);
        } else if (score == best) {
            cand.push_back(i);                           // ties are all kept
        }
    }
    if (!cand.empty()) {
        const size_t pick = cand[(msvc_rand() & 0x7fff) % cand.size()];   // 0x571110
        cross_claim_path(c, pick);                       // 0x415b40
        cross_path_ref(g, ci, cross_node, c, pick);      // 0x40f900
        return true;
    }
    // Stale-block sweep, only when no path scored.
    for (size_t i = 0; i < c.paths.size(); i++) {
        if (c.paths[i].a != link) continue;
        if ((int32_t)c.paths[i].c < prio) continue;
        if ((int32_t)c.paths[i].c <= c.path_state[i].block + 5) continue;
        if (c.paths[i].b >= c.links.size()) continue;    // INVENTED bounds (exe unchecked)
        Lane *lane = lane_at(g, c.links[c.paths[i].b].node);
        if (!lane || lane->halted) continue;             // INVENTED null guard
        cross_claim_path(c, i);                          // 0x415b40
    }
    return false;
}

// 0x4159d0 ctCross__routeDying (ECX = cross): live_refs < 0; the car takes the
// cheapest exit lane (strict min, ties keep the first).
bool cross_route_dying(Graph &g, Cross &c, uint32_t ci, uint32_t link)
{
    const Car &car = g.cars[ci];
    const float need = car.k20 + car.k14;
    int32_t best = -1;
    float best_score = 0.0f;
    for (size_t i = 0; i < c.paths.size(); i++) {
        if (c.path_state[i].block != 0) continue;        // +4
        if (c.path_state[i].count != 0) continue;        // +0 (both zero)
        if (c.paths[i].a != link) continue;
        if (c.paths[i].b >= c.links.size()) continue;    // INVENTED bounds (exe unchecked)
        Lane *lane = lane_at(g, c.links[c.paths[i].b].node);
        if (!lane) continue;                             // INVENTED null guard
        if (!lane_room(g, *lane, need)) continue;        // 0x40e3a0, before halted
        if (lane->halted) continue;                      // +0xc4
        float score = lane->kc8 / lane->kcc;             // DIVSS +0xc8/+0xcc
        if (((uint32_t)car.k40 & lane->ka0) != 0)
            score = (float)((double)score - 100.0);      // 0x593730 double 100.0
        if (best == -1 || score < best_score) { best = (int32_t)i; best_score = score; }
    }
    if (best == -1) return false;
    Lane *lane = lane_at(g, c.links[c.paths[best].b].node);   // valid: checked above
    car_set_ref(g, ci, (size_t)(lane - g.lanes.data()), &lane->path, -1);   // 0x40f950
    return true;
}

// 0x40e590 ctCross__routeIdle (ECX = cross; args Car* car, uint32_t link):
// live_refs == 0; cheapest link lane with room on another road.
bool cross_route_idle(Graph &g, Cross &c, uint32_t ci, uint32_t link)
{
    const Car &car = g.cars[ci];
    const float need = car.k20 + car.k14;
    if (link >= c.links.size()) return false;            // INVENTED: exe indexes unchecked
    const Lane *l0 = lane_at(g, c.links[link].node);
    if (!l0) return false;                               // INVENTED null guard
    const uint32_t road0 = l0->road;                     // +0x10
    int32_t best = -1;
    float best_score = 0.0f;
    for (size_t i = 0; i < c.links.size(); i++) {
        if (c.links[i].flag != 0) continue;              // +4 byte
        Lane *lane = lane_at(g, c.links[i].node);
        if (!lane) continue;                             // INVENTED null guard
        if (lane->road == road0) continue;
        if (!lane->cars.empty()) {                       // +0xc tail
            const uint32_t tail = lane->cars.back();
            float dist = 0.0f;
            if (!car_distance_on_ref(g, tail, &lane->path, nullptr, &dist))  // 0x40daa0
                continue;
            if (need > lane->path.length - (g.cars[tail].k20 + dist)) continue;
        }
        if (lane->halted) continue;                      // +0xc4
        float score = lane->kc8 / lane->kcc;             // +0xc8 / +0xcc
        if (((uint32_t)car.k40 & lane->ka0) != 0)
            score = (float)((double)score - 100.0);      // 0x593730 double 100.0
        if (best == -1 || score < best_score) { best = (int32_t)i; best_score = score; }
    }
    if (best == -1) return false;
    Lane *lane = lane_at(g, c.links[best].node);
    car_set_ref(g, ci, (size_t)(lane - g.lanes.data()), &lane->path, -1);   // 0x40f950
    return true;
}

// 0x40e400 ctCross__route (ECX = cross; args Car* car, float t, uint32_t link):
// pick the cross vf by the sign of live_refs; return its result.
bool cross_dispatch(Graph &g, uint32_t cross_node, uint32_t ci, float t, uint32_t link)
{
    Cross *c = cross_by_node(g, cross_node, nullptr);
    // INVENTED: a road/lane node id is not a cross (the exe calls a vtable on it).
    if (!c) return false;
    if (c->k40 > g.sim_time) return false;               // 0x40e40d COMISS / 0x40e414 JBE; writer 0x40de70
    const int32_t r = c->live_refs;                      // +0x38
    if (r > 0) return cross_route_live(g, cross_node, *c, ci, t, link);   // vt+0x1c
    if (r < 0) return cross_route_dying(g, *c, ci, link);                 // vt+0x20
    return cross_route_idle(g, *c, ci, link);                             // vt+0x24
}

// 0x40e460 ctLane__laneEndMove (ECX = lane, args: Car* car, float t,
// uint32_t link = lane.k38): move a car waiting at the lane end to a next lane.
bool lane_end_move(Graph &g, size_t li, uint32_t ci, float t)
{
    Lane &lane = g.lanes[li];
    const Car &car = g.cars[ci];
    if (car.k128 != 0) return false;                 // +0x128
    // INVENTED: a non-lane node id is treated as null (the exe dereferences it).
    const Lane *next0 = g.lane_by_node(lane.node88[0]);   // +0x88
    const Lane *next1 = g.lane_by_node(lane.node88[1]);   // +0x8c
    if (next0 == nullptr && next1 == nullptr) {
        // INVENTED: null/k18 guards (the exe dereferences the road unchecked).
        Road *rd = road_by_node(g, lane.road);
        uint32_t cn = 0;
        if (rd && lane.k18 < 3) cn = rd->node[2 - lane.k18];
        return cross_dispatch(g, cn, ci, t, lane.k38);   // 0x40e400
    }
    if (t > car_target_speed(g, car) / car.k38) return false;   // 0x40dc00, +0x38
    if (!lane.live && t > 0.0f) return false;        // +0xa8
    const float need = car.k20 + car.k14;            // +0x20 + +0x14
    const Lane *chosen = next0;
    if (next0 == nullptr) {
        chosen = next1;
    } else if (next1 != nullptr) {                   // both present
        const float cap0 = next0->kcc * next0->k9c - next0->kc8;  // +0xcc*+0x9c-+0xc8
        const float cap1 = next1->kcc * next1->k9c - next1->kc8;
        if (cap0 <= need) {
            if (cap1 <= need) return false;          // neither fits
            chosen = next1;                          // only next1 fits
        } else if (cap1 > need && cap1 >= cap0) {    // larger room wins, tie -> next1
            chosen = next1;
        }
    }
    const size_t cj = (size_t)(chosen - g.lanes.data());
    car_set_ref(g, ci, cj, &chosen->path, -1);       // 0x40f950
    return true;
}

// 0x40c270 ctLeaderSlot__sort: min-heap build + heapsort, dist only. Empty
// slots (car == 0, the exe's null) compare smallest, so the result is
// descending dist with the empty slots last.
void slot_sort(LeaderSlot *a, int n)
{
    for (int i = 1; i < n; i++) {                          // 0x40c284..0x40c2ec
        const LeaderSlot e = a[i];
        int j = i;
        while (j != 0) {
            const int p = (j - 1) / 2;
            if (a[p].car == 0) break;                      // 0x40c2b1
            if (e.car != 0 && !(e.dist < a[p].dist)) break;   // 0x40c2b8..0x40c2c5
            a[j] = a[p];
            j = p;
        }
        a[j] = e;
    }
    for (int end = n - 1; end > 0; end--) {                // 0x40c2ee..0x40c3d3
        const LeaderSlot temp = a[0];
        int i = 0, left = 1;
        while (left < end) {                               // 0x40c30a, 0x40c3a6
            const int right = 2 * i + 2;
            int child;
            if (right >= end || a[left].car == 0 ||
                (a[right].car != 0 && a[left].dist <= a[right].dist))
                child = left;                              // 0x40c319..0x40c33e
            else
                child = right;
            if (a[end].car == 0 ||
                (a[child].car != 0 && a[end].dist <= a[child].dist))
                break;                                     // 0x40c343..0x40c38c
            a[i] = a[child];
            i = child;
            left = 2 * i + 1;
        }
        a[i] = a[end];
        a[end] = temp;                                     // 0x40c3b9..0x40c3c7
    }
}

// 0x40daa0 TrafficCar__distanceOnRef (ECX = leader car): distance of the
// leader's position measured on ref1 (or ref2), 0 = the refs do not match.
bool car_distance_on_ref(Graph &g, uint32_t ci, const Path *ref1, const Path *ref2,
                         float *out)
{
    car_step(g, ci);                                       // 0x40daa6
    const Car &c = g.cars[ci];
    if (c.path == ref1) { *out = c.pos; return true; }
    if (c.prev_path == ref1) {
        *out = c.pos - car_ref_length(c.path, c.dir);      // 0x40dad2..0x40db04
        return true;
    }
    if (c.next_lane_b == ref1) {
        *out = c.pos + car_ref_length(c.next_lane_b, c.kc0);   // 0x40db18..0x40db46
        return true;
    }
    if (ref2) {
        if (c.path == ref2) {
            *out = c.pos - car_ref_length(c.path, c.dir);  // 0x40db5f..0x40db74
            return true;
        }
        if (c.prev_path == ref2) {
            *out = (c.pos - car_ref_length(c.path, c.dir)) -
                   car_ref_length(c.prev_path, c.prev_dir);    // 0x40db80..0x40dba6
            return true;
        }
        if (c.next_lane_b == ref2) { *out = c.pos; return true; }  // 0x40dbae
    }
    return false;                                          // 0x40dbb6
}

// 0x40eff0 TrafficCar__checkObstacle (ECX = car, arg = the obstacle entry's Car):
// closest-approach test of the obstacle body against the car's front direction;
// head-inserts the car onto the event-1 list T+0xc8 (drained in Traffic__step).
void car_check_obstacle(Graph &g, uint32_t ci, const Car &o)
{
    Car &car = g.cars[ci];
    float dx = car.front_x - car.world[0];          // +0x98 - +0x8c
    float dz = car.front_z - car.world[2];          // +0x9c - +0x94
    const float l = sqrtf(dx * dx + dz * dz);       // the y direction is 0
    if (l != 0.0f) {                                // 0x40f057 DIVSS, then MULSS
        const float inv = 1.0f / l;
        dx *= inv;
        dz *= inv;
    }
    const float back = -car.speed;                  // 0x40f04c -[+0xb0]
    float rx = o.k108[0] - car.world[0];
    float ry = o.k108[1] - car.world[1];
    float rz = o.k108[2] - car.world[2];
    const float vx = o.k114[0] - dx * back;
    const float vy = o.k114[1];
    const float vz = o.k114[2] - dz * back;
    const float dot = vx * rx + vy * ry + vz * rz;
    if (dot > 0.0f) return;
    const float dist = sqrtf(rx * rx + ry * ry + rz * rz);
    const float spd = sqrtf(vx * vx + vy * vy + vz * vz);
    const float k = -(((dot / (spd * dist)) * dist) / spd);   // keep this order
    rx += vx * k;
    ry += vy * k;
    rz += vz * k;
    if (o.k120 + 1.0f < rx * rx + ry * ry + rz * rz) return;  // entry +0x34
    if (car.event_next == 0) {                      // +0x10
        car.event_next = g.event1;                  // T+0xc8
        g.event1 = ci + 1;
    }
}

// 0x4107c0 TrafficCar__step step 3 (0x410a57..0x410fbf): the three obstacle-list
// passes (cur owner, next owner shifted by the next length, lane-change target);
// fills free / leader_speed and may queue an event-1 check.
void car_scan_obstacles(Graph &g, uint32_t ci, float dt, float ego_len, float *free,
                        float *leader_speed)
{
    Car &car = g.cars[ci];
    for (int pass = 0; pass < 3; pass++) {
        const std::vector<uint32_t> *obs;
        float base_pos = 0.0f;
        if (pass == 0) {
            // SERVICE MISSING: a cross owner's obstacle list (exe owner+4) is not
            // modelled; no obstacle list exists while the current ref is a cross.
            if (car.dir >= 0) continue;
            // INVENTED: bounds check (the exe dereferences the owner pointer).
            if (car.path_lane >= g.lanes.size()) continue;
            base_pos = car.pos;                          // +0x88
            obs = &g.lanes[car.path_lane].obstacles;     // +0xc4 owner
        } else if (pass == 1) {
            if (!car.next_lane_b) continue;              // +0xbc
            // shortcut: a cross next ref (kc0 >= 0, Segment length +0x30, cross
            // owner) has no obstacle list in src.
            if (car.kc0 >= 0) continue;
            base_pos = car.pos + car.next_lane_b->length;   // +0x38
            // INVENTED: bounds check (the exe dereferences the owner pointer).
            if (car.next_lane_a >= g.lanes.size()) continue;
            obs = &g.lanes[car.next_lane_a].obstacles;   // +0xb8 owner
        } else {
            if (!car.k124) continue;                     // +0x124
            if (!(car.kfc > -0.5f)) continue;            // 0x593744
            base_pos = car.kfc;                          // +0xfc
            // src k124 is a raw side-lane node id (lane_attach 0x40cbd0).
            const Lane *tl = g.lane_by_node(car.k124);   // INVENTED bounds (exe pointer)
            if (!tl) continue;
            obs = &tl->obstacles;
        }
        for (uint32_t oi : *obs) {                       // exe lane+4 head, node+4 next
            const Car &o = g.cars[oi];
            const float s = o.kfc;                       // entry +0x10
            if (s <= -0.5f) {                            // free body
                float p[3], dir[3];
                car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral, p);  // 0x4133f0
                car_ref_dir(car.path, car.dir, car.pos, dir);                    // 0x412ed0
                const float e0 = p[0] - o.k108[0];
                const float e1 = p[1] - o.k108[1];
                const float e2 = p[2] - o.k108[2];
                const float dot = dir[0] * e0 + dir[1] * e1 + dir[2] * e2;
                if (!(dot > 0.0f)) continue;
                const float lat = -dir[2] * e0 + dir[1] * e1 + dir[0] * e2;
                if (fabsf(lat) - o.k120 >= 2.0f) continue;           // 0x593728
                float d = dot - (o.k120 + car.k14 + ego_len);
                float rear = ego_len;
                if (d < 0.0f) {
                    rear = (-ego_len <= d) ? d + ego_len : 0.0f;
                    d = 0.0f;
                }
                if (*free <= d) continue;
                const float closing = -(dir[0] * o.k114[0] + dir[1] * o.k114[1] +
                                        dir[2] * o.k114[2]);
                float brake_d = d;
                // 0x410e96: speed <= 1 -> leader; 0x410ee8: d < 1e-5 or
                // closing < 0 or ratio < -k38 -> the bd block.
                if (car.speed <= 1.0f ||
                    (d >= 1e-5f && closing >= 0.0f &&
                     (closing * closing - car.speed * car.speed) / (d + d) >= -car.k38)) {
                    *leader_speed = fmaxf(closing, 0.0f);
                } else {
                    const float bd = (car.speed <= car.k38 * dt)
                        ? car.speed * car.speed / (car.k38 + car.k38)
                        : car.speed * dt - dt * dt * car.k38 * 0.5f;
                    const float v2 = car.speed - car.k38 * dt;
                    const float bd_raw = bd;
                    brake_d = fmaxf(bd, d);
                    *leader_speed = fmaxf(v2, 0.0f);
                    if (rear + d <= bd_raw && closing <= v2)   // 0x410c3f addss
                        car_check_obstacle(g, ci, o);    // 0x40eff0 (unconditional here)
                }
                *free = fminf(*free, brake_d);
            } else {                                     // on-path
                if (base_pos <= s) continue;             // 0x410b3e JBE
                float d = base_pos - (o.k120 + s + car.k14 + ego_len);
                float rear = ego_len;
                if (d < 0.0f) {
                    rear = (-ego_len <= d) ? d + ego_len : 0.0f;
                    d = 0.0f;
                }
                if (*free <= d) continue;                // 0x410b82 JBE
                const float os = o.k100;                 // entry +0x14
                float brake_d = d;
                // 0x410b9d JBE (speed <= 1 -> leader), 0x410bae/0x410bb8 JA
                // (d < 1e-5 | os < 0 -> bd), 0x410bdf JBE (ratio >= -k38).
                if (car.speed <= 1.0f ||
                    (d >= 1e-5f && os >= 0.0f &&
                     (os * os - car.speed * car.speed) / (d + d) >= -car.k38)) {
                    *leader_speed = fmaxf(os, 0.0f);
                } else {
                    const float bd = (car.speed <= car.k38 * dt)
                        ? car.speed * car.speed / (car.k38 + car.k38)
                        : car.speed * dt - dt * dt * car.k38 * 0.5f;
                    const float v2 = car.speed - car.k38 * dt;
                    const float bd_raw = bd;
                    brake_d = fmaxf(bd, d);
                    *leader_speed = fmaxf(v2, 0.0f);
                    if (rear + d <= bd_raw && os <= v2) {     // 0x410c3f addss
                        if (o.k104) {                    // 0x410c63 CMP byte
                            car_check_obstacle(g, ci, o);
                        } else {
                            // shortcut: the k104 writer is not traced, so src only
                            // reaches this branch with k104 == 0.
                            brake_d = rear;   // XMM7 at 0x410c7b: the pre-+d value
                            *leader_speed = fmaxf(os, 0.0f);
                        }
                    }
                }
                *free = fminf(*free, brake_d);
            }
        }
    }
}

// 0x41180d..0x4118b0, inlined in TrafficCar__step: unlink the car's obstacle node
// (+0xec/+0xf0 links, +0xf8 owner) from its lane's list.
void obstacle_unlink(Graph &g, uint32_t ci)
{
    Car &car = g.cars[ci];
    const uint32_t owner = car.ob_owner;
    if (owner != kNoneLane) {
        if (owner < g.lanes.size()) {                    // INVENTED bounds (exe pointer)
            std::vector<uint32_t> &list = g.lanes[owner].obstacles;
            std::vector<uint32_t>::iterator it = std::find(list.begin(), list.end(), ci);
            if (it != list.end()) list.erase(it);
        }
        car.ob_owner = kNoneLane;
    }
    car.ob_next = 0;
    car.ob_prev = 0;
}

// 0x411801..0x411859, inlined in TrafficCar__step step 8: head-insert the car's
// obstacle node into lane li's list (exe owner+4 head).
void obstacle_link(Graph &g, uint32_t li, uint32_t ci)
{
    Car &car = g.cars[ci];
    // INVENTED: bounds check (the exe dereferences the target lane pointer).
    if (li >= g.lanes.size()) return;
    std::vector<uint32_t> &list = g.lanes[li].obstacles;
    list.insert(list.begin(), ci);
    car.ob_owner = li;
}

// 0x40fe50 ctLane__vf5 (ECX = lane, args float f, Car *car): pick a side lane
// (node88[2]/[3]) the car may move to at gap f; returns the side-lane node id
// or 0.
uint32_t lane_side_pick(const Graph &g, const Lane &lane, uint32_t ci, float f)
{
    const Car &car = g.cars[ci];
    const float need = car.k24 + car.k14 + 12.666666f;     // 0x593740
    if (lane.k34 == 0xffffffffu) return 0;
    if (!(need < car.pos)) return 0;                       // +0x88
    // INVENTED: the exe walks car+4 (prev in the intrusive lane list); src
    // lane.cars is ascending pos, so the element before ci is the car ahead.
    const Car *prev = nullptr;
    std::vector<uint32_t>::const_iterator it =
        std::find(lane.cars.begin(), lane.cars.end(), ci);
    if (it != lane.cars.end() && it != lane.cars.begin()) prev = &g.cars[*(it - 1)];
    if (prev && !(prev->pos + prev->k20 -
                  prev->speed * prev->speed / prev->k38 < car.pos - need))
        return 0;
    if (lane.node88[2] == 0 && lane.node88[3] == 0) return 0;
    float p[3];
    car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral, p);   // 0x40dde0
    bool ok[2] = {false, false};
    for (int i = 0; i < 2; i++) {                          // 0x40ff31..0x41019a
        // INVENTED: bounds check (the exe compares pointers).
        const Lane *side = g.lane_by_node(lane.node88[2 + i]);
        if (!side || side->halted) continue;               // 0x40ff3b, +0xc4
        float d = 0, lat = 0;
        if (!path_project_strict(side->path, p[0], p[2], &d, &lat)) continue;
        if (!(need < d)) continue;
        // Nearest obstacle entry of the side lane's +4 list: before = largest
        // kfc < d, after = smallest kfc > d, both with kfc > -0.5 (0x593744).
        // The exe walks head -> next (LIFO newest-first); strict comparisons
        // keep the first entry on a tie. INVENTED: vector for the intrusive node.
        const Car *before = nullptr, *after = nullptr;
        for (uint32_t oi : side->obstacles) {
            const Car &o = g.cars[oi];
            if (!(o.kfc > -0.5f)) continue;
            if (o.kfc < d) {
                if (!before || before->kfc < o.kfc) before = &o;
            } else if (o.kfc > d) {
                if (!after || o.kfc < after->kfc) after = &o;
            }
        }
        // side.cars (ascending pos): c = the last car with pos < d, scanning
        // from the tail (exe car+0xc via car+4); nxt = the car after it, else
        // the head (exe lane+8).
        const std::vector<uint32_t> &list = side->cars;
        const Car *c = nullptr, *nxt = nullptr;
        for (size_t k = list.size(); k-- > 0;) {
            if (g.cars[list[k]].pos < d) {
                c = &g.cars[list[k]];
                if (k + 1 < list.size()) nxt = &g.cars[list[k + 1]];
                break;
            }
        }
        if (!c && !list.empty()) nxt = &g.cars[list.front()];
        float brake_car = 0.0f;
        if (nxt) {                                         // 0x410001..0x410087
            const float inv = 1.0f / nxt->k38;
            float v = nxt->speed;
            float t = (v * v - car.speed * car.speed) * inv;
            if (!(0.0f <= t)) t = 0.0f;
            v -= 2.0f;
            if (!(0.0f < v)) v = 0.0f;
            brake_car = (nxt->pos - nxt->k14) - (v * 0.5f + 1.0f) * nxt->k24 - t;
        }
        float brake_obs = 0.0f;
        if (after)                                         // 0x410097..0x4100ea
            brake_obs = (after->kfc - after->k120) -
                        fmaxf(0.0f, (after->k100 * after->k100 -
                                     car.speed * car.speed) * 0.16666667f);   // 0x593700
        if (c && !(c->pos + c->k20 - c->speed * c->speed / c->k38 < d - need &&
                   c->pos + c->k20 < d - f)) continue;     // 0x4100f6..0x41013a
        if (nxt && !(car.k20 + d < brake_car)) continue;   // 0x410143..0x410153
        if (before && !(d - fmaxf(need, f) > before->k120 + before->kfc)) continue;
        if (after && !(car.k20 + d < brake_obs)) continue; // 0x410176..0x410186
        ok[i] = true;                                      // 0x410188
    }
    if (ok[0] && ok[1]) return lane.node88[2 + (msvc_rand() & 1)];   // 0x4101ae
    if (ok[0]) return lane.node88[2];                      // 0x4101c7
    if (ok[1]) return lane.node88[3];                      // 0x4101d9
    return 0;                                              // 0x4101e9
}

// 0x40fde0 TrafficCar__startLaneChange (ECX = car, one float arg): pick the
// target side lane and set k124/k128/k12c; a no-op when vf5 finds none.
void car_start_lane_change(Graph &g, uint32_t ci, float f)
{
    Car &car = g.cars[ci];
    // +0xdc, +0xbc, +0xcc: the current owner must be a lane (the exe uses owner
    // vf5 0x46acd0 for a cross, untraced).
    if (car.live == 0 || car.next_lane_b != nullptr || car.dir >= 0) return;
    // INVENTED: bounds check (the exe dereferences the owner pointer).
    if (car.path_lane >= g.lanes.size()) return;
    const Lane &lane = g.lanes[car.path_lane];             // car+0xc4 owner
    const uint32_t l = lane_side_pick(g, lane, ci, f);
    if (l == 0) return;
    car.k124 = l;                                          // 0x40fe1b
    car.k12c = 0.0f;                                       // 0x40fe27
    // (lane+0x90 != l) * 2 - 1
    car.k128 = (lane.node88[2] == l) ? (uint32_t)-1 : 1u;  // 0x40fe21..0x40fe3b
}

// 0x4107c0 TrafficCar__step (ECX = car): entry gate, step 1 free distance, step 2
// leaders, step 3 obstacle scan, step 4 lane-change decision, step 5 speed
// integration, step 6 ref hand-over, step 7 lane-change completion, step 8
// target-lane obstacle registration.
void car_step(Graph &g, uint32_t ci)
{
    Car &car = g.cars[ci];
    // INVENTED: car.serial == 0 stands in for the exe's null lane pointer
    // (car+0xc; src lane ids are indices where 0 is a valid lane).
    if (car.serial == 0) return;
    const float t = g.sim_time;            // +0x84
    if (!(t > car.e0)) return;             // 0x4107eb COMISS e0,t
    const float dt = t - car.e0;
    car.e0 = t;

    // step 1: free distance (0x41080a..0x410871).
    float d0 = car.pos - car.k14;          // +0x88, +0x14
    if (car.next_lane_b == nullptr) {      // +0xbc
        d0 = std::max(d0 - g.cross_stop_offset, 0.0f);   // +0x3140, 0x593580
    } else if (car.kc0 < 0) {              // +0xc0, lane ref
        d0 += car.next_lane_b->length;     // +0x38
    } else {
        // 0x41082b: kc0 >= 0 = cross path, length is the Segment's +0x30.
        d0 += reinterpret_cast<const Segment *>(car.next_lane_b)->len;
    }
    float target = car_target_speed(g, car);   // 0x41086c -> 0x40dc00
    float leader_speed = -1.0f;                // [EBP-0x20], 0x5935ac
    float free = d0;                           // [EBP-0x8]

    // step 2: leader slots and the nearest gap (0x41087b..0x410a55).
    const float brake =                              // 0x410888
        (std::max(car.speed - 2.0f, 0.0f) * 0.5f + 1.0f) * car.k24;
    uint32_t leader = 0;
    if (car.dir == -1) {                             // 0x4108b9
        // INVENTED: the exe reads car+4 (prev in the intrusive lane list); src
        // lane.cars is ascending pos, so the element before ci is ahead.
        if (car.lane < g.lanes.size()) {
            const std::vector<uint32_t> &list = g.lanes[car.lane].cars;
            std::vector<uint32_t>::const_iterator it =
                std::find(list.begin(), list.end(), ci);
            if (it != list.end() && it != list.begin()) leader = *(it - 1) + 1;
        }
    }
    if (!leader && car.next_lane_b) {
        // shortcut: the exe takes the leader from the next ref's owner vf0
        // (0x4108db indirect vtable call); not ported.
    }
    if (leader) car_note_leader(g, ci, leader, 0);   // 0x4108e1..0x4108e6
    // shortcut: target-lane marking 0x4108eb..0x41091b and the flag != 0 refresh
    // below (0x410948 branch) are not ported; car+0x124 is now set by the step-4
    // decision (0x40fde0) and car+0xfc by step 8 (0x41179c).
    for (int i = 0; i < 4; i++) {
        LeaderSlot &slot = car.leaders[i];
        if (!slot.car) continue;                     // 0x410935..0x410937
        if (slot.serial != g.cars[slot.car - 1].serial) {   // 0x41093d..0x410942
            slot.serial = slot.car = 0; slot.flag = 0;      // 0x4109cc
            continue;
        }
        if (slot.flag != 0) {
            // shortcut: target-lane refresh not modelled (see above).
        } else {
            if (!car_distance_on_ref(g, slot.car - 1, car.path, car.next_lane_b,
                                     &slot.dist)) {  // 0x41099c..0x4109b6
                slot.serial = slot.car = 0; slot.flag = 0;
                continue;
            }
            slot.dist += g.cars[slot.car - 1].k20;   // 0x4109b8..0x4109c5
        }
    }
    slot_sort(car.leaders, 4);                       // 0x4109ec
    if (car.leaders[0].car) {                        // 0x4109f7
        LeaderSlot &slot = car.leaders[0];
        if (slot.serial != g.cars[slot.car - 1].serial) {   // 0x410a0b..0x410a0f
            slot.serial = slot.car = 0; slot.flag = 0;      // 0x410a11
        } else {
            const float gap = (car.pos - car.k14) - brake - slot.dist;  // 0x410a24..0x410a35
            if (gap < free) {                                    // 0x410a3a..0x410a52
                free = gap;
                leader_speed = g.cars[slot.car - 1].speed;
            }
        }
    }

    // step 3: obstacle scan (0x410a57..0x410fbf).
    car_scan_obstacles(g, ci, dt, brake, &free, &leader_speed);
    // 0x410fc8..0x410fdd: the clamp is unconditional; 0x410fc1 only sets the
    // flags that 0x410fe2 JNZ tests for the lane-change branch.
    free = fminf(fmaxf(free, 0.0f), d0);
    // step 4: lane-change decision (0x410fe2..0x411105).
    if (car.k128 == 0) {                             // 0x410fe2 JNZ 0x41110a
        if (free < d0 * 0.9f &&                      // 0x59370c
            leader_speed < car.speed &&              // 0x410ff5..0x411005
            ((leader_speed * leader_speed -
              (target * 0.8f) * (target * 0.8f)) * 0.5f) / (free + 1.0f)
                < -car.accel)                        // 0x411007..0x411047
            // 0x411049..0x41105e: XMM6 = brake, XMM4 = free, + k14, + 1.0
            car_start_lane_change(g, ci, brake + free + car.k14 + 1.0f);
        if (car.k128 == 0 && car.live) {             // 0x411068, 0x411075
            const float lo = target < 0.01f ? 0.01f : 0.001f;   // 0x593584/0x5935d0
            car.k12c = powf(0.01f, dt) * (car.k12c - lo) + lo;  // 0x4110a6..0x4110cb
            if (car.next_lane_b == nullptr) {        // 0x4110d3
                const float r = (float)(msvc_rand() & 0x7fff) *
                                3.0517578e-05f;      // 0x5936fc
                if (r < car.k12c)                    // 0x4110d5..0x4110f9
                    car_start_lane_change(g, ci, 0.0f);          // 0x411105
            }
        }
    }

    // step 5: speed integration (live block 0x41110a..0x411448, simple
    // block 0x411455..0x4114fb; the exe takes the simple block whenever
    // car.live == 0, whatever k128 is: 0x411075 JZ 0x411455 at the top and
    // the k128 != 0 jump re-tests +0xdc then JZ 0x411455 at 0x411111).
    float travelled, new_speed;
    if (car.live == 0) {
        // simple block 0x411455..0x4114fb
        const float limit = 1.5749999f - car.k28;   // 0x593718 = 0x3fc99999
        car.kac = 0.0f;                             // +0xac
        if (fabsf(car.lateral) > limit) {           // +0xa8
            if (car.lateral >= 0.0f) {
                float v = limit;
                if (v <= 0.0f) v = 0.0f;
                car.lateral = v;
            } else {
                float v = car.k28 - 1.5749999f;
                if (v >= 0.0f) v = 0.0f;
                car.lateral = v;
            }
        }
        travelled = target * dt;
        car.kb4 = 0.0f;                             // +0xb4
        if (travelled <= free) {
            car.speed = target;
        } else {
            car.speed = free / dt;
            travelled = free;
        }
        car.pos -= travelled;
    } else {
        // live block 0x41110a..0x411448
        float desired;
        if (leader_speed <= -0.5f)                  // 0x593744
            desired = 0.0f;
        else if (d0 - free <= 0.0f)
            desired = std::min(0.0f, leader_speed);
        else
            desired = std::min(sqrtf(2.0f * car.accel * (d0 - free)), leader_speed);
        const float old = car.speed;
        if (target < old) {                          // decelerate, car.k38 (+0x38)
            const float tt = (old - target) / car.k38;
            if (tt < dt) {
                // 0x411207/0x41120f: (old^2 - target^2) * 0.5 / k38 + (dt - tt) * target
                travelled = (old * old - target * target) * 0.5f / car.k38 +
                            (dt - tt) * target;
                new_speed = target;
            } else {
                travelled = (old - 0.5f * car.k38 * dt) * dt;
                new_speed = old - car.k38 * dt;
            }
        } else {                                     // accelerate, car.accel (+0x30)
            const float tt = (target - old) / car.accel;
            if (dt <= tt) {
                travelled = (car.accel * 0.5f * dt + old) * dt;
                new_speed = car.accel * dt + old;
            } else {
                travelled = (target * target - old * old) * 0.5f / car.accel +
                            (dt - tt) * target;
                new_speed = target;
            }
        }
        if (free > travelled)
            desired = sqrtf(2.0f * car.accel * (free - travelled) + desired * desired);
        else
            travelled = free;
        car.pos -= travelled;
        if (car.k128 != 0) {                          // lane-change steering, +0x128
            const float up = std::min((car.kac + 5.0f * dt) * dt, 0.3f * travelled);
            const float lo = std::max((car.kac - 5.0f * dt) * dt, -0.3f * travelled);
            float push = (float)(int32_t)car.k128 * -3.5f - car.lateral;   // 0x593748
            if (lo <= push) push = std::min(push, up);
            else push = lo;
            car.lateral += push;
            car.kac = push / dt;
        } else {                                       // lateral centring
            const float limit = 1.5749999f - car.k28;
            if (fabsf(car.lateral) > limit || fabsf(car.kac) > 5.0f * dt) {
                const float up = std::min((car.kac + 5.0f * dt) * dt, 0.3f * travelled);
                const float lo = std::max((car.kac - 5.0f * dt) * dt, -0.3f * travelled);
                float push = 0.0f;
                if (car.lateral > limit) push = car.lateral - limit;
                else if (car.lateral < -limit) push = car.lateral + limit;
                push = -push;
                if (lo <= push) push = std::min(push, up);
                else push = lo;
                car.lateral += push;
                car.kac = push / dt;
            }
        }
        car.speed = std::min(new_speed, desired);      // 0x411431
        car.kb4 = (car.speed - old) / dt;              // 0x411440
    }

    // step 6: ref hand-over (0x411503..0x4115ee).
    if (car.speed < 0.0f) car.speed = 0.0f;
    if (car.pos < 0.0f) {                              // 0x411519
        if (car.next_lane_b == nullptr) {              // 0x411526
            car.pos = 0.0f;
        } else {
            // prev = {path_lane (+0xc4), path (+0xc8), dir (+0xcc)}; vf4 reads only
            // the owner (0x40e710 ignores the idx argument).
            // The exe copies cur to the prev ref +0xd0/+0xd4/+0xd8 before
            // overwriting it (0x41154c/0x41155c); car_advance's 0x4105cc reads it.
            car.kd0 = car.path_lane;
            car.prev_path = car.path;
            car.prev_dir = car.dir;
            const uint32_t prev_lane = car.path_lane;
            car.path_lane = car.next_lane_a;           // 0x411546..
            car.path = car.next_lane_b;
            car.dir = car.kc0;
            car.next_lane_a = 0;
            car.next_lane_b = nullptr;
            // +0xc0 (kc0) is NOT cleared (0x411576/0x411580).
            if (car.path)
                car.pos += car.dir < 0 ? car.path->length
                                       : reinterpret_cast<const Segment *>(car.path)->len;  // +0x38/+0x30
            lane_unlink(g, ci);                        // 0x4115bb -> 0x40ca40
            // 0x40cbd0 append branch: the hand-over appends itself at the lane
            // tail (lane_attach assumes lane_insert did).
            // INVENTED: dir/lane-id bounds check (the exe dereferences the owner).
            if (car.dir < 0 && car.path_lane < g.lanes.size()) {
                g.lanes[car.path_lane].cars.push_back(ci);
                car.lane = car.path_lane;
                lane_attach(g, car.path_lane, ci);     // 0x4115c0 cur owner vf3
            } else {
                cross_attach(g, car.path_lane, ci, (size_t)car.dir);   // 0x40cb60
            }
            if (car.prev_dir < 0) lane_detach(g, prev_lane, ci);   // 0x4115d2 prev vf4
            // A cross prev owner (prev_dir >= 0) calls vf4 0x4883e0, a no-op
            // RET 8 (writes and calls nothing); the car already left its list in
            // lane_unlink 0x40ca40 above.
            car.k12c = 0.0f;                           // 0x4115e4
        }
    }
    // step 7: lane-change completion (0x4115f1..0x4116f4).
    if (car.k128 != 0) {                            // 0x4115f1
        float x = 0.0f, z = 0.0f;
        if (car.path) {                             // 0x411617
            float p[3];
            car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral, p);
            x = p[0];
            z = p[2];
        }
        // INVENTED: bounds (the exe dereferences the lane pointer).
        Lane *tl = lane_at(g, car.k124);
        float d = 0.0f, lat = 0.0f;
        bool ok = tl && path_project_strict(tl->path, x, z, &d, &lat);   // 0x414050
        if (!ok) {
            car.k128 = 0;                           // 0x411683 -> 0x4116f4
        } else if (fabsf(lat) < 1.75f) {            // 0x41168a, strict <
            const uint32_t old = car.path_lane;     // 0x4116a9
            const uint32_t ti = (uint32_t)(tl - g.lanes.data());
            car.path_lane = ti;                     // 0x4116b4
            car.pos = d;                            // 0x4116bd
            car.lateral = lat;                      // 0x4116c5 (signed)
            car.path = &tl->path;                   // 0x4116cd
            car.k124 = lane_node_id(g, old);        // 0x4116d3 swap cur <-> target
            lane_unlink(g, ci);                     // 0x4116d9 -> 0x40ca40
            car.lane = ti;                          // 0x40d1e0 writes car+0xc itself
            lane_insert(g, ti, ci);                 // 0x4116e5 -> 0x40d1e0
            lane_detach(g, old, ci);                // 0x4116f1 -> vf4 0x40e710
            car.k128 = 0;                           // 0x4116f4
        }
        // |lat| >= 1.75 keeps k128 for the next step.
    }

    // step 8: target-lane obstacle registration (0x4116fe..0x4118e4), every step.
    const bool was = car.ob_owner != kNoneLane;     // 0x4116fe
    bool registered = false;                        // 0x411859 -> done
    if (car.k124 != 0) {                            // 0x411708
        float x = 0.0f, z = 0.0f;
        if (car.path) {                             // 0x411734
            float p[3];
            car_ref_pos_offset(car.path, car.dir, car.pos, car.lateral, p);
            x = p[0];
            z = p[2];
        }
        // INVENTED: bounds (the exe dereferences the lane pointer).
        Lane *tl = lane_at(g, car.k124);
        float lat = 0.0f;
        if (!tl || !path_project_strict(tl->path, x, z, &car.kfc, &lat)) {   // 0x41179c
            car.kfc = -1.0f;                        // 0x4117a3 -> 0x41185b
        } else {
            car.k100 = car.speed;                   // 0x4117b0 raw copy
            if (car.k128 != 0 || fabsf(lat) < car.k28 + 1.75f) {   // 0x4117be
                const uint32_t ti = (uint32_t)(tl - g.lanes.data());
                if (car.ob_owner != ti) {           // 0x4117e0
                    car.k120 = car.k20;             // 0x4117f2 raw copy
                    if (car.ob_owner != kNoneLane) obstacle_unlink(g, ci);   // 0x411801
                    obstacle_link(g, ti, ci);       // 0x411838 head insert
                }
                registered = true;                  // 0x411859 -> done
            }
        }
    }
    if (was && !registered) obstacle_unlink(g, ci); // 0x411861
    if (car.k124 != 0 && car.k128 == 0 && car.ob_owner == kNoneLane)
        car.k124 = 0;                               // 0x4118d6
}

// 0x40d1e0 ctLane__insertCar: insert before the first car with a larger pos,
// else append (list order = ascending pos).
void lane_insert(Graph &g, size_t li, uint32_t ci)
{
    std::vector<uint32_t> &list = g.lanes[li].cars;
    float pos = g.cars[ci].pos;
    size_t at = 0;
    while (at < list.size() && !(pos < g.cars[list[at]].pos)) at++;
    list.insert(list.begin() + at, ci);
    lane_attach(g, li, ci);
}

// Per-car body of 0x40ed30/0x40ebb0, the same branch sequence as 0x410680.
void car_bind_object(Graph &g, uint32_t ci, uint32_t object)
{
    Car &car = g.cars[ci];
    const uint32_t inst = car.inst_id;   // +0x138
    if (car.type_id == 0) {              // +0x14c
        if (inst) {
            // shortcut: 0x4ec210 reparent under (car.live && linked ? obj
            // : T+0x311c CarContainer) is not ported.
        }
    } else if (!car.live || object == 0) {   // exe: obj+8 == 0 (not linked). INVENTED: object id 0 stands in for it
        if (inst) {
            // shortcut: the car_rem event (T+0x3130) is not modelled.
            car.e8 = g.sim_time;
            // shortcut: Traffic__parkInstance 0x417080 and the +0x130
            // type link unlink 0x488080 are not modelled.
            car.inst_id = 0;
            g.inst_count -= 1;                       // 0x40ee9b / 0x40ed0b
            g.parked_count += 1;                     // 0x40eea1 / 0x40ed11
        }
    } else if (inst) {
        // shortcut: 0x4ec210 reparent under obj is not ported.
    } else {
        g.inst_count += 1;                       // 0x40edda / 0x40ec4a
        g.spawn_count += 1;                      // 0x40ede0 / 0x40ec50
        queue_spawn(g, ci);                      // 0x41a500
    }
}

// 0x40ed30 ctRoad__bindObject.
void road_bind_object(Graph &g, Road &road, uint32_t object)
{
    road.object = object;                       // +0x68 ResRefLink
    Lane *l = lane_at(g, road.node[0]);
    // INVENTED: cap the pointer walk with the lane count (the exe follows a cycle).
    for (size_t guard = 0; l && guard < g.lanes.size(); guard++) {
        for (uint32_t ci : l->cars) car_bind_object(g, ci, object);
        l = lane_at(g, l->node14);
    }
}

// 0x40ebb0 ctCross__bindObject.
void cross_bind_object(Graph &g, Cross &cross, uint32_t object)
{
    cross.object = object;   // +0x44 ResRefLink
    // cross+8/+0xc car list, first to last.
    for (uint32_t ci : cross.cars) car_bind_object(g, ci, object);
}

// 0x40f4e0 ctLane__activeStep: keep a live lane listed while its sim-time stamp
// is fresh, else switch it off so the caller unlinks it.
bool lane_active_step(Graph &g, size_t li)
{
    Lane &lane = g.lanes[li];
    if (!lane.live) return false;                    // +0xa8
    if (g.sim_time - lane.kc0 <= 0.5f) {             // +0x84, +0xc0
        // INVENTED: index loop (the exe walks the intrusive car list); car_step
        // can append to this lane from another lane's hand-over.
        for (size_t k = 0; k < lane.cars.size(); k++) {
            const uint32_t ci = lane.cars[k];
            if (g.cars[ci].next_lane_b != 0) continue;   // +0xbc
            car_step(g, ci);                             // 0x4107c0
            // 0x40f4e0: 2v braking time to the lane end, min speed 10.
            const float stop = g.cars[ci].next_lane_b ? 0.0f : g.cross_stop_offset;
            float v = car_target_speed(g, g.cars[ci]);   // 0x40dc00
            if (v <= 10.0f) v = 10.0f;                   // 0x59373c
            float t = ((g.cars[ci].pos - g.cars[ci].k14) - stop) * (2.0f / v);  // 0x593728
            if (t < 0.0f) t = 0.0f;                      // 0x593580
            lane_end_move(g, li, ci, t);                 // 0x40e460, result ignored
        }
        return true;
    }
    g.set_lane_live(li, false);                      // 0x40f6d0 ctLane__setLive
    return false;
}

// 0x40d380 ctLane__scheduleIfIdle (ECX = lane, float dt): schedule a timed
// event for the lane unless one is already pending.
void lane_schedule_if_idle(Graph &g, Lane &lane, size_t li, float dt)
{
    if (lane.pending_events != 0) return;            // +0xac
    schedule_lane(g, dt, (uint32_t)li);              // 0x417050
    lane.pending_events++;
}

// 0x41a810 TrafficHeap__pop (ECX = T): drop the root and sift the last entry
// down (children always < n, so the back value is only read and written last).
void heap_pop(Graph &g)
{
    if (g.heap.empty()) return;                      // INVENTED guard (caller checks the count)
    const Graph::HeapEntry last = g.heap.back();
    g.heap.pop_back();
    const size_t n = g.heap.size();                  // T+4 after the decrement
    if (n == 0) return;
    size_t i = 0, c = 1;
    while (c < n) {
        const size_t r = 2 * i + 2;
        if (r < n) {
            if (g.heap[c].time <= g.heap[r].time) {
                if (last.time <= g.heap[c].time) break;
                g.heap[i] = g.heap[c];
                i = c;
            } else {
                if (last.time <= g.heap[r].time) break;
                g.heap[i] = g.heap[r];
                i = r;
            }
        } else {
            if (last.time <= g.heap[c].time) break;
            g.heap[i] = g.heap[c];
            i = c;
        }
        c = 2 * i + 1;
    }
    g.heap[i] = last;
}

// 0x40fcf0 ctLane__timedEvent (ECX = lane): step the cars of a not-live lane
// and reschedule it from the head car's time to the lane end.
void lane_timed_event(Graph &g, uint32_t li)
{
    Lane &lane = g.lanes[li];
    lane.pending_events--;                           // +0xac (int)
    if ((int32_t)lane.pending_events < 0) lane.pending_events = 0;
    if (lane.live) return;                           // +0xa8: live lanes are stepped by 0x40f4e0
    // INVENTED: index loop (the exe reads car+0x8 before each step); car_step
    // can relink this lane's car list under the walk.
    for (size_t k = 0; k < lane.cars.size(); k++)
        car_step(g, lane.cars[k]);                   // 0x4107c0
    const uint32_t head = lane.cars.empty() ? kLiveEnd : lane.cars.front();
    if (head != kLiveEnd && g.cars[head].next_lane_b == 0) {   // +0xbc: no next lane
        const float f = car_time_to_lane_end(g, head);         // 0x40d990
        if ((double)f <= 0.01) {                     // 0x593560
            // 0x40e460 call at 0x40fd8c, t = 0; a failed move falls back to the
            // idle retry 0x40d380.
            if (!lane_end_move(g, li, head, 0.0f))
                lane_schedule_if_idle(g, lane, li, 1.0f);
        } else {
            if (lane.pending_events != 0) return;
            schedule_lane(g, f, li);                 // 0x417050
            lane.pending_events++;
        }
    }
    if (lane.pending_events == 0 && !lane.cars.empty()) {
        schedule_lane(g, 1.0f, li);                  // 0x417050
        lane.pending_events++;
    }
}

}  // namespace

// 0x5e6530 / 0x5e6534: route score offsets, process-wide BSS. DAT534 keeps a
// stale value when p2 was not snapped (tasks/251).
static float route_530;
static float route_534;

namespace {

// INVENTED: the exe compares Cross* directly; roads store cross node ids, so
// this maps a cross pointer back to its node id (inverse of cross_by_node).
uint32_t cross_id_of(const Graph &g, const Cross *c)
{
    const uintptr_t a = (uintptr_t)c;
    const uintptr_t eq = (uintptr_t)g.cross_eq.data();
    if (a >= eq && a < eq + g.cross_eq.size() * sizeof(Cross))
        return (uint32_t)((a - eq) / sizeof(Cross)) + 1;
    const uintptr_t light = (uintptr_t)g.cross_light.data();
    if (a >= light && a < light + g.cross_light.size() * sizeof(Cross))
        return (uint32_t)(g.cross_eq.size() + (a - light) / sizeof(Cross)) + 1;
    return 0;
}

// 0x4120d0 ctPath__addStraight + 0x414e00 ctSegment__buildStraight.
void path_add_straight(Path &p, const float p0[3], const float p1[3], float w,
                       float f1, int32_t i1, float f2, float f3, int32_t i2)
{
    Segment s;
    float d[3];
    for (int i = 0; i < 3; i++) {
        s.p0[i] = p0[i];
        s.p1[i] = p1[i];
        d[i] = p1[i] - p0[i];
    }
    float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    float flat = sqrtf(d[0] * d[0] + d[2] * d[2]);   // 0x414ecc: horizontal
    for (int i = 0; i < 3; i++) s.m0[i] = d[i];
    // exe 0x414e5e..0x414f93 normalizes m0/m1 in place and skips the division
    // when the length is 0 or NaN (0x414f12, 0x414f70), keeping the raw vector;
    // the card's "normalizes a zero vector" is wrong, exe wins.
    if (len > 0.0f) normalize(s.m0);
    // m1 is the horizontal side vector (the card said -m0; exe wins), which
    // segment_side 0x4131a0 returns for straight segments.
    s.m1[0] = -d[2];
    s.m1[1] = 0.0f;
    s.m1[2] = d[0];
    if (flat > 0.0f) normalize(s.m1);
    s.len = len;
    s.flat_len = flat;
    s.curved = false;
    s.width = w;
    s.k40 = f1;
    s.i1 = i1;
    s.f2 = f2;
    s.f3 = f3;
    s.i2 = i2;
    p.segs.push_back(s);
    p.length = s.len + p.length;
    // 0x4120d0 stores seg.m0 verbatim in start_dir and -seg.m0 in end_dir; it
    // does not normalize again.
    if (p.segs.size() == 1) {
        for (int i = 0; i < 3; i++) {
            p.start[i] = p0[i];
            p.start_dir[i] = s.m0[i];
        }
    }
    for (int i = 0; i < 3; i++) {
        p.end[i] = p1[i];
        p.end_dir[i] = -s.m0[i];
    }
}

// 0x412960 ctPath__append: append src (reversed when reverse), bridging the gap
// with a 0x4122b0 connector when the ends are more than 0.001 apart.
void path_append(Path &p, const Path &src, bool reverse)
{
    const size_t n = src.segs.size();
    if (p.segs.empty()) {
        if (reverse) {
            for (int i = 0; i < 3; i++) {
                p.start[i] = src.end[i];
                p.start_dir[i] = -src.end_dir[i];
            }
        } else {
            for (int i = 0; i < 3; i++) {
                p.start[i] = src.start[i];
                p.start_dir[i] = src.start_dir[i];
            }
        }
    } else {
        // 0x412960 runs this connector whenever dest is non-empty, with no
        // source-count test. INVENTED: an empty source has no segs[0] to read,
        // so use width/f1 = 0 (a loaded road/lane path always has one).
        const Segment &last = p.segs.back();
        const float sw = src.segs.empty() ? 0.0f : src.segs.front().width;
        const float sf = src.segs.empty() ? 0.0f : src.segs.front().k40;
        const float *attach = reverse ? src.end : src.start;
        const float *adir = reverse ? src.end_dir : src.start_dir;
        float dx = p.end[0] - attach[0];
        float dy = p.end[1] - attach[1];
        float dz = p.end[2] - attach[2];
        if (dx * dx + dy * dy + dz * dz > 0.001f) {
            float d1[3];
            for (int i = 0; i < 3; i++) d1[i] = reverse ? -adir[i] : adir[i];
            float w = last.width < sw ? last.width : sw;
            float f1 = last.k40 < sf ? last.k40 : sf;
            path_add_segment(p, p.end, p.end_dir, attach, d1, w, f1, 1, 10000.0f,
                             10000.0f, 0);
        }
    }
    for (size_t k = 0; k < n; k++) {
        const Segment &s = src.segs[reverse ? n - 1 - k : k];
        Segment d = s;
        if (reverse) {
            for (int i = 0; i < 3; i++) {
                d.p0[i] = s.p1[i];
                d.p1[i] = s.p0[i];
                if (!s.curved) {
                    d.m0[i] = -s.m0[i];
                    d.m1[i] = -s.m1[i];
                } else {
                    d.m0[i] = -s.m1[i];
                    d.m1[i] = -s.m0[i];
                }
            }
        }
        p.segs.push_back(d);
    }
    if (reverse) {
        for (int i = 0; i < 3; i++) {
            p.end[i] = src.start[i];
            p.end_dir[i] = -src.start_dir[i];
        }
    } else {
        for (int i = 0; i < 3; i++) {
            p.end[i] = src.end[i];
            p.end_dir[i] = src.end_dir[i];
        }
    }
    p.length = p.length + src.length;
}

// 0x4167a0 node-table push: 12-byte {parent,cross,road}; returns the old count.
struct RouteEntry {
    int32_t parent;      // -1 = start node
    const Cross *cross;
    Road *road;
};
// 0x41a8b0 open-heap entry: 12-byte {g,h,node}; key = g+h.
struct RouteHeapItem {
    float g;
    float h;
    uint32_t node;
};

uint32_t route_node_push(std::vector<RouteEntry> &t, int32_t parent,
                         const Cross *cross, Road *road)
{
    uint32_t idx = (uint32_t)t.size();
    t.push_back(RouteEntry{parent, cross, road});
    return idx;
}

// 0x41a8b0 open-heap push: sift up while the parent key is greater.
void route_heap_push(std::vector<RouteHeapItem> &h, const RouteHeapItem &v)
{
    const float key = v.g + v.h;
    h.push_back(v);
    size_t i = h.size() - 1;
    while (i > 0) {
        size_t par = (i - 1) / 2;
        if (!(h[par].g + h[par].h > key)) break;
        h[i] = h[par];
        i = par;
    }
    h[i] = v;
}

// 0x41a6f0 open-heap pop: heap[0], last moved to the root, sift down on the
// smaller child key.
RouteHeapItem route_heap_pop(std::vector<RouteHeapItem> &h)
{
    RouteHeapItem top = h.front();
    RouteHeapItem last = h.back();
    h.pop_back();
    if (!h.empty()) {
        size_t i = 0, n = h.size();
        const float key = last.g + last.h;
        while (true) {
            size_t l = i * 2 + 1;
            if (l >= n) break;
            size_t m = l;
            if (l + 1 < n && h[l + 1].g + h[l + 1].h < h[l].g + h[l].h) m = l + 1;
            if (!(h[m].g + h[m].h < key)) break;
            h[i] = h[m];
            i = m;
        }
        h[i] = last;
    }
    return top;
}

// 0x419c50 Traffic__heuristic (body inlined at 0x418511..0x418626).
float route_heuristic(const Cross *node, const Cross *goal, const Cross *alt)
{
    float dx = node->pos[0] - goal->pos[0];
    float dy = node->pos[1] - goal->pos[1];
    float dz = node->pos[2] - goal->pos[2];
    float dA = sqrtf(dx * dx + dy * dy + dz * dz);
    if (goal == alt) return dA;
    dx = node->pos[0] - alt->pos[0];
    dy = node->pos[1] - alt->pos[1];
    dz = node->pos[2] - alt->pos[2];
    float dB = sqrtf(dx * dx + dy * dy + dz * dz);
    float a = dA + route_530;
    float b = dB + route_534;
    return b <= a ? b : a;
}

}  // namespace

// 0x417e50 Traffic__findRoute: A*-style search from p2 to p1 over the
// cross/road graph; returns a new Path (p2 side first) or false.
bool Graph::find_route(const float p1[3], const float p2[3], bool flag1, bool flag2,
                       Path *out)
{
    // --- snap p2 (0x417e91..0x418020)
    const Cross *c2 = nearest_cross(p2[0], p2[1], p2[2], 0.0f, true);
    const Cross *goal = c2, *alt = c2;
    Road *p2road = nullptr;
    float d2 = 0.0f;
    if (!flag2) {
        const Lane *l = nearest_lane_pos(p2[0], p2[1], p2[2], nullptr, nullptr);
        if (l) {
            Road *r = road_by_node(*this, l->road);
            // INVENTED: the exe dereferences a null/out-of-range road.
            if (r) {
                p2road = r;
                float lat = 0.0f;
                path_project(p2road->path, p2[0], p2[2], &d2, &lat);
                goal = cross_by_node(*this, p2road->node[1], nullptr);
                if (p2road->k64 > 0) {
                    p2road = nullptr;
                    route_530 = 0.0f;
                } else {
                    alt = cross_by_node(*this, p2road->node[2], nullptr);
                    // INVENTED: an unresolvable alt falls back to goal.
                    if (!alt) alt = goal;
                    route_534 = d2;
                    route_530 = p2road->path.length - d2;
                }
            } else {
                route_530 = 0.0f;
            }
        } else {
            route_530 = 0.0f;
        }
    } else {
        route_530 = 0.0f;
    }
    if (!goal) return false;                                    // 0x417f44

    // --- snap p1 (0x417f6b..0x418176)
    const Cross *start = nearest_cross(p1[0], p1[1], p1[2], 0.0f, true);
    Road *p1road = nullptr;
    float d1 = 0.0f;
    const Lane *lane1 =
        flag1 ? nullptr : nearest_lane_pos(p1[0], p1[1], p1[2], nullptr, nullptr);
    bool two_start = false;
    if (lane1) {
        Road *r = road_by_node(*this, lane1->road);
        // INVENTED: the exe dereferences a null/out-of-range road.
        if (r) {
            p1road = r;
            float lat = 0.0f;
            path_project(p1road->path, p1[0], p1[2], &d1, &lat);
            start = cross_by_node(*this, p1road->node[1], nullptr);
            if (p1road->k64 <= 0) {                             // 0x417fe4
                two_start = true;
                // same-road fast path (0x418025)
                if (p2road && p2road == p1road) {
                    Path P;
                    if (d2 > d1) {
                        const Cross *c0 =
                            cross_by_node(*this, p2road->node[1], nullptr);
                        if (c0 && !p2road->path.segs.empty())   // INVENTED guards
                            path_add_straight(P, c0->pos, p2road->path.end,
                                              p2road->path.segs.back().width, 1.0f,
                                              1, 10000.0f, 10000.0f, 0);
                        path_append(P, p2road->path, true);
                    } else {
                        const Cross *c1 =
                            cross_by_node(*this, p2road->node[2], nullptr);
                        if (c1 && !p2road->path.segs.empty())   // INVENTED guards
                            path_add_straight(P, c1->pos, p2road->path.start,
                                              p2road->path.segs.front().width, 1.0f,
                                              1, 10000.0f, 10000.0f, 0);
                        path_append(P, p2road->path, false);
                    }
                    *out = P;                                   // 0x418104
                    return true;
                }
            } else {
                p1road = nullptr;                               // 0x417ff0
            }
        }
    }

    std::vector<RouteEntry> node_table;
    std::vector<RouteHeapItem> open_heap;
    if (two_start) {
        // two start nodes (0x418109); cross0 = path end side, cross1 = start.
        Cross *c0 = cross_by_node(*this, p1road->node[1], nullptr);
        if (c0) {                                               // INVENTED guard
            float g = p1road->path.length - d1;
            uint32_t idx = route_node_push(node_table, -1, c0, p1road);
            float h = route_heuristic(c0, goal, alt);
            route_heap_push(open_heap, RouteHeapItem{g, h, idx});
        }
        Cross *c1 = cross_by_node(*this, p1road->node[2], nullptr);
        if (c1) {                                               // INVENTED guard
            float g = d1;
            uint32_t idx = route_node_push(node_table, -1, c1, p1road);
            float h = route_heuristic(c1, goal, alt);
            route_heap_push(open_heap, RouteHeapItem{g, h, idx});
        }
    } else {
        if (!start) return false;                               // 0x418176
        if (p2road != nullptr && start == goal) {               // 0x41817e
            Path P;
            path_append(P, p2road->path,
                        p2road->node[1] != cross_id_of(*this, goal));
            *out = P;
            return true;
        }
        if (p2road != nullptr && start == alt) {                // 0x4181c4
            Path P;
            path_append(P, p2road->path,
                        p2road->node[1] != cross_id_of(*this, alt));
            *out = P;
            return true;
        }
        // push_one_start (0x418226)
        uint32_t idx = route_node_push(node_table, -1, start, nullptr);
        float h = route_heuristic(start, goal, alt);
        route_heap_push(open_heap, RouteHeapItem{0.0f, h, idx});
    }

    // --- reset route_g of every cross to -1.0f (0x41826d / 0x4182a2)
    for (Cross &c : cross_eq) c.route_g = -1.0f;
    for (Cross &c : cross_light) c.route_g = -1.0f;

    // --- A* main loop (0x4182dd..0x4186b2)
    int32_t found = -1;
    uint32_t found_idx = 0;
    while (!open_heap.empty()) {
        RouteHeapItem top = route_heap_pop(open_heap);          // 0x4182f0
        const RouteEntry &E = node_table[top.node];
        if (E.cross == goal || E.cross == alt) {                // 0x41831c
            found = (int32_t)top.node;
            found_idx = top.node;
            break;
        }
        const uint32_t eid = cross_id_of(*this, E.cross);
        float prev_dir[3] = {0.0f, 0.0f, 0.0f};
        if (E.road) {
            if (E.road->node[1] == eid) {
                for (int i = 0; i < 3; i++) prev_dir[i] = E.road->path.end_dir[i];
            } else {
                for (int i = 0; i < 3; i++) prev_dir[i] = -E.road->path.start_dir[i];
            }
        }
        Road *prev_road = nullptr;                              // ESI, 0x41838f
        for (const CrossLink &link : E.cross->links) {          // 0x4183a0
            Lane *l = lane_at(*this, link.node);
            if (!l) continue;                                   // INVENTED guard
            Road *Rn = road_by_node(*this, l->road);
            if (!Rn) continue;                                  // INVENTED guard
            if (Rn == prev_road) continue;                      // 0x4183a9 dedupe
            prev_road = Rn;                                     // 0x4183b1
            if (Rn->k64 > 0) continue;                          // 0x4183b7
            // INVENTED: the exe reads segs[0] unchecked.
            if (Rn->path.segs.empty()) continue;
            const bool at_end = (Rn->node[1] == eid);
            float next_dir[3];
            if (at_end) {
                for (int i = 0; i < 3; i++) next_dir[i] = Rn->path.end_dir[i];
            } else {
                for (int i = 0; i < 3; i++) next_dir[i] = -Rn->path.start_dir[i];
            }
            Cross *far = cross_by_node(*this, at_end ? Rn->node[2] : Rn->node[1],
                                       nullptr);
            if (!far) continue;                                 // INVENTED guard
            const bool is_new = (far != goal && far != alt);
            float dot = prev_dir[0] * next_dir[0] + prev_dir[1] * next_dir[1] +
                        prev_dir[2] * next_dir[2];
            float g_new = (dot + 1.0f) * 30.0f;
            g_new = g_new + top.g;
            g_new = g_new + (((max_road_width / Rn->path.segs[0].width) + 4.0f) *
                             Rn->path.length * 0.2f);
            if (far == goal) g_new = g_new + route_530;         // 0x4184c4
            else if (far == alt) g_new = g_new + route_534;
            float rg = far->route_g;
            if (!(-0.5f > rg || g_new < rg)) continue;          // 0x4184e6
            far->route_g = g_new;                               // 0x418504
            float h = is_new ? route_heuristic(far, goal, alt) : 0.0f;
            uint32_t idx = route_node_push(node_table, (int32_t)top.node, far, Rn);
            route_heap_push(open_heap, RouteHeapItem{g_new, h, idx});
        }
    }
    if (found < 0) return false;                                // 0x4186b5

    // --- found: assemble the path (0x4186e7..0x418ad7)
    Path P;
    const RouteEntry &F = node_table[found_idx];
    int32_t root = found;
    Road *last_road = nullptr;

    // 0x4120d0 connector: cross centre -> the road endpoint that sits at c
    // (node[1] is the path end side).
    auto add_road_connector = [&](const Cross *c, const Road *r) {
        if (!c || !r || r->path.segs.empty()) return;   // INVENTED guards
        if (r->node[1] == cross_id_of(*this, c)) {
            path_add_straight(P, c->pos, r->path.end, r->path.segs.back().width,
                              1.0f, 1, 10000.0f, 10000.0f, 0);
        } else {
            path_add_straight(P, c->pos, r->path.start, r->path.segs.front().width,
                              1.0f, 1, 10000.0f, 10000.0f, 0);
        }
    };

    if (!flag2) {
        if (p2road) {
            // 0x418834 branch B: p2 road piece, ending at the found cross.
            const uint32_t fid = cross_id_of(*this, F.cross);
            Cross *other = cross_by_node(*this,
                                         p2road->node[1] == fid ? p2road->node[2]
                                                                : p2road->node[1],
                                         nullptr);
            add_road_connector(other, p2road);
            path_append(P, p2road->path, p2road->node[1] != fid);
        } else {
            // 0x418918 branch E: no p2 road.
            Road *r = F.road ? F.road : p1road;
            if (r) add_road_connector(goal, r);
        }
    } else if (F.parent == -1) {
        // 0x418777 branch C: found is a start node; p1road == 0 falls through
        // to branch E (0x418834 -> 0x418918 -> CHAIN_SETUP).
        if (p1road) add_road_connector(goal, p1road);
    } else {
        // 0x41872e branch D: found road piece back to the goal cross.
        add_road_connector(goal, F.road);
    }

    // 0x418998 CHAIN_SETUP / 0x4189b0 parent-chain loop: append road(cur) so the
    // piece ends at entry[parent].cross; the root node's own road is not added.
    int32_t cur = found;
    int32_t par = F.parent;
    while (par != -1) {                                         // 0x4189a0
        Road *rc = node_table[(size_t)cur].road;
        if (rc) {                                               // INVENTED guard
            last_road = rc;
            const bool reverse =
                (rc->node[1] != cross_id_of(*this, node_table[(size_t)par].cross));
            path_append(P, rc->path, reverse);
        }
        int32_t grand = node_table[(size_t)par].parent;
        cur = par;
        par = grand;
    }
    root = cur;

    // 0x4189f9 post-loop.
    if (flag1 && last_road) {
        add_road_connector(start, last_road);
    } else if (p1road) {
        // 0x418ab4: the p1 road starts at the root cross (SETZ).
        const bool reverse =
            (p1road->node[1] == cross_id_of(*this, node_table[(size_t)root].cross));
        path_append(P, p1road->path, reverse);
    }

    // 0x418ad7 empty-path fallback: p2 argument -> p1 argument, width 1.0f.
    if (P.segs.empty())
        path_add_straight(P, p2, p1, 1.0f, 1.0f, 1, 10000.0f, 10000.0f, 0);

    *out = P;                                                   // 0x4186b7
    return true;
}

// 0x419da0 Traffic__init.
void Graph::init(float t)
{
    sim_time = t;
    // T+0x3118 (player pseudo-car) is null right after the ctor.
    max_road_width = 0.0f;
    for (Cross &c : cross_eq) cross_init(*this, c);
    for (Cross &c : cross_light) light_init(*this, c);
    for (Road &r : roads) road_init(*this, r);
    for (Lane &l : lanes) lane_init(*this, l);

    int m = 0;
    for (const Road &r : roads)
        if ((int32_t)r.k64 > m) m = (int32_t)r.k64;
    road_enabled.assign((size_t)m + 1, 0);         // [0] = 0
    for (int i = 1; i <= m; i++) road_enabled[(size_t)i] = 1;
}

// 0x41b290 Traffic step.
void Graph::step(float t)
{
    sim_time = t;  // +0x84
    // shortcut: the pseudo-car prologue stores (T+0x3118) are a no-op in src
    // (nothing is parked) and DAT_005e652c is not modelled.
    while (!heap.empty() && heap.front().time < t) {  // strict <; +0x4 count, +0x0 array
        lane_timed_event(*this, heap.front().lane);   // 0x40fcf0
        heap_pop(*this);                              // 0x41a810
    }
    uint32_t *p = &active_eq;      // +0x90 head, link = Cross::active_next (+0x54)
    while (*p != kLiveEnd) {
        if (cross_eq_step(cross_eq[*p - 1])) {
            p = &cross_eq[*p - 1].active_next;
        } else {
            const uint32_t n = cross_eq[*p - 1].active_next;
            cross_eq[*p - 1].active_next = 0;
            *p = n;
        }
    }
    p = &active_light;             // +0x94 head, same link field (+0x60)
    while (*p != kLiveEnd) {
        if (cross_light_step(*this, cross_light[*p - 1])) {
            p = &cross_light[*p - 1].active_next;
        } else {
            const uint32_t n = cross_light[*p - 1].active_next;
            cross_light[*p - 1].active_next = 0;
            *p = n;
        }
    }
    p = &live_cars;                // +0x88 head, link = Car::live_next (+0x44)
    while (*p != kLiveEnd) {
        if (car_live_step(*this, cars[*p - 1])) {
            p = &cars[*p - 1].live_next;
        } else {
            const uint32_t n = cars[*p - 1].live_next;
            cars[*p - 1].live_next = 0;
            *p = n;
        }
    }
    p = &active_lanes;   // +0x8c head, link = Lane::active_next (+0xa4)
    while (*p != kLiveEnd) {
        if (lane_active_step(*this, *p - 1)) {
            p = &lanes[*p - 1].active_next;
        } else {
            const uint32_t n = lanes[*p - 1].active_next;
            lanes[*p - 1].active_next = 0;
            *p = n;
        }
    }
    // T+0xc8 event-1 drain: unlink each car and notify its instance.
    while (event1 != kLiveEnd) {
        const uint32_t ci = event1 - 1;   // INVENTED: 1-based car index (a pointer in the exe)
        Car &car = cars[ci];
        event1 = car.event_next;          // T+0xc8 = car+0x10
        car.event_next = 0;
        if (car.inst_id != 0) {           // +0x138
            // SERVICE MISSING: 0x47d550 Game__queueEvent(ECX = 0x5fde90,
            // &car+0x130, 0, 1, 0, 0) binds the instance (ResRefLink__set
            // 0x40cac0 from car+0x13c) and queues event code 1 (0x47c760
            // returns 1, ignored). The traffic module has no Runtime*, so
            // the event is not dispatched.
        }
    }
}

// 0x40ce00 ctRoad__spawnCar (ECX = road).
uint32_t Graph::spawn_on_road(size_t road, float p1, float p2, float p3, float p4)
{
    if (road >= roads.size()) return 0;   // INVENTED: the exe has no bounds check.
    // Road lanes are the node chain road.node[0] -> lane.node14.
    std::vector<size_t> free_lanes;
    const Lane *l = lane_at(*this, roads[road].node[0]);
    // INVENTED: cap the pointer walk with the lane count (the exe follows a cycle).
    for (size_t guard = 0; l && guard < lanes.size(); guard++) {
        if (!l->halted) free_lanes.push_back((size_t)(l - lanes.data()));
        l = lane_at(*this, l->node14);
    }
    if (free_lanes.empty()) return 0;
    size_t li = free_lanes[msvc_rand() % free_lanes.size()];  // rand15
    Lane &lane = lanes[li];

    uint32_t ci = car_alloc(*this);
    car_ctor(*this, ci);
    Car &car = cars[ci];
    car.max_speed = 15.0f;
    car.accel = 1.0f;
    car.k34 = 0.06f;
    car.k3c = 0.01f;
    car.k158 = 0;
    car.k14 = p1; car.k18 = p2; car.k20 = p3; car.k24 = p4;
    car.next_lane_a = 0;
    car.next_lane_b = nullptr;
    car.lane = (uint32_t)li;
    car.path_lane = (uint32_t)li;
    car.path = &lane.path;
    car.dir = -1;
    car.kd0 = 0; car.prev_path = nullptr; car.prev_dir = 0;
    car.lateral = 0.0f;
    car.e0 = sim_time;

    // Free length along the lane (list order is ascending pos).
    float free_len = 0.0f, edge = p1;
    for (uint32_t cj : lane.cars) {
        const Car &c = cars[cj];
        float back = c.pos - c.k14 - ((c.speed - 2.0f) * 0.5f + 1.0f) * c.k24 - p3;
        if (edge < back) free_len += back - edge;
        edge = c.k20 + c.pos + p4 + p1;
    }
    float tail = lane.path.length - p3;
    if (edge < tail) free_len += tail - edge;
    if (free_len <= 0.0f) {
        car_release(*this, ci);
        free_cars.push_back(ci);
        return 0;
    }

    float r = (float)msvc_rand() * 3.0517578e-05f * free_len;  // rand15
    float acc = 0.0f;
    edge = p1;
    for (uint32_t cj : lane.cars) {
        const Car &c = cars[cj];
        float back = c.pos - c.k14 - ((c.speed - 2.0f) * 0.5f + 1.0f) * c.k24 - p3;
        if (edge < back) {
            float nacc = back + acc - edge;
            if (r <= nacc) break;
            acc = nacc;
        }
        edge = c.k20 + c.pos + p4 + p1;
    }
    car.speed = 0.0f;
    car.kac = 0.0f;
    car.pos = (r - acc) + edge;
    float p[3];
    path_pos_offset_at(lane.path, car.pos, car.lateral, p);
    for (int k = 0; k < 3; k++) car.world[k] = p[k];
    float v[3];
    path_dir_at(lane.path, car.pos, v);
    car.front_x = p[0] + car.k18 * v[0];
    car.front_z = p[2] + car.k18 * v[2];
    lane_insert(*this, li, ci);
    return ci + 1;
}

// 0x40fae0, the per-car helper of 0x40de70 (ECX = cross, arg = car): recycle a
// car stopped by the halt. type_id == 0 spawns a replacement near the cross and
// copies its fields; type_id != 0 parks the script instance. Both end in
// TrafficCar__remove 0x40f290.
// shortcut: +0x130/+0x134/+0x13c instance links, 0x417080 parkInstance,
// 0x40cac0, 0x47c640 getInfo 0x38, 0x410680 syncInstance and car_add are not
// modelled (src has no materialized cars).
static void recycle_halted_car(Graph &g, const Cross &c, uint32_t ci)
{
    Car &car = g.cars[ci];
    if (car.type_id == 0) {                          // +0x14c
        uint32_t nc = g.spawn_near(c.pos[0], c.pos[1], c.pos[2], 100.0f,
                                   car.k14, car.k18, car.k20, car.k24);  // 0x416800
        // INVENTED: the exe writes new+0x130 unchecked; a failed spawn only
        // removes the old car here.
        if (nc) {
            Car &n = g.cars[nc - 1];
            n.max_speed = car.max_speed;   // +0x2c
            n.accel = car.accel;           // +0x30
            n.lateral = car.lateral;       // +0xa8
            n.seed = car.seed;             // +0x154
            n.notify = car.notify;         // +0xdd
        }
    } else {
        // typed branch: counters are unconditional (no +0x138 test), unlike 0x410680.
        g.inst_count -= 1;                           // 0x40fb4a
        g.parked_count += 1;                         // 0x40fb50
    }
    car.inst_id = 0;                                 // +0x138, both branches
    g.remove_car(ci + 1);                            // 0x40f290
}

// 0x40de70 ctCross__halt (ECX = cross; args float delay, char all): the only
// writer of Cross::k40. It writes no lane field: lane+0xc4 is set by 0x419980/
// 0x419ab0 (haltPath) only.
void Graph::halt_cross(Cross &c, float delay, bool all)
{
    c.k40 = sim_time + delay;                        // 0x40de90
    std::vector<uint32_t> own = c.cars;              // INVENTED: snapshot (exe follows car+0x8)
    for (uint32_t ci : own) {
        Car &car = cars[ci];
        if (car.notify && car.live) {                // 0x40dea3/0x40deac
            live_count--;                            // 0x40deb5 (global 0x5e6514)
            // shortcut: T+0x3130 "car_rem %d %d" (0x528030/0x47d550) not modelled
            car.live = 0;                            // 0x40df10
            if (car.k128) car.k128 = 0;              // 0x40df19
        }
        recycle_halted_car(*this, c, ci);            // 0x40fae0, always
    }
    std::vector<CrossLink> links = c.links;          // INVENTED: snapshot of +0x14/+0x18
    for (size_t i = 0; i < links.size(); i++) {
        if (!all && links[i].flag == 0) continue;    // 0x40df50..0x40df5b
        Lane *lane = lane_at(*this, links[i].node);  // INVENTED: null guard
        if (!lane) continue;
        std::vector<uint32_t> list = lane->cars;     // INVENTED: snapshot (exe re-reads +0x8)
        for (uint32_t ci : list) {
            Car &car = cars[ci];
            if (!all && car.next_lane_b == nullptr) continue;   // +0xbc
            if (car.notify && car.live) {
                live_count--;
                // shortcut: car_rem sink not modelled
                car.live = 0;
                if (car.k128) car.k128 = 0;
            }
            recycle_halted_car(*this, c, ci);           // 0x40fae0, at 0x40e009
        }
    }
}

// Inner block of 0x419980 and the body of 0x419ab0 after the halted write: from
// the lane's road read the far cross (road node[1 + lane.k18]) and queue every
// unhalted flagged link lane that no open path of that cross reaches.
static void halt_push_far_links(Graph &g, Lane &lane)
{
    Road *rn = road_by_node(g, lane.road);              // lane+0x10
    if (!rn) return;                                    // INVENTED: null guard
    // 0x4199c6/0x4199c9 read *(road + 0x8 + 4*k18); the road nodes sit at
    // +0x4/+0x8/+0xc, so that is node[1 + k18]. (lane_end_move 0x40e460 reads
    // node[2 - k18] instead; the halt trace wins here.)
    size_t ni = 1 + (size_t)lane.k18;
    if (ni > 2) return;                                 // INVENTED: k18 guard
    Cross *c2 = cross_by_node(g, rn->node[ni], nullptr);
    if (!c2) return;                                    // INVENTED: null guard
    for (size_t j = 0; j < c2->links.size(); j++) {     // cross+0x18
        if (c2->links[j].flag == 0) continue;           // 0x4199d3
        Lane *lj = lane_at(g, c2->links[j].node);       // INVENTED: null guard
        if (!lj || lj->halted) continue;                // +0xc4
        bool open = false;
        for (const CrossPath &p : c2->paths) {          // cross+0x1c / +0x20
            if ((size_t)p.a != j) continue;
            // INVENTED: link index guard; the exe reads links[b] unchecked.
            if (p.b >= c2->links.size()) continue;
            Lane *lb = lane_at(g, c2->links[p.b].node);
            if (lb && !lb->halted) { open = true; break; }
        }
        if (open) continue;                             // goto skip_push
        // The exe grows the global by 0x400 entries (0x5679b0); a vector grows
        // geometrically and holds lane indices. INVENTED: index, growth.
        g.halt_worklist.push_back((uint32_t)(lj - g.lanes.data()));
    }
}

// 0x419980 halt_cross_propagate (ECX = cross): halt the cross with delay
// 0.001f on every link, then halt each link lane and expand through its far
// cross. Called only from Traffic__haltPath 0x4196f0.
static void halt_cross_propagate(Graph &g, const Cross *cp)
{
    if (!cp) return;                                    // 0x419980 null check
    Cross &c = const_cast<Cross &>(*cp);
    g.halt_cross(c, 0.001f, true);                      // 0x3a83126f, all = 1
    for (const CrossLink &link : c.links) {
        Lane *lane = lane_at(g, link.node);             // INVENTED: null guard
        if (!lane) continue;
        lane->halted = true;                            // 0x4199bf
        halt_push_far_links(g, *lane);
    }
}

// 0x419ab0 halt_lane_propagate (ECX = lane): the same expansion from one lane,
// without the halt_cross call.
static void halt_lane_propagate(Graph &g, Lane &lane)
{
    lane.halted = true;
    halt_push_far_links(g, lane);
}

// 0x4196f0 Traffic__haltPath (ECX = T; p1, p2): clear every lane halt, flag an
// endpoint that lies inside 0.7 * its cross radius, route p2 -> p1, halt the
// crosses along the route, then drain the lane worklist.
void Graph::halt_path(const float p1[3], const float p2[3])
{
    for (Lane &l : lanes) l.halted = false;             // clear ALL halts first
    bool flag2 = false;
    const Cross *c2 = nearest_cross(p2[0], p2[1], p2[2], 0.0f, true);
    if (c2) {
        float r = 0.7f * c2->radius;                    // +0x34, 0x3f333333
        float dx = c2->pos[0] - p2[0];
        float dy = c2->pos[1] - p2[1];
        float dz = c2->pos[2] - p2[2];
        if (r * r > dx * dx + dy * dy + dz * dz) flag2 = true;
    }
    bool flag1 = false;
    const Cross *c1 = nearest_cross(p1[0], p1[1], p1[2], 0.0f, true);
    if (c1) {
        float r = 0.7f * c1->radius;
        float dx = c1->pos[0] - p1[0];
        float dy = c1->pos[1] - p1[1];
        float dz = c1->pos[2] - p1[2];
        if (r * r > dx * dx + dy * dy + dz * dz) flag1 = true;
    }
    Path P;
    if (find_route(p1, p2, flag1, flag2, &P)) {
        // 0x4198xx: free the worklist (0x567950) before it is refilled.
        halt_worklist.clear();
        halt_cross_propagate(*this,
            nearest_cross(P.start[0], P.start[1], P.start[2], 0.0f, true));
        for (const Segment &s : P.segs) {
            halt_cross_propagate(*this,
                nearest_cross(s.p1[0], s.p1[1], s.p1[2], 0.0f, true));
        }
        // 0x511140 / 0x56a42f free the path; P dies at scope exit.
        // The count is re-read every step: the walk may append to the list.
        for (size_t i = 0; i < halt_worklist.size(); i++) {
            Lane &l = lanes[halt_worklist[i]];
            if (!l.halted) halt_lane_propagate(*this, l);   // 0x419ab0
        }
    }
    halt_worklist.clear();                              // final 0x567950
}

// 0x419ba0 Traffic__haltRoad (ECX = T; p1, p2): halt every lane shared by the
// crosses nearest to p1 and p2. No halt reset, no halt_cross, no routing.
// INVENTED: null-cross guards; the exe dereferences both unchecked.
void Graph::halt_road(const float p1[3], const float p2[3])
{
    const Cross *c1 = nearest_cross(p1[0], p1[1], p1[2], 0.0f, false);
    const Cross *c2 = nearest_cross(p2[0], p2[1], p2[2], 0.0f, false);
    if (!c1 || !c2) return;
    for (size_t i = 0; i < c1->links.size(); i++) {
        Lane *lane = lane_at(*this, c1->links[i].node);      // INVENTED: null guard
        if (!lane) continue;
        for (size_t j = 0; j < c2->links.size(); j++) {
            Lane *other = lane_at(*this, c2->links[j].node); // INVENTED: null guard
            if (!other) continue;
            if (lane == other && !lane->halted) halt_lane_propagate(*this, *lane);
        }
    }
    // shortcut: the exe pushes onto the global worklist here and never drains
    // it (haltRoad has no worklist loop); the entries stay until the next
    // haltPath clears them, as here.
}

// 0x40f290 TrafficCar__remove: unlink the car (GroundRef.remTrafficCar 0x4de520).
void Graph::remove_car(uint32_t handle)
{
    // INVENTED: bounds and freed-slot checks; the exe dereferences the car pointer.
    if (handle == 0 || handle > cars.size()) return;
    const uint32_t ci = handle - 1;
    Car &car = cars[ci];
    if (car.serial == 0) return;   // INVENTED: "not on a lane" (car+0xc == 0); every
                                   // spawned src car is on a lane until released
    lane_unlink(*this, ci);
    // INVENTED: dir/bounds check; the exe tests the owner pointer. A cross owner
    // (dir >= 0) already left the cross list in lane_unlink 0x40ca40 above; its
    // vf4 0x4883e0 is a no-op RET 8, not lane_detach 0x40e710.
    if (car.dir < 0 && car.path_lane < lanes.size()) lane_detach(*this, car.path_lane, ci);
    car.next_lane_a = 0;
    car.next_lane_b = nullptr;
    car.path_lane = 0;
    car.path = nullptr;
    car.kd0 = 0; car.prev_path = nullptr; car.prev_dir = 0;
    car.lane = 0;
    if (car.live_next != 0) {   // in the live list
        if (live_cars == ci + 1) {
            live_cars = car.live_next;
        } else {
            // INVENTED: the walk stops at kLiveEnd (the exe walks into its sentinel
            // and relies on the car being listed).
            uint32_t p = live_cars;
            while (p != kLiveEnd && p != ci + 1) p = cars[p - 1].live_next;
            if (p != kLiveEnd) cars[p - 1].live_next = car.live_next;
        }
        car.live_next = 0;
    }
    car_release(*this, ci);
    free_cars.push_back(ci);
}

// 0x40f6d0 ctLane__setLive: the live switch; cars go live and the road's two
// crosses gain a reference, or the reverse.
void Graph::set_lane_live(size_t li, bool on)
{
    // INVENTED: out-of-range lane index returns (the exe takes a valid pointer).
    if (li >= lanes.size()) return;
    Lane &lane = lanes[li];
    if (lane.live == on) return;   // +0xa8
    live_lanes += on ? 1 : -1;
    lane.live = on;
    // INVENTED: a lane whose road id is not a road skips the cross refcounts
    // (the exe dereferences the table unchecked).
    Road *road = road_by_node(*this, lane.road);
    if (on) {
        if (road) {
            for (int k = 1; k < 3; k++) {
                bool light = false;
                Cross *c = cross_by_node(*this, road->node[k], &light);
                if (!c) continue;   // INVENTED: a null cross id is skipped
                if (c->live_refs == 0) {
                    cross_activate(*this, *c, light);
                    c->live_refs = 1;
                } else if (c->live_refs < 0) {
                    c->live_refs = 1;
                } else {
                    c->live_refs += 1;
                }
            }
        }
        for (uint32_t ci : lane.cars) {   // first to last
            Car &car = cars[ci];
            if (car.live != 1) {
                live_count++;
                if (!car.type_id && car.inst_id && car.notify) {
                    // shortcut: the notify sink (T+0x3130) is not modelled (notes/29 §5)
                }
                car.live = 1;
                if (car.live_next == 0) {   // head insert into the live list
                    car.live_next = live_cars;
                    live_cars = ci + 1;
                }
            }
        }
        if (lane.active_next == 0) {
            lane.active_next = active_lanes;
            active_lanes = (uint32_t)li + 1;
        }
    } else {
        if (road) {
            for (int k = 1; k < 3; k++) {
                Cross *c = cross_by_node(*this, road->node[k], nullptr);
                if (!c) continue;   // INVENTED: a null cross id is skipped
                c->live_refs -= 1;
                if (c->live_refs == 0) c->live_refs = -1;
            }
        }
        for (uint32_t ci : lane.cars) car_drop_live(*this, cars[ci]);
        if (!lane.cars.empty()) {
            float t = car_time_to_lane_end(*this, lane.cars.front());
            if (lane.pending_events == 0) {
                schedule_lane(*this, t, (uint32_t)li);
                lane.pending_events++;
            }
        }
    }
    // 0x410680 with the road's bound object (+0x68) for every car of the lane.
    for (uint32_t ci : lane.cars) car_sync_instance(*this, ci, road ? road->object : 0);
}

// 0x40f8e0 ctRoad__vf1: every lane of the road gets vf7 (0x40eed0), which keeps
// the lane live and stamps its sim time.
void Graph::keep_road_live(size_t road)
{
    // INVENTED: out-of-range road index returns (the exe takes a valid pointer).
    if (road >= roads.size()) return;
    uint32_t id = roads[road].node[0];   // +0x04 first lane
    // INVENTED: cap the pointer walk with the lane count (the exe follows a cycle).
    for (size_t guard = 0; guard < lanes.size(); guard++) {
        const Lane *l = lane_by_node(id);
        if (!l) break;   // INVENTED: a null lane id ends the walk
        size_t li = (size_t)(l - lanes.data());
        set_lane_live(li, true);
        lanes[li].kc0 = sim_time;   // +0xc0
        id = l->node14;             // +0x14
    }
}

// 0x416800 Traffic__spawnCarNear: a link lane of the nearest cross picks the
// road; the lane itself only selects it.
uint32_t Graph::spawn_near(float x, float y, float z, float dist, float p1,
                           float p2, float p3, float p4)
{
    const Cross *cross = nearest_cross(x, y, z, dist, false);
    if (!cross) return 0;

    const size_t n = cross->links.size();
    size_t free = 0, live = 0;
    for (const CrossLink &link : cross->links) {
        const Lane *l = lane_by_node(link.node);
        // INVENTED: a link that is not a lane is neither free nor live (the exe
        // dereferences it).
        if (!l || l->halted) continue;
        free++;
        if (l->live) live++;
    }
    if (free == 0) return 0;

    const uint32_t pick = msvc_rand();   // rand15, one call per selection
    const Lane *lane = nullptr;
    if (free == n && (live == 0 || live == n)) {
        lane = lane_by_node(cross->links[pick % n].node);   // may be halted
    } else if (live == 0) {
        size_t k = pick % free;
        for (const CrossLink &link : cross->links) {
            const Lane *l = lane_by_node(link.node);
            if (!l || l->halted) continue;
            if (k-- == 0) { lane = l; break; }
        }
    } else {
        size_t k = pick % live;
        for (const CrossLink &link : cross->links) {
            const Lane *l = lane_by_node(link.node);
            if (!l || l->halted || !l->live) continue;
            if (k-- == 0) { lane = l; break; }
        }
    }
    // INVENTED: a selection that yields no lane, or a road id outside the road
    // table, returns 0 (the exe dereferences both).
    if (!lane) return 0;
    size_t base = cross_eq.size() + cross_light.size();
    if (lane->road < 1 + base) return 0;
    size_t ri = lane->road - 1 - base;
    if (ri >= roads.size()) return 0;
    return spawn_on_road(ri, p1, p2, p3, p4);
}

// 0x41a3e0 Traffic__bindObject.
uint32_t Graph::bind_object(const std::string &params, uint32_t object)
{
    int32_t kind = 0, graph = 0, idx = 0;
    if (!ped::parse_road_params(params, &kind, &graph, &idx)) return 0;
    if (graph != (int32_t)version) return 0;
    // INVENTED: return 0 for idx outside the pool (the exe indexes unchecked).
    if (idx < 0) return 0;
    if (kind == 1) {
        if ((size_t)idx >= roads.size()) return 0;
        road_bind_object(*this, roads[(size_t)idx], object);
        return (uint32_t)(cross_eq.size() + cross_light.size() + (size_t)idx + 1);
    }
    if (kind == 2) {
        if ((size_t)idx >= cross_eq.size()) return 0;
        cross_bind_object(*this, cross_eq[(size_t)idx], object);
        return (uint32_t)idx + 1;
    }
    if (kind == 3) {
        if ((size_t)idx >= cross_light.size()) return 0;
        cross_bind_object(*this, cross_light[(size_t)idx], object);
        return (uint32_t)(cross_eq.size() + (size_t)idx + 1);
    }
    return 0;
}

// 0x41a000 Traffic__load.
bool Graph::load(const uint8_t *data, size_t size, size_t *consumed, std::string *err)
{
    *this = Graph();
    Reader r{data, size, 0, err, std::string()};

    uint32_t n_eq, n_light, n_road, n_lane;
    r.ctx = "header";
    if (!r.u32(&version) || !r.u32(&n_eq) || !r.u32(&n_light) || !r.u32(&n_road) ||
        !r.u32(&n_lane))
        return false;

    cross_eq.resize(n_eq);
    for (size_t i = 0; i < cross_eq.size(); i++) {
        set_ctx(r, "cross eq", i);
        if (!read_cross(r, cross_eq[i])) return false;
    }
    cross_light.resize(n_light);
    for (size_t i = 0; i < cross_light.size(); i++) {
        set_ctx(r, "cross light", i);
        if (!read_cross(r, cross_light[i])) return false;
    }
    roads.resize(n_road);
    for (size_t i = 0; i < roads.size(); i++) {
        set_ctx(r, "road", i);
        if (!read_road(r, roads[i])) return false;
    }
    lanes.resize(n_lane);
    for (size_t i = 0; i < lanes.size(); i++) {
        set_ctx(r, "lane", i);
        if (!read_lane(r, lanes[i])) return false;
    }

    if (consumed) *consumed = r.off;
    return true;
}

}  // namespace traffic
