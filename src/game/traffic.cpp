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

}  // namespace

namespace traffic {

size_t Graph::node_count() const
{
    return cross_eq.size() + cross_light.size() + roads.size() + lanes.size();
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
