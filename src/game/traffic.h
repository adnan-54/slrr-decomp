// Road graph of the Invictus traffic system (Traffic__load 0x41a000, notes/19 §2).
#pragma once
#include <stddef.h>
#include <stdint.h>

#include <deque>
#include <string>
#include <vector>

namespace traffic {

// End of the live-car list. INVENTED encoding: the exe's sentinel is the Traffic
// object itself (T+0x88 = T, 0x416000), src uses a reserved car handle.
constexpr uint32_t kLiveEnd = 0xffffffffu;

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
// Per-path claim state at cross+0x58 (notes/37 §2).
struct PathState { int32_t count = 0; int32_t block = 0; };
struct Cross {                  // ctCross 0x40e850
    std::vector<CrossLink> links; std::vector<CrossPath> paths;
    float pos[3] = {}; uint32_t k30 = 0;
    float radius = 0;                 // +0x34 (cross_init 0x415650)
    int32_t live_refs = 0;            // +0x38 live-lane refcount, -1 = going inactive
    uint32_t active_next = 0;         // +0x54 (eq) / +0x60 (light)
    // +0x44 ResRefLink. INVENTED: id of the bound object's instance, 0 = none.
    uint32_t object = 0;
    std::vector<PathState> path_state; // +0x58, one zeroed pair per path (count +0x20)
    uint32_t cycle = 0;               // +0x5c (light_init 0x415700 zeroes it)
    float light_remain = 0;           // +0x64 (light only, ctor 0x4154b0)
    int32_t light_cur = 0;            // +0x68 current phase (light only)
    float phase_time = 0;             // +0x6c (light only)
    std::vector<int32_t> phase;       // +0x70 (light only)
    int32_t phase_count = 0;          // +0x74 (light only)
};

struct Road { uint32_t node[3] = {}; float v10[3] = {}, v1c[3] = {}; uint32_t k64 = 0; Path path;
              // +0x68 ResRefLink. INVENTED: id of the bound object's instance, 0 = none.
              uint32_t object = 0; };

struct Lane {
    uint32_t road = 0, node14 = 0; uint8_t k18 = 0;       // +0x10 (parent road), +0x14, +0x18
    float v1c[3] = {}, v28[3] = {}; uint32_t k34 = 0, k38 = 0;
    float index_min_x = 0, index_run_max_x = 0, index_max_x = 0; uint32_t index_lane = 0;  // +0x78..+0x84
    uint32_t node88[4] = {}; float k98 = 0; uint32_t k9c = 0, ka0 = 0;  // +0x98 speed factor
    Path path;                                            // +0x3c
    std::vector<uint32_t> cars;                           // +0x08/+0x0c list; INVENTED: exe intrusive prev/next replaced by car indices, ascending pos
    uint32_t active_next = 0;                             // +0xa4 (0 = not listed, kLiveEnd = list end)
    bool live = false;                                    // +0xa8
    uint32_t pending_events = 0;                          // +0xac (0 = no timed event scheduled)
    // +0xb0 weak ref to the car blocking this lane. INVENTED: 1-based car index,
    // 0 = null (a pointer in the exe).
    struct Blocker { uint32_t serial = 0, car = 0; uint8_t k = 0; };
    Blocker blocker;
    bool halted = false;                                  // +0xc4, INVENTED initial value
    float kc0 = 0, kc8 = 0, kcc = 0;                      // +0xc0 sim time, +0xc8, +0xcc path length
};

struct LeaderSlot {             // car+0x48..0x84, 4 x 16 bytes (0x40d2c0)
    uint32_t serial = 0;        // +0x00 serial copy (validity check)
    uint32_t car = 0;           // +0x04 INVENTED: 1-based car index, 0 = empty (a Car* in the exe)
    uint8_t flag = 0;           // +0x08 0 = lane leader, 1 = target-lane slot
    float dist = 0.0f;          // +0x0c
};

struct Car {                    // 0x164 bytes in the exe
    uint32_t serial = 0;        // +0x00 (++g_car_serial; also the free-list link)
    uint32_t prev = 0, next = 0;  // +0x04, +0x08 (exe intrusive lane links; unused here)
    uint32_t lane = 0;          // +0x0c (INVENTED: lane index, pointer in the exe)
    float k14 = 0, k18 = 0, k1c = 0, k20 = 0, k24 = 0, k28 = 0;  // +0x14..+0x28
    float max_speed = 0;        // +0x2c
    float accel = 0;            // +0x30
    float k34 = 0, k38 = 0, k3c = 0;  // +0x34..+0x3c
    int32_t k40 = 0;            // +0x40 (ctor 0x40c400 stores the int -1)
    uint32_t live_next = 0;     // +0x44 (0 = not listed, kLiveEnd = list end)
    LeaderSlot leaders[4];      // +0x48 leader slots (0x40d2c0 / 0x410920)
    float pos = 0;              // +0x88 (distance on the lane path; counts down to the lane end)
    float world[3] = {};        // +0x8c
    float front_x = 0, front_z = 0;  // +0x98, +0x9c
    float heading_x = 0, heading_z = 0;  // +0xa0, +0xa4 (0x4102d0)
    float lateral = 0;          // +0xa8
    float kac = 0;              // +0xac
    float speed = 0;            // +0xb0
    float kb4 = 0;              // +0xb4
    // +0xb8/+0xbc/+0xc0 next ref {owner, path, idx} (notes/37 §3). INVENTED:
    // the owner is a lane index (a pointer in the exe).
    uint32_t next_lane_a = 0;          // +0xb8 next owner
    const Path *next_lane_b = nullptr; // +0xbc next path (&lane.path)
    int32_t kc0 = 0;                   // +0xc0 next idx (-1 = lane)
    uint32_t path_lane = 0;     // +0xc4 PathRef lane (INVENTED: index)
    const Path *path = nullptr; // +0xc8 (&lane.path)
    int32_t dir = 0;            // +0xcc
    uint32_t kd0 = 0;                // +0xd0 prev ref lane/owner
    const Path *prev_path = nullptr; // +0xd4 prev ref path
    int32_t prev_dir = 0;            // +0xd8 prev ref dir
    uint8_t live = 0;           // +0xdc
    uint8_t notify = 0;         // +0xdd
    float e0 = 0, e4 = 0, e8 = 0;  // +0xe0..+0xe8 step times
    uint32_t k124 = 0, k128 = 0;   // +0x124, +0x128
    float k12c = 0.0f;             // +0x12c lane-change rate
    uint32_t k130 = 0, k134 = 0, inst_id = 0, k13c = 0;  // +0x130 instance link (id +0x138)
    uint32_t k140 = 0;          // +0x140
    uint32_t k144 = 0, k148 = 0, type_id = 0, k150 = 0;  // +0x144 type link (id +0x14c)
    uint32_t seed = 0;          // +0x154
    uint32_t k158 = 0;          // +0x158
    uint32_t k15c = 0;          // +0x15c
    uint32_t behaviour = 0;     // +0x160
};

struct Graph {
    uint32_t version = 0;
    std::vector<Cross> cross_eq, cross_light;   // Traffic +0x10, +0x28
    std::vector<Road> roads;                    // +0x40
    std::vector<Lane> lanes;                    // +0x58
    float sim_time = 0;                         // +0x84
    // global 0x5e6518 front-point mode of 0x4102d0 (0/1/2). INVENTED: per graph,
    // initial 0 (BSS); the setter is not traced.
    int32_t front_mode = 0;
    std::vector<uint8_t> road_enabled;          // +0xc4: enable flag per road group
    float max_road_width = 0;                   // +0x312c
    float cross_stop_offset = 0;                // +0x3140 (ctor 0x416000: 0)
    // +0x00 lane event min-heap (schedule 0x417050, push 0x41a950). INVENTED:
    // std::vector grows geometrically where the exe's array grows by 0x400 entries;
    // lanes are indices (pointers in the exe).
    struct HeapEntry { float time; uint32_t lane; };
    std::vector<HeapEntry> heap;
    // +0x88 head of the live-car list (link car+0x44). INVENTED: 1-based car index,
    // kLiveEnd = end (a pointer in the exe).
    uint32_t live_cars = kLiveEnd;
    int32_t live_count = 0;                     // global 0x5e6514; INVENTED: per graph
    // +0x8c/+0x90/+0x94 heads of the active lane/cross lists. INVENTED: 1-based
    // indices into lanes / cross_eq / cross_light (kLiveEnd = end), like live_cars.
    uint32_t active_lanes = kLiveEnd;
    uint32_t active_eq = kLiveEnd;
    uint32_t active_light = kLiveEnd;
    int32_t live_lanes = 0;                     // global 0x5e6500; INVENTED: per graph
    // +0x70 car pool. INVENTED: deque gives stable addresses, the free list keeps
    // indices, and the serial counter is per graph (the exe's is process-wide).
    std::deque<Car> cars;
    std::vector<uint32_t> free_cars;
    uint32_t car_serial = 0;
    // Node ids stay raw (1-based into eq, light, road, lane; 0 = null).
    // INVENTED: ids instead of pointers; bounds checks the exe does not have.
    bool load(const uint8_t *data, size_t size, size_t *consumed, std::string *err);
    void init(float t);   // 0x419da0: per-node init pass after Traffic__load
    void step(float t);   // 0x41b290: sim_time = t (+0x84); car pool not ported
    // 0x40ce00: spawn an abstract car on a free lane of road; returns index + 1 (0 = none).
    uint32_t spawn_on_road(size_t road, float p1, float p2, float p3, float p4);
    // 0x40f290 TrafficCar__remove: unlink from lane and live list, release the slot.
    void remove_car(uint32_t handle);
    // 0x416800: spawn on a lane of the nearest cross; the cross picks the road.
    uint32_t spawn_near(float x, float y, float z, float dist, float p1, float p2,
                        float p3, float p4);
    // 0x41a3e0 Traffic__bindObject: params "<kind>;<graph>,<idx>" bind and link
    // the object; returns the node id (0 = none / out of pool).
    uint32_t bind_object(const std::string &params, uint32_t object);
    size_t node_count() const;  // eq + light + road + lane
    // INVENTED: node id -> lane with a bounds check (the exe dereferences it).
    const Lane *lane_by_node(uint32_t id) const;
    // 0x419050: cross_eq first, then cross_light; first strict minimum wins.
    const Cross *nearest_cross(float x, float y, float z, float dist, bool yflag) const;
    void clear_halts();   // 0x417c50: every lane's halt flag (+0xc4) = 0
    // 0x40f6d0 ctLane__setLive: cars and the road's two cross refcounts follow.
    void set_lane_live(size_t lane, bool on);
    // 0x40f8e0 ctRoad__vf1 -> 0x40eed0 lane vf7: keep every lane of the road live.
    void keep_road_live(size_t road);
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
