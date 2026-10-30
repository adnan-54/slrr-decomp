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
    if (!r.u32(&road.k64)) return false;
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
    if (!r.f32(&lane.k98) || !r.u32(&lane.k9c) || !r.u32(&lane.ka0)) return false;
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
    float nrm[2] = {-s.m1[0], -s.m1[2]};
    n = sqrtf(nrm[0] * nrm[0] + nrm[1] * nrm[1]);
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
    float pos[3], d3[3], side[3];
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
    *lat = (x - pos[0]) * side[0] + (z - pos[2]) * side[2];   // pos from the last evaluation
    return true;
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

// INVENTED: node id -> cross with a bounds check (the exe dereferences the table).
Cross *cross_by_node(Graph &g, uint32_t id, bool *is_light)
{
    if (id == 0) return nullptr;
    if (id <= g.cross_eq.size()) {
        if (is_light) *is_light = false;
        return &g.cross_eq[id - 1];
    }
    size_t i = id - 1 - g.cross_eq.size();
    if (i < g.cross_light.size()) {
        if (is_light) *is_light = true;
        return &g.cross_light[i];
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

// 0x40c730 TrafficCar__release: unlink the refs and clear the serial; the
// caller pushes the slot on the free list.
void car_release(Graph &g, uint32_t i)
{
    g.cars[i].serial = 0;
    // INVENTED: the +0x130 instance and +0x144 type link unlinks are not modelled.
}

// 0x40dc00 TrafficCar__targetSpeed: lane speed limit and next-lane/cross objects.
float car_target_speed(const Graph &g, const Car &car)
{
    float s = car.max_speed;
    // +0xc4 PathRef lane. INVENTED: bounds check; an index (a pointer in the exe).
    if (car.path_lane < g.lanes.size() && g.lanes[car.path_lane].k98 < s)
        s = g.lanes[car.path_lane].k98;   // ctLane vf6 0x40dd50
    float f = 0.0f;
    if (!car.next_lane_b) {          // +0xbc, next lane
        if (car.k124) {
            // shortcut: +0x124 vf6(-1) not modelled (a cross's vf6 0x40dd00 would read
            // paths[-1]); unreached: no city.trc lane has +0x34 == -1, so attach never sets +0x124
            s = (0.0f + s) * 0.5f;
        }
    } else {
        f = car.max_speed;
        if (car.next_lane_a) {       // +0xb8, cross object (kb8 in the exe)
            // shortcut: vf6 of next-lane/cross objects not traced; 0 for spawned cars
            f = 0.0f;
            return f <= s ? s : f;
        }
    }
    return f <= s ? s : f;
}

// 0x4107c0 TrafficCar__step; defined after lane_detach (mutual recursion:
// step 6 -> lane_attach/lane_detach -> car_time_to_lane_end -> car_step).
void car_step(Graph &g, uint32_t ci);

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

// 0x40cbd0 ctLane__vf3: live/not-live bookkeeping, first-event scheduling and
// occupancy. The exe's tail-append branch is a no-op: insert already set car.lane.
void lane_attach(Graph &g, size_t li, uint32_t ci)
{
    Lane &lane = g.lanes[li];
    Car &car = g.cars[ci];

    if (lane.blocker.car == ci + 1 && lane.blocker.serial == car.serial)
        lane.blocker = Lane::Blocker();

    if (!lane.live) {
        if (car.live) {
            g.live_count--;
            if (!car.type_id && car.inst_id && car.notify) {
                // shortcut: the notify sink (T+0x3130) is not modelled (notes/29 §5)
            }
            car.live = 0;
            car.k128 = 0;
        }
        if (!lane.cars.empty() && lane.cars.front() == ci) {
            float t = car_time_to_lane_end(g, ci);
            if (lane.pending_events == 0) {
                schedule_lane(g, t, (uint32_t)li);
                lane.pending_events++;
            }
        }
    } else if (car.live != 1) {
        g.live_count++;
        if (!car.type_id && car.inst_id && car.notify) {
            // shortcut: the notify sink (T+0x3130) is not modelled (notes/29 §5)
        }
        car.live = 1;
        if (car.live_next == 0) {   // head insert into the live list
            car.live_next = g.live_cars;
            g.live_cars = ci + 1;
        }
    }
    // shortcut: TrafficCar__syncInstance 0x410680 needs the instance side (notes/29 §5)
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
        // shortcut: ctSegment__posOffset 0x413660 (cross path, ref->dir >= 0) has
        // no src equivalent; no src car is on a cross (dir is only ever -1).
        return;
    }
    path_pos_offset_at(*path, d, off, out);      // 0x4133f0 ctPath__posOffsetAt
}

// 0x40dd60 CarPathRef__dir (ECX = ref): path direction at d.
void car_ref_dir(const Path *path, int32_t dir, float d, float out[3])
{
    out[0] = out[1] = out[2] = 0.0f;
    if (!path) return;
    if (dir >= 0) {
        // shortcut: ctSegment__dir 0x412f60 (cross path, ref->dir >= 0) is not
        // reached: no src car is on a cross (dir is only ever -1).
        return;
    }
    path_dir_at(*path, d, out);                  // 0x412ed0 ctPath__dirAt
}

// 0x40e810 CarPathRef__length (ECX = ref): current path length.
float car_ref_length(const Path *path, int32_t dir)
{
    if (!path) return 0.0f;
    if (dir >= 0) {
        // shortcut: the cross path length read (ref + 0x30 = Segment::len) is not
        // reached: no src car is on a cross (dir is only ever -1).
        return 0.0f;
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

// 0x40f350 TrafficCar__liveStep: a live car stays listed while its current lane
// is live; the cross-claim branches (idx >= 0) have no src state.
bool car_live_step(Graph &g, Car &car)
{
    if (!car.live) {              // +0xdc
        car_push_instance(g, car);    // 0x4118f0 -> 0x4102d0
        return false;
    }
    // shortcut: cross owners (idx >= 0) are not representable in src until
    // TrafficCar__step 0x4107c0 hands cars to crosses (skip 0x415b40/0x415b00)
    // INVENTED: a car not on a lane (dir >= 0) counts as not live.
    const bool cur_live = car.dir < 0 && car.path_lane < g.lanes.size() &&
                          g.lanes[car.path_lane].live;   // lane vf1 0x40e390 = +0xa8
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

// 0x40ca40 TrafficCar__unlinkLane: unlink from the lane's car list.
// INVENTED: the vector erase replaces the exe's intrusive prev/next pointers, and
// the exe has no lane bounds check (it trusts the lane pointer).
void lane_unlink(Graph &g, uint32_t ci)
{
    Car &car = g.cars[ci];
    if (car.lane < g.lanes.size()) {
        std::vector<uint32_t> &list = g.lanes[car.lane].cars;
        std::vector<uint32_t>::iterator it = std::find(list.begin(), list.end(), ci);
        if (it != list.end()) list.erase(it);
    }
    car.prev = car.next = car.lane = 0;
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

// 0x4107c0 TrafficCar__step (ECX = car): lane motion slice - entry gate, step 1
// free distance, step 5 speed integration, step 6 ref hand-over. Steps 2/3/4
// (leaders, obstacles, lane change) and 7/8 (projection, registration) are later
// cards; they only fill leader_speed / free / car.k128.
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
        // shortcut: the cross path +0x30 length has no src field; no src car
        // leaves a lane.
    }
    float target = car_target_speed(g, car);   // 0x41086c -> 0x40dc00
    float leader_speed = -1.0f;                // [EBP-0x20], 0x5935ac
    // shortcut: steps 2/3/4 (leader slots 0x40d2c0/0x40daa0, obstacle scan
    // 0x40eff0, lane-change decision 0x40fde0) are later cards; they fill
    // leader_speed, free and car.k128. g_brake (0x410888) is only read there.
    float free = d0;                           // [EBP-0x8]

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
                car.pos += car.dir < 0 ? car.path->length : 0.0f;   // +0x38
            // shortcut: the cross path +0x30 length has no src field; no src car
            // leaves a lane.
            lane_unlink(g, ci);                        // 0x4115bb -> 0x40ca40
            // 0x40cbd0 append branch: the hand-over appends itself at the lane
            // tail (lane_attach assumes lane_insert did).
            // INVENTED: lane id bounds check (the exe dereferences the owner).
            if (car.path_lane < g.lanes.size()) {
                g.lanes[car.path_lane].cars.push_back(ci);
                car.lane = car.path_lane;
                lane_attach(g, car.path_lane, ci);     // 0x4115c0 cur owner vf3
            }
            lane_detach(g, prev_lane, ci);             // 0x4115d2 prev owner vf4
            car.k12c = 0.0f;                           // 0x4115e4
        }
    }
    // shortcut: steps 7/8 (lane-change completion, target-lane registration
    // 0x4115f1..) are later cards.
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

// 0x40ed30 ctRoad__bindObject.
void road_bind_object(Graph &g, Road &road, uint32_t object)
{
    road.object = object;                       // +0x68 ResRefLink
    Lane *l = lane_at(g, road.node[0]);
    // INVENTED: cap the pointer walk with the lane count (the exe follows a cycle).
    for (size_t guard = 0; l && guard < g.lanes.size(); guard++) {
        for (uint32_t ci : l->cars) {
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
                    // shortcut: Traffic__parkInstance 0x41a500 and the +0x130
                    // type link unlink are not modelled.
                    car.inst_id = 0;
                    // shortcut: counters 0x5e64f4/0x5e64fc are not modelled.
                }
            } else if (inst) {
                // shortcut: 0x4ec210 reparent under obj is not ported.
            } else {
                // shortcut: counters 0x5e64f4/0x5e64f8 and Traffic__queueSpawn
                // 0x41a500 are not modelled.
            }
        }
        l = lane_at(g, l->node14);
    }
}

