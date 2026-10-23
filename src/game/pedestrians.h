// Pedestrian walk areas of the Invictus ground (exe 0x4d5600 -> 0x41dd70 ->
// 0x41b480 + 0x41d8c0) and the traffic road-object params parser (0x41a3e0).
// Ported from notes/19 §3.1; no walker simulation here.
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <string>
#include <vector>

namespace ped {

struct SplinePoint { float pos[3], nrm[3]; };

struct Edge {                           // cWAreaWall / cWAreaSpline (0x41d8c0)
    int32_t type = 0;                   // 0x17 = spline
    std::vector<SplinePoint> spline;    // p0 + k points (spline only)
    float x1 = 0, z1 = 0, x2 = 0, z2 = 0;   // +0x0c..+0x18
    int32_t link = 0;                   // +0x1c
    float nx = 0, nz = 0;               // +0x20, +0x24
};

struct Area {                           // 0x41df30
    int32_t index = 0;                  // +0x10
    float f14 = 0, f18 = 0, f1c = 0, f20 = 0, f24 = 0;
    std::vector<Edge> edges;            // +4 list, exe order (head first)
    std::vector<int32_t> related;       // +8 list, exe order (head first)
};

// 0x41b480: hex-letter text -> bytes (skips the 8-char prefix).
// INVENTED: a text shorter than the prefix is returned empty.
std::vector<uint8_t> decode_text(const char *text);

// 0x41d8c0. INVENTED: bounds checks (returns false on a short buffer; exe has
// none); *used = bytes consumed.
bool parse_area(const uint8_t *data, size_t size, Area *out, size_t *used);

// 0x4d5600 params split ("%d;" + text after ';'). false when there is no ';'.
bool split_walk_params(const std::string &params, int32_t *index, std::string *text);

struct Manager {                        // Pedestrians, area part only
    std::deque<Area> areas;             // exe list +0x3398, head first; deque keeps Area* valid
    // 0x41dd70/0x41dc10: existing index -> that area, else decode+parse+add.
    const Area *add_area(int32_t index, const char *text);
};

// 0x41a3e0 parse part. INVENTED: bounds check of idx against the pool size.
bool parse_road_params(const std::string &params, int32_t *kind, int32_t *graph,
                       int32_t *idx);

}  // namespace ped
