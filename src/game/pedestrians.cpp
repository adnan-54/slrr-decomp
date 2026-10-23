#include "game/pedestrians.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

namespace {

// Little-endian reads; INVENTED bounds checks (the exe has none).
bool read_i32(const uint8_t *&p, const uint8_t *end, int32_t *v)
{
    if (end - p < 4) return false;
    uint32_t u;
    memcpy(&u, p, 4);   // x86 target
    p += 4;
    *v = (int32_t)u;
    return true;
}

bool read_f32(const uint8_t *&p, const uint8_t *end, float *v)
{
    if (end - p < 4) return false;
    uint32_t u;
    memcpy(&u, p, 4);
    p += 4;
    memcpy(v, &u, 4);
    return true;
}

bool read_point(const uint8_t *&p, const uint8_t *end, ped::SplinePoint *pt)
{
    for (int i = 0; i < 3; i++)
        if (!read_f32(p, end, &pt->pos[i])) return false;
    for (int i = 0; i < 3; i++)
        if (!read_f32(p, end, &pt->nrm[i])) return false;
    return true;
}

}  // namespace

namespace ped {

// 0x41b480: the 8-char length prefix is skipped, then each letter pair is one
// byte: ((c0 - 1) << 4) | (c1 - 0x41), truncated to 8 bits ('A'..'P' = nibble).
std::vector<uint8_t> decode_text(const char *text)
{
    std::vector<uint8_t> out;
    if (!text) return out;
    size_t n = strlen(text);
    if (n < 8) return out;   // INVENTED: the exe skips 8 chars unchecked
    const char *src = text + 8;
    while (src[0] != 0 && src[1] != 0) {
        out.push_back((uint8_t)(((src[0] - 1) << 4) | (src[1] - 0x41)));
        src += 2;
    }
    return out;
}

// 0x41d8c0: area fields, then n edges (0x17 = spline with a p0 + k points),
// then m related area indices. Edges shorter than 0.0001 are dropped; kept
// edges and related indices are inserted at the head of their lists.
bool parse_area(const uint8_t *data, size_t size, Area *out, size_t *used)
{
    if (used) *used = 0;
    if (!data || size == 0 || !out) return false;
    const uint8_t *p = data;
    const uint8_t *end = data + size;
    out->edges.clear();
    out->related.clear();
    if (!read_f32(p, end, &out->f14) || !read_f32(p, end, &out->f18) ||
        !read_f32(p, end, &out->f1c) || !read_f32(p, end, &out->f20) ||
        !read_f32(p, end, &out->f24))
        return false;
    int32_t n;
    if (!read_i32(p, end, &n)) return false;
    if (n < 0) return false;   // INVENTED
    for (int32_t i = 0; i < n; i++) {
        Edge e;
        if (!read_i32(p, end, &e.type)) return false;
        if (e.type == 0x17) {
            int32_t k;
            if (!read_i32(p, end, &k)) return false;
            if (k < 0) return false;   // INVENTED
            SplinePoint pt;
            if (!read_point(p, end, &pt)) return false;
            e.spline.push_back(pt);
            for (int32_t j = 0; j < k; j++) {
                if (!read_point(p, end, &pt)) return false;
                e.spline.push_back(pt);
            }
        }
        if (!read_f32(p, end, &e.x1) || !read_f32(p, end, &e.z1) ||
            !read_f32(p, end, &e.x2) || !read_f32(p, end, &e.z2) ||
            !read_i32(p, end, &e.link))
            return false;
        float dx = e.x2 - e.x1;
        float dz = e.z2 - e.z1;
        float len = sqrtf(dz * dz + dx * dx);
        if (len >= 0.0001f) {
            float l = sqrtf(dz * dz + dx * dx);   // exe recomputes it
            if (l <= 1e-5f) {
                e.nx = 0;
                e.nz = 0;
            } else {
                float r = 1.0f / l;
                e.nx = -(r * dz);
                e.nz = r * dx;
            }
            out->edges.insert(out->edges.begin(), e);
        }
        // else the exe deletes the edge; its spline points were still read.
    }
    int32_t m;
    if (!read_i32(p, end, &m)) return false;
    if (m < 0) return false;   // INVENTED
    for (int32_t i = 0; i < m; i++) {
        int32_t v;
        if (!read_i32(p, end, &v)) return false;
        out->related.insert(out->related.begin(), v);
    }
    if (used) *used = (size_t)(p - data);
    return true;
}

// 0x4d5600: obj+0x1c is sscanf("%d;") of params and only a missing ';' stops
// the exe; a failed conversion leaves the index at its zero initial value.
bool split_walk_params(const std::string &params, int32_t *index, std::string *text)
{
    size_t semi = params.find(';');   // 0x569330 strchr
    if (semi == std::string::npos) return false;
    int32_t idx = 0;
    sscanf(params.c_str(), "%d;", &idx);   // 0x569380
    if (index) *index = idx;
    if (text) *text = params.substr(semi + 1);
    return true;
}

// 0x41dd70 -> 0x41dc10: 0x400-bucket hash in the exe; linear search here.
const Area *Manager::add_area(int32_t index, const char *text)
{
    for (const Area &a : areas)
        if (a.index == index) return &a;   // existing area: data is not re-parsed
    Area area;
    area.index = index;
    std::vector<uint8_t> data = decode_text(text);
    size_t used = 0;
    // INVENTED: the exe has no parse failure path; a failed parse still keeps
    // the (partly filled) area.
    if (!data.empty()) parse_area(data.data(), data.size(), &area, &used);
    areas.insert(areas.begin(), area);   // exe pushes at the head of +0x3398
    return &areas.front();
}

// 0x41a3e0: sscanf("%d;%d,%d") — kind, graph version, pool index.
bool parse_road_params(const std::string &params, int32_t *kind, int32_t *graph,
                       int32_t *idx)
{
    int32_t k, g, i;
    if (sscanf(params.c_str(), "%d;%d,%d", &k, &g, &i) != 3) return false;
    if (kind) *kind = k;
    if (graph) *graph = g;
    if (idx) *idx = i;
    return true;
}

}  // namespace ped