// 0x40ebb0 ctCross__bindObject.
void cross_bind_object(Cross &cross, uint32_t object)
{
    cross.object = object;   // +0x44 ResRefLink
    // shortcut: the exe walks cross+8 car links with the same per-car body as
    // 0x40ed30; src keeps cars only on lanes (card 216), so the loop is empty.
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
            // shortcut: the next-lane move 0x40e460 is not ported.
        }
        return true;
    }
    g.set_lane_live(li, false);                      // 0x40f6d0 ctLane__setLive
    return false;
}

}  // namespace

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
    // shortcut: the pseudo-car prologue stores, the lane event heap pop
    // (ctLane__timedEvent), the notify queue and the T+0xc8 drain are not ported.
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
    // INVENTED: path_lane is an index where 0 is a valid lane; the exe tests the pointer.
    if (car.path_lane < lanes.size()) lane_detach(*this, car.path_lane, ci);
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
    // shortcut: TrafficCar__syncInstance 0x410680 (road+0x68) is not modelled.
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
        cross_bind_object(cross_eq[(size_t)idx], object);
        return (uint32_t)idx + 1;
    }
    if (kind == 3) {
        if ((size_t)idx >= cross_light.size()) return 0;
        cross_bind_object(cross_light[(size_t)idx], object);
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
