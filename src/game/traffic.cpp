#include "game/traffic.h"

#include <math.h>
#include <string.h>

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
    if (!r.u32(&lane.k98) || !r.u32(&lane.k9c) || !r.u32(&lane.ka0)) return false;
    return read_path(r, lane.path);
}

// 0x413360/0x412ed0 share this segment walk.
size_t path_seg(const traffic::Path &p, float *d)
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
    return i;
}

}  // namespace

namespace traffic {

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
    size_t i = path_seg(p, &d);
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
    size_t i = path_seg(p, &d);
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

size_t Graph::node_count() const
{
    return cross_eq.size() + cross_light.size() + roads.size() + lanes.size();
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
