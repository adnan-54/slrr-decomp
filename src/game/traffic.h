// Road graph of the Invictus traffic system (Traffic__load 0x41a000, notes/19 §2).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <string>
#include <vector>

namespace traffic {

struct Segment {                // 0x414d50, 0x54 bytes in the exe
    float p0[3], p1[3], m0[3], m1[3];
    float len = 0, flat_len = 0;  // +0x30, +0x34
    bool curved = false;          // +0x38
    float width = 0.0f;           // +0x3c
    float k40 = 1.0f;             // +0x40 (f1), set by the loader, use unknown
    int32_t i1 = 0;               // +0x44
    float f2 = 0.0f;              // +0x48
    float f3 = 0.0f;              // +0x4c
    int32_t i2 = 0;               // +0x50
};

struct Path {                   // 0x414b80, 0x3c bytes in the exe
    std::vector<Segment> segs;
    float start[3] = {}, end[3] = {}, start_dir[3] = {}, end_dir[3] = {};
    float length = 0;             // +0x38
};

struct CrossLink { uint32_t node = 0; uint8_t flag = 0; };
struct CrossPath { uint8_t a = 0, b = 0; uint16_t c = 0; uint32_t k58 = 0, k5c = 0; Segment seg; };
struct Cross {                  // ctCross 0x40e850
    std::vector<CrossLink> links; std::vector<CrossPath> paths;
    float pos[3] = {}; uint32_t k30 = 0;
};

struct Road { uint32_t node[3] = {}; float v10[3] = {}, v1c[3] = {}; uint32_t k64 = 0; Path path; };

struct Lane {
    uint32_t road = 0, node14 = 0; uint8_t k18 = 0;       // +0x10 (parent road), +0x14, +0x18
    float v1c[3] = {}, v28[3] = {}; uint32_t k34 = 0, k38 = 0;
    float index_min_x = 0, index_run_max_x = 0, index_max_x = 0; uint32_t index_lane = 0;  // +0x78..+0x84
    uint32_t node88[4] = {}; uint32_t k98 = 0, k9c = 0, ka0 = 0;
    Path path;                                            // +0x3c
    bool halted = false;                                  // +0xc4, INVENTED initial value
};

struct Graph {
    uint32_t version = 0;
    std::vector<Cross> cross_eq, cross_light;   // Traffic +0x10, +0x28
    std::vector<Road> roads;                    // +0x40
    std::vector<Lane> lanes;                    // +0x58
    float cross_stop_offset = 0;                // +0x3140 (ctor 0x416000: 0)
    // Node ids stay raw (1-based into eq, light, road, lane; 0 = null).
    // INVENTED: ids instead of pointers; bounds checks the exe does not have.
    bool load(const uint8_t *data, size_t size, size_t *consumed, std::string *err);
    size_t node_count() const;  // eq + light + road + lane
    // 0x419050: cross_eq first, then cross_light; first strict minimum wins.
    const Cross *nearest_cross(float x, float y, float z, float dist, bool yflag) const;
    void clear_halts();   // 0x417c50: every lane's halt flag (+0xc4) = 0
    // 0x4191d0: x-sorted lane index lookup, then path projection within |dy| < 5.
    const Lane *nearest_lane_pos(float x, float y, float z, float *d, float *lat) const;
    // 0x419410: snap pos onto the nearest lane's road path; dir = path direction.
    bool align_to_road(float pos[3], float dir[3]) const;
};

void segment_pos(const Segment &s, float d, float out[3]);   // 0x4134e0
void segment_dir(const Segment &s, float d, float out[3]);   // 0x412f60
void segment_side(const Segment &s, float d, float out[3]);  // 0x4131a0
void path_pos_at(const Path &p, float d, float out[3]);      // 0x413360
void path_dir_at(const Path &p, float d, float out[3]);      // 0x412ed0
bool segment_project(const Segment &s, float x, float z, float *d, float *lat);  // 0x414480
bool path_project(const Path &p, float x, float z, float *d, float *lat);        // 0x414830
// 0x414fe0 (with 0x4124d0): Hermite arc from the two (pos, dir) pairs.
void segment_build_hermite(Segment &s, const float p0[3], const float d0[3],
                           const float p1[3], const float d1[3]);
// 0x4122b0: append one built segment and its per-row fields to the path.
void path_add_segment(Path &p, const float p0[3], const float d0[3],
                      const float p1[3], const float d1[3], float w, float f1,
                      int32_t i1, float f2, float f3, int32_t i2);
// 0x414050: like path_project without the endpoint fallback.
bool path_project_strict(const Path &p, float x, float z, float *d, float *lat);
void path_pos_offset_at(const Path &p, float d, float off, float out[3]);  // 0x4133f0
// Shared wrap/segment walk; returns the segment index, *local = d within it.
size_t path_seg_at(const Path &p, float *d, float *local);
void path_side_at(const Path &p, float d, float out[3]);   // 0x413110
float path_width_at(const Path &p, float d);               // 0x414a80
float path_f1_at(const Path &p, float d);                  // 0x414980
int32_t path_i1_at(const Path &p, float d);                // 0x414a00
int32_t path_i2_at(const Path &p, float d);                // 0x413fd0
float path_lerp_at(const Path &p, float d);                // 0x413f30

}  // namespace traffic
