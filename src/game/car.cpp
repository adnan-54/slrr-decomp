// Car / part payloads behind the Chassis, WheelRef and Part natives.
// Offsets, defaults and setter formulas: notes/13-car-structs.md §6-§8 and the
// native summaries in §12. Wheels are built from the car cfg `type`/`spring`/
// `wheel`/`friction`/`wheelpars` lines (createCar), slots from the part cfg.
#include "game/car.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <memory>
#include <set>
#include <utility>

#include "input/controller.h"
#include "mesh/scx.h"
#include "parts/cfg.h"
#include "physics/mesh.h"
#include "physics/vclip.h"
#include "resource/manager.h"
#include "script/natives.h"
#include "script/vm.h"
#include "vehicle/dyno.h"

// 0x7a75df: Steam.neutralize flag; createCar 0x49a4ef reads it.
extern bool steam_neutralized;

namespace game {
namespace {

// Wheel__ctor / createCar defaults, notes/13 §7 and the Pacejka block at
// 0x5da8b0.
const float kPacejkaDefaults[kPacejkaSlots] = {
    1.4f, 0.0f, 1.49f, 0.0f, 15.2f, 0.0f, -0.0f, 0.0f, -1.0f,
    0.0f, 0.0f, 8000.0f, 1.0f, 0.015f, 0.4f, 0.0f, 0.0f,
};

// 0x4b0ff0 health->factor: 1 - w^2*0.6, floored at 0.100000024 when w >= 1.
float wear_factor(float w)
{
    if (w >= 1.0f) return 0.100000024f;
    return 1.0f - w * w * 0.6f;
}

float num(const std::string &s)
{
    return (float)strtod(s.c_str(), nullptr);
}

// Point-mass parallel-axis term of 0x5032d0: m * (|r|^2 E - r r^T).
void add_point_inertia(Mat34 *inertia, float m, Vec3 r)
{
    float r2 = vec_dot(r, r);
    inertia->m[0] += m * (r2 - r.x * r.x);
    inertia->m[1] -= m * r.x * r.y;
    inertia->m[2] -= m * r.x * r.z;
    inertia->m[4] -= m * r.y * r.x;
    inertia->m[5] += m * (r2 - r.y * r.y);
    inertia->m[6] -= m * r.y * r.z;
    inertia->m[8] -= m * r.z * r.x;
    inertia->m[9] -= m * r.z * r.y;
    inertia->m[10] += m * (r2 - r.z * r.z);
}

Runtime *g_rt = nullptr;
std::map<Runtime::Resource *, std::unique_ptr<PartData>> g_parts;
std::map<Runtime::Resource *, std::unique_ptr<CarData>> g_cars;
std::map<Object *, WheelData *> g_wheels;
std::map<Object *, audio::SfxTable *> g_sfx_tables;
std::map<Object *, std::unique_ptr<EngineModel>> g_dynos;
// Native-player instances (type record's `native player`, no cfg). Their vf14
// swallows every command; the instance's type is not reachable from the node,
// so the flag is recorded when the type manager runs (instantiate).
std::set<Runtime::Resource *> g_players;

// The cfg ref sits on the record's native line, possibly on a super record.
bool load_native_cfg(Runtime &rt, Runtime::Resource *r, const char *native,
                     CfgFile *cfg, const Package **pkg = nullptr)
{
    for (Runtime::Resource *n = r; n; n = n->parent) {
        if (!n->package || !n->record) continue;
        for (const DataRef &ref : rt.resources().refs(*n->package, *n->record)) {
            if (ref.kind != DataRef::Native || ref.native != native ||
                !ref.is_cfg || !ref.file || ref.path.empty())
                continue;
            std::string err;
            if (!rt.resources().load_cfg(ref, cfg, &err)) continue;
            if (pkg) *pkg = n->package;
            return true;
        }
    }
    return false;
}

// Same walk, for the native handlers without a cfg (0x48dee0's `player`).
bool has_native_ref(Runtime &rt, Runtime::Resource *r, const char *native)
{
    for (Runtime::Resource *n = r; n; n = n->parent) {
        if (!n->package || !n->record) continue;
        for (const DataRef &ref : rt.resources().refs(*n->package, *n->record))
            if (ref.kind == DataRef::Native && ref.native == native && !ref.is_cfg)
                return true;
    }
    return false;
}

// Type slots: the part manager's list (0x4c6f90 nodes). The car manager embeds
// the same list, so the car cfg goes through here too (card 150).
// 0x4bfe90 (attach) / 0x4bffb0 (compatible): the part id is stored through
// 0x51e860 in the cfg's package, so 0x4c9d90 compares global type ids.
void fill_slots(Runtime &rt, const Package *pkg, PartData &p, const CfgFile &cfg)
{
    for (const CfgSlot &cs : cfg.slots) {
        Slot s;
        s.id = cs.id;
        s.slot_type = cs.slot_type;
        s.type_pos[0] = cs.xform.px;
        s.type_pos[1] = cs.xform.py;
        s.type_pos[2] = cs.xform.pz;
        float ang[3] = {cs.xform.rx, cs.xform.ry, cs.xform.rz};
        euler_to_mat(&s.type_rot, ang);
        for (int i = 0; i < 3; i++) s.pos[i] = s.type_pos[i];
        s.rot = s.type_rot;
        s.attaches = cs.attaches;
        s.compatibles = cs.compatibles;
        if (pkg) {
            for (CfgAttach &a : s.attaches) a.part = rt.resources().global_id(*pkg, a.part);
            for (CfgAttach &a : s.compatibles) a.part = rt.resources().global_id(*pkg, a.part);
        }
        p.slots.push_back(std::move(s));
    }
}

void fill_part(Runtime &rt, const Package *pkg, PartData &p, const CfgFile &cfg)
{
    if (!cfg.textures.empty()) p.texture = (int32_t)cfg.textures[0].id;
    if (!cfg.meshes.empty()) p.mesh = (int32_t)cfg.meshes[0].id;
    if (!cfg.renders.empty()) p.render_type = (int32_t)cfg.renders[0].id;
    // INVENTED: which cfg body line P+0x1c binds to is UNRESOLVED; use the first.
    // 0x4bf4d0 passes this mass to Physics__createBody, whose inverse mass is 1/m.
    if (!cfg.bodies.empty() && cfg.bodies[0].mass > 0.0f)
        p.inv_mass = 1.0f / cfg.bodies[0].mass;
    fill_slots(rt, pkg, p, cfg);
}

// createCar wheel template: one record per `type` line, later lines refine the
// current one (notes/13 §6).
void fill_car(CarData &c, const CfgFile &cfg)
{
    for (const CfgLine &ln : cfg.lines) {
        // 0x48dee0 / 0x48e070: control slots M+0x17ec, driver slot M+0x17f0.
        // M+0x120 is taken as 0 (UNRESOLVED; same in notes/09).
        if (ln.key == "controller" || ln.key == "driver") {
            if (c.control_slots < 4) {
                if (c.control_slots == 0) c.driver_slot = 0;
                if (ln.key == "driver") c.driver_slot = c.control_slots - 1;
                c.control_slots++;
            }
            continue;
        }
        if (ln.key == "nodriver") {
            c.driver_slot = -1;
            continue;
        }
        if (ln.key == "type") {
            WheelData w;
            w.index = (int32_t)c.wheels.size();
            w.car = &c;
            if (ln.values.size() >= 1) w.bone_mass = num(ln.values[0]);
            if (ln.values.size() >= 3) w.radius = num(ln.values[2]);
            c.wheels.push_back(w);
            continue;
        }
        if (c.wheels.empty()) continue;
        WheelData &w = c.wheels.back();
        if (ln.key == "spring" && ln.values.size() >= 3) {
            float base = num(ln.values[0]), travel = num(ln.values[1]);
            if (base <= 0.0f) base = 0.1f;
            if (travel <= 0.0f) travel = 0.2f;
            w.template_len = base;
            w.template_travel = travel;
            w.damp_ratio = num(ln.values[2]);  // +0x40
        } else if (ln.key == "wheel" && ln.values.size() >= 10) {
            for (int i = 0; i < 3; i++) w.template_pos[i] = num(ln.values[i]);
            for (int i = 0; i < 3; i++) w.template_ypr[i] = num(ln.values[3 + i]);
            w.steer = num(ln.values[6]);
            w.drive = num(ln.values[7]);
            w.brake = fabsf(num(ln.values[8]));
            w.hbrake = fabsf(num(ln.values[9]));
            w.drive_ratio = 1.0f;
            w.template_damage = 27.0f;
        } else if (ln.key == "friction" && !ln.values.empty()) {
            w.friction = num(ln.values[0]);
        } else if (ln.key == "wheelpars" && ln.values.size() >= 9) {
            static const int slots[9] = {0, 2, 4, 11, 12, 13, 14, 15, 16};
            for (int i = 0; i < 9; i++) w.pacejka[slots[i]] = num(ln.values[i]);
        }
    }
    for (size_t i = 0; i < c.wheels.size(); i++) {
        WheelData &w = c.wheels[i];
        w.base_len = w.template_len;
        w.travel = w.template_travel;
        w.rest_len = w.template_len + w.template_travel;
        w.max_len = w.template_len + 0.1f;
        w.min_len = -1.0f;
        w.free_len = w.rest_len + w.radius;
        w.friction_eff = w.friction * wear_factor(w.tyre_wear);
        w.brake_eff = w.brake * wear_factor(w.brake_wear);
        w.hbrake_eff = w.hbrake * wear_factor(w.brake_wear);
        w.drive_eff = w.drive;
        w.roll_res = 0.015f;
        w.damage_visual = w.template_damage;
        w.hub[0] = w.template_pos[0];
        w.hub[1] = w.template_pos[1] + w.base_len + w.travel;
        w.hub[2] = w.template_pos[2];
        euler_to_mat(&w.rot, w.template_ypr);
        w.pose = w.rot;
        w.half_width = 0.1f;
        size_t j = i ^ 1;  // opposite wheel
        w.opp = j < c.wheels.size() ? &c.wheels[j] : nullptr;
    }
}

// 0x4a2dd0 Engine__torqueCurve: 0 outside [first, last), else lerp.
float curve_at(const std::vector<float> &rpm, const std::vector<float> &val,
               float q)
{
    size_t n = rpm.size();
    if (n == 0 || val.size() < n || n <= 1 || q < rpm[0]) return 0.0f;
    size_t i = 1;
    while (rpm[i] <= q) {
        i++;
        if (n <= i) return 0.0f;
    }
    // exe order: (dv * dq) / drpm + v0
    return (val[i] - val[i - 1]) * (q - rpm[i - 1]) / (rpm[i] - rpm[i - 1]) +
           val[i - 1];
}

// 0x59b438 custom scanner: skip the command word, then strtol base 0 (the
// "0x%x" ids of "activate" and the signed decimal ids of "controllable").
bool scan_id(const std::string &cmd, const char *word, uint32_t *id)
{
    size_t n = strlen(word);
    if (cmd.compare(0, n, word) != 0) return false;
    const char *p = cmd.c_str() + n;
    while (*p == ' ' || *p == '\t') p++;
    char *end = nullptr;
    long v = strtol(p, &end, 0);
    if (end == p) return false;
    *id = (uint32_t)v;
    return true;
}

// ---- part install (notes/13 §15) ------------------------------------------

Slot *slot_by_part(PartData *p, int32_t id)
{
    if (!p) return nullptr;
    for (Slot &s : p->slots)
        if (s.id == id) return &s;
    return nullptr;
}

// Type record behind an instance. 0x4c9d90 compares the refs' type ids
// (`pkg<<16|local`), not the instance ids.
Runtime::Resource *part_type_of(Runtime &, Runtime::Resource *r)
{
    if (!r) return nullptr;
    auto pit = g_parts.find(r);
    if (pit != g_parts.end() && pit->second->type) return pit->second->type;
    auto cit = g_cars.find(r);
    if (cit != g_cars.end() && cit->second->cfg_resource)
        return cit->second->cfg_resource;
    return r->record ? r : nullptr;  // the type record itself
}

uint32_t part_type_id(Runtime &rt, Runtime::Resource *r)
{
    Runtime::Resource *t = part_type_of(rt, r);
    if (!t) return 0;
    if (t->record && t->package) return rt.resources().global_id(*t->package, t->id);
    return t->id;
}

Runtime::Resource *slot_part(Runtime &rt, Object *o)
{
    Runtime::Handle *h = rt.handle(o);
    return h ? h->target : nullptr;
}

// 0x4c8b10: the wheel a part belongs to. An installed part (runtime slot flag
// 1) inherits the wheel of the slot it sits in on its host; otherwise the type
// slot with this id carries the cfg `slottype`. 0 when nothing resolves.
// INVENTED: depth guard, the exe chain is acyclic (flag 1 only on the child
// side, as in part_locked 0x4cb0b0).
uint32_t wheel_id_of(Runtime &rt, Runtime::Resource *r, int32_t slot_id, int depth)
{
    if (!r || depth > 64) return 0;
    PartData *p = part_data(rt, r);
    if (!p) return 0;
    for (const Slot &s : p->slots) {
        if (!(s.flags & 1u) || !s.attached) continue;
        Runtime::Resource *t = slot_part(rt, s.attached);
        if (!t) return 0;  // the exe returns 0 when the target does not resolve
        return wheel_id_of(rt, t, s.mate_id, depth + 1);
    }
    for (const Slot &s : p->slots)
        if (s.id == slot_id) return (uint16_t)s.slot_type;
    return 0;
}

bool attach_has(const std::vector<CfgAttach> &list, uint32_t part, int32_t slot)
{
    for (const CfgAttach &a : list)
        if (a.part == part && a.slot == slot) return true;
    return false;
}

// 0x4c9d90: the two slots' cfg attach/compatible lists meet.
bool part_compatible(Runtime &rt, Runtime::Resource *A, int32_t slotA,
                     Runtime::Resource *B, int32_t slotB)
{
    uint32_t aid = part_type_id(rt, A), bid = part_type_id(rt, B);
    if (aid && aid == bid && slotA == slotB) return false;
    Slot *sa = slot_by_part(part_data(rt, A), slotA);
    Slot *sb = slot_by_part(part_data(rt, B), slotB);
    if (!sa || !sb) return false;
    if (attach_has(sa->attaches, bid, slotB)) return true;
    if (attach_has(sb->attaches, aid, slotA)) return true;
    for (const CfgAttach &a : sa->attaches)
        if (attach_has(sb->compatibles, a.part, a.slot)) return true;
    for (const CfgAttach &b : sb->attaches)
        if (attach_has(sa->compatibles, b.part, b.slot)) return true;
    return false;
}

// 0x4cb0b0: walks UP the install chain only. Per slot in order, only my-side
// slots (flags & 1, installed onto a parent) count: 0x50 -> locked; else the
// first one with a mate returns the parent's verdict (no further slots).
bool part_locked(Runtime &rt, Runtime::Resource *r, int depth)
{
    // INVENTED: depth guard; the exe chain is acyclic (flag 1 only on the child side).
    if (!r || depth > 64) return false;
    PartData *p = part_data(rt, r);
    if (!p) return false;
    for (const Slot &s : p->slots) {
        if (!(s.flags & 1u)) continue;
        if ((s.flags & 0x50u) == 0x50u) return true;
        if (!s.attached) continue;
        Runtime::Resource *t = slot_part(rt, s.attached);
        if (t) return part_locked(rt, t, depth + 1);
    }
    return false;
}

// shortcut: 0x4c6e50 maps the slot through the body pose; src has no part body
// transform, so the runtime slot pose is compared (only the explicit-position
// "install x y z" form uses it, shipped scripts pass 5 ints).
float slot_dist2(const Slot &s, const float pos[3])
{
    float dx = s.pos[0] - pos[0], dy = s.pos[1] - pos[1], dz = s.pos[2] - pos[2];
    return dx * dx + dy * dy + dz * dz;
}

// 0x4c89f0: first (or nearest) free target slot compatible with my slot.
int32_t target_slot_for(Runtime &rt, Runtime::Resource *T,
                        Runtime::Resource *my, int32_t my_slot, const float *pos)
{
    if (part_locked(rt, T, 0)) return 0;
    PartData *tp = part_data(rt, T);
    if (!tp) return 0;
    int32_t best = 0;
    float bestd = 0.0f;
    for (Slot &t : tp->slots) {
        if (t.attached || (t.flags & 0x10u)) continue;
        if (!part_compatible(rt, T, t.id, my, my_slot)) continue;
        if (!pos) return t.id;
        float d = slot_dist2(t, pos);
        if (d < 1e8f && (!best || d < bestd)) {
            best = t.id;
            bestd = d;
        }
    }
    return best;
}

// 0x4c9af0: pick my slot (and the target slot, when auto) that mates with tref.
int32_t find_mate_slot(Runtime &rt, Runtime::Resource *P,
                       Runtime::Resource *tref, int32_t *t_slot,
                       const float *pos)
{
    PartData *tp = part_data(rt, tref);
    if (!tp || tp->slots.empty() || tref->id == P->id) return 0;
    PartData *mp = part_data(rt, P);
    if (!mp) return 0;
    if (*t_slot == 0) {  // auto: walk my slots in cfg order
        int32_t best = 0, best_t = 0;
        float bestd = 0.0f;
        for (Slot &s : mp->slots) {
            // exe test: (+8 == 0 || flags & 1) && !(flags & 0x10)
            if (!((!s.attached) || (s.flags & 1u)) || (s.flags & 0x10u)) continue;
            int32_t c = target_slot_for(rt, tref, P, s.id, pos);
            if (!c) continue;
            if (!pos) {
                *t_slot = c;
                return s.id;
            }
            Slot *ts = slot_by_part(tp, c);
            float d = ts ? slot_dist2(*ts, pos) : 0.0f;
            if (!best || d < bestd) {
                best = s.id;
                best_t = c;
                bestd = d;
            }
        }
        if (best) *t_slot = best_t;
        return best;
    }
    Slot *ts = slot_by_part(tp, *t_slot);
    if (!ts || ts->attached || (ts->flags & 0x10u) || part_locked(rt, tref, 0))
        return 0;
    for (Slot &s : mp->slots) {
        if (s.attached || (s.flags & 0x10u)) continue;
        if (part_compatible(rt, tref, *t_slot, P, s.id)) return s.id;
    }
    return 0;
}

// 0x4c96c0: target part first, else the dest object (0x4c96c0's fallback).
// Writes the target actually used into *tref_out.
int32_t install_search(Runtime &rt, Runtime::Resource *P,
                       Runtime::Resource *tref, Runtime::Resource *dest,
                       Runtime::Resource **tref_out, int32_t *t_slot,
                       const float *pos)
{
    if (tref) {
        int32_t r = find_mate_slot(rt, P, tref, t_slot, nullptr);
        if (r) {
            *tref_out = tref;
            return r;
        }
    }
    if (!dest) return 0;
    // shortcut: getInfo(dest, 7) kinds other than 5 (vehicle) and 9 (part) are
    // not modelled; both shipped script calls pass the car.
    int kind = g_cars.count(dest) ? 5 : g_parts.count(dest) ? 9 : 0;
    if (kind != 5 && kind != 9) return 0;
    int32_t r = find_mate_slot(rt, P, dest, t_slot, pos);
    if (r) {
        *tref_out = dest;
        return r;
    }
    // shortcut: 0x4c96c0's getInfo(dest, 0x31) iterator is the car's installed
    // parts; src uses the resources reparented under the car (attach step 1).
    if (kind == 5)
        for (Runtime::Resource *child : dest->children) {
            if (child == P || !g_parts.count(child)) continue;
            r = find_mate_slot(rt, P, child, t_slot, pos);
            if (r) {
                *tref_out = child;
                return r;
            }
        }
    return 0;
}

// ---- compound mass (0x5032d0, card 153) ------------------------------------

Mat34 identity_rot()
{
    Mat34 m{};
    m.m[0] = m.m[5] = m.m[10] = 1.0f;
    return m;
}

// mat_inverse 0x566830 has no singular guard; no shape props means point mass.
bool invert3(const Mat34 &m, Mat34 *out)
{
    float det = m.m[0] * (m.m[5] * m.m[10] - m.m[6] * m.m[9]) -
                m.m[1] * (m.m[4] * m.m[10] - m.m[6] * m.m[8]) +
                m.m[2] * (m.m[4] * m.m[9] - m.m[5] * m.m[8]);
    if (!isfinite(det) || !(fabsf(det) > 1e-12f)) return false;
    mat_inverse(out, &m);
    return true;
}

// The `.scx` hull of a cfg body line. Cached: every install rebuilds the whole
// compound and re-reads every installed part.
std::shared_ptr<const CollisionMesh> body_hull(const std::string &path)
{
    static std::map<std::string, std::shared_ptr<const CollisionMesh>> cache;
    auto it = cache.find(path);
    if (it != cache.end()) return it->second;
    std::string p = path;
    for (char &ch : p)
        if (ch == '\\') ch = '/';
    mesh::Mesh m;
    std::string err;
    std::shared_ptr<const CollisionMesh> out;
    if (mesh::load(p.c_str(), &m, &err)) out = build_collision_mesh(m);
    cache[path] = out;
    return out;
}

// 0x4bf4d0 body line shape: the primitive branches use the createBox sizes; any
// other token is the kind-6 .scx hull.
void body_line_shape(const CfgBody &b, Shape *shape)
{
    shape->type = SHAPE_NONE;
    if (b.shape == "ball" || b.shape == "sphere") {
        shape->type = SHAPE_SPHERE;
        if (b.tail_count > 0) shape->radius = b.tail[0];
    } else if (b.shape == "box") {
        shape->type = SHAPE_BOX;
        shape->half_extents = Vec3{b.tail[0], b.tail[1], b.tail[2]};
    } else if (b.shape == "rodx" || b.shape == "rody" || b.shape == "rodz") {
        shape->type = SHAPE_CAPSULE_X + (b.shape == "rody" ? 1 : b.shape == "rodz" ? 2 : 0);
        if (b.tail_count > 0) shape->half_length = b.tail[0];
        if (b.tail_count > 1) shape->radius = b.tail[1];
    } else {
        // shortcut: the numeric-resource form of the shape token is not
        // resolved; the shipped car/part cfgs use .scx paths and primitives.
        shape->mesh = body_hull(b.shape);
        if (shape->mesh) shape->type = SHAPE_MESH;
    }
}

struct CompoundChild {
    float mass = 0.0f;
    Vec3 pos{0.0f, 0.0f, 0.0f};  // car frame
    Mat34 rot{};                 // child -> car frame
    Mat34 inertia{};             // about the child's own COM, child frame
    Shape shape;
};

// 0x5032d0 child: the body's mass and I_c (def+0x60 = inverse of its inv
// inertia); the 0x508d20 centroid offset moves the child, not the mesh.
void add_compound_child(std::vector<CompoundChild> &out, float mass, const Shape &shape,
                        const Mat34 &rot, Vec3 pos)
{
    if (!(mass > 0.0f)) return;
    BodyDef def;
    def.shape = shape;
    Vec3 center{0.0f, 0.0f, 0.0f};
    compute_mass_props(&def, mass, &center);
    CompoundChild c;
    c.mass = mass;
    c.rot = rot;
    c.pos = pos + mat_vec(rot, center);
    c.shape = shape;
    if (!invert3(def.inv_inertia, &c.inertia)) c.inertia = Mat34{};
    out.push_back(c);
}

// Every `body` line of cfg, whose frame sits at (rot, pos) in the car frame.
void add_body_lines(std::vector<CompoundChild> &out, const CfgFile &cfg,
                    const Mat34 &rot, Vec3 pos)
{
    for (const CfgBody &b : cfg.bodies) {
        if (!(b.mass > 0.0f)) continue;
        float ang[3] = {b.xform.rx, b.xform.ry, b.xform.rz};
        Mat34 lr;
        euler_to_mat(&lr, ang);
        Mat34 cr;
        mat_mul(&cr, &rot, &lr);
        Vec3 cp = pos + mat_vec(rot, Vec3{b.xform.px, b.xform.py, b.xform.pz});
        Shape shape;
        body_line_shape(b, &shape);
        add_compound_child(out, b.mass, shape, cr, cp);
    }
}

// 0x4c1a10: car-space pose of part r, walking the committed slot up to the car
// resource (the chassis has p = 0, R = I).
// INVENTED: depth guard; the committed install graph is acyclic.
void part_pose(Runtime &rt, Runtime::Resource *r, Mat34 *rot, Vec3 *pos, int depth)
{
    *rot = identity_rot();
    *pos = Vec3{0.0f, 0.0f, 0.0f};
    if (!r || depth > 64 || g_cars.count(r)) return;
    auto pit = g_parts.find(r);
    if (pit == g_parts.end()) return;
    Slot *ms = nullptr;
    for (Slot &s : pit->second->slots)
        if ((s.flags & 1u) && s.attached) {
            ms = &s;
            break;
        }
    if (!ms) return;
    Runtime::Resource *t = slot_part(rt, ms->attached);
    Mat34 rT;
    Vec3 pT;
    part_pose(rt, t, &rT, &pT, depth + 1);
    auto tit = g_parts.find(t);
    Slot *ts = tit == g_parts.end() ? nullptr : slot_by_part(tit->second.get(), ms->mate_id);
    if (!ts) return;
    Mat34 mst, tmp;
    mat_transpose(&mst, &ms->rot);
    mat_mul(&tmp, &rT, &ts->rot);
    mat_mul(rot, &tmp, &mst);
    // INVENTED: the subtraction matrix (R_P vs R_T*Ts.rot) is hidden inside
    // 0x567020; the card uses R_P. ms is the first committed slot.
    *pos = pT + mat_vec(rT, Vec3{ts->pos[0], ts->pos[1], ts->pos[2]}) -
           mat_vec(*rot, Vec3{ms->pos[0], ms->pos[1], ms->pos[2]});
}

// 0x5032d0 PhysBody__updateCompoundMass on the car: part_comp's children (car
// cfg body lines) and car_root's children (installed parts, wheelbone spheres
// and 0.01 kg sensors). Also 0x4c7d20's tail after every commitInstall.
void rebuild_compound(Runtime &rt, Runtime::Resource *carres)
{
    auto cit = g_cars.find(carres);
    if (cit == g_cars.end()) return;
    CarData *car = cit->second.get();
    if (!car->root_body) return;

    std::vector<CompoundChild> children;
    CfgFile car_cfg;
    if (load_car_cfg(rt, carres, &car_cfg, nullptr))
        add_body_lines(children, car_cfg, identity_rot(), Vec3{0.0f, 0.0f, 0.0f});
    for (const WheelData &w : car->wheels) {
        Vec3 hub{w.hub[0], w.hub[1], w.hub[2]};
        // cfg `type`: "<bone mass> sphere <radius>" (fill_car reads the 3rd value).
        Shape bone;
        bone.type = SHAPE_SPHERE;
        bone.radius = w.radius;
        add_compound_child(children, w.bone_mass, bone, identity_rot(), hub);
        Shape sensor;  // 0x4f3f10 createBox(0.01, 3, L/2, 0.05), L = W+0x5c
        sensor.type = SHAPE_CAPSULE_Y;
        sensor.half_length = 0.5f * w.free_len;
        sensor.radius = 0.05f;
        add_compound_child(children, 0.01f, sensor, identity_rot(),
                           hub - Vec3{0.0f, 0.5f * w.free_len, 0.0f});
    }
    // INVENTED: every installed part joins car_root; 0x4c7d20's getInfo 0x2f
    // slot->body mapping is not traced (0x5032d0 recurses to car_root anyway).
    std::set<Runtime::Resource *> seen;
    std::vector<Runtime::Resource *> stack(carres->children.begin(), carres->children.end());
    while (!stack.empty()) {
        Runtime::Resource *r = stack.back();
        stack.pop_back();
        if (!seen.insert(r).second) continue;
        for (Runtime::Resource *child : r->children) stack.push_back(child);
        auto pit = g_parts.find(r);
        if (pit == g_parts.end() || pit->second->installed <= 0) continue;
        CfgFile part_cfg;
        if (!load_part_cfg(rt, r, &part_cfg)) continue;
        Mat34 rot;
        Vec3 pos;
        part_pose(rt, r, &rot, &pos, 0);
        add_body_lines(children, part_cfg, rot, pos);
    }

    float mass = 0.0f;
    Vec3 com{0.0f, 0.0f, 0.0f};
    for (const CompoundChild &c : children) {
        mass += c.mass;
        com += c.pos * c.mass;
    }
    RigidBody *root = car->root_body;
    if (mass > 0.0f) {
        com = com * (1.0f / mass);
        Mat34 inertia{};
        for (const CompoundChild &c : children) {
            Mat34 ct, term, tmp;
            mat_transpose(&ct, &c.rot);
            mat_mul(&term, &c.rot, &c.inertia);
            mat_mul(&tmp, &term, &ct);
            for (int i = 0; i < 12; i++) inertia.m[i] += tmp.m[i];
            add_point_inertia(&inertia, c.mass, c.pos - com);
        }
        // 0x5032d0: the body pose moves to the COM, the child offsets lose it;
        // car_root's local frame is its COM.
        Vec3 old{car->com[0], car->com[1], car->com[2]};
        root->pos += mat_vec(root->rotation(), com - old);
        Mat34 inv{};
        if (!invert3(inertia, &inv)) inv = Mat34{};
        root->def.inv_inertia = inv;
        root->def.inv_mass = 1.0f / mass;
        root->def.linear_drag = -0.005f * mass;
        root->def.angular_drag = -0.01f * mass;
        car->com[0] = com.x;
        car->com[1] = com.y;
        car->com[2] = com.z;
    } else {
        root->def.inv_mass = 0.0f;
        root->def.flags |= 4u;  // static
    }
    // Card 160: Collide__bodyVsTriangles 0x4feb20 recurses over the compound
    // children. INVENTED storage: kind-6 hulls with their root-frame poses
    // instead of real child bodies; car_root's own shape stays SHAPE_NONE.
    root->collision_children.clear();
    for (const CompoundChild &c : children) {
        if (c.shape.type != SHAPE_MESH || !c.shape.mesh) continue;
        RigidBody::CollisionChild child;
        child.shape = c.shape;
        child.pose.pos = c.pos - com;
        mat_to_quat(&child.pose.rot, &c.rot);
        child.poly = std::make_shared<const Poly>(poly_from_triangles(*c.shape.mesh));
        root->collision_children.push_back(child);
    }
    // 0x5032d0 re-centres the children; the sim wheels are src's children.
    Car *sim = car->sim.get();
    if (sim && sim->wheels.size() == car->wheels.size())
        for (size_t i = 0; i < sim->wheels.size(); i++) {
            const WheelData &w = car->wheels[i];
            sim->wheels[i].mount = Vec3{w.hub[0] - com.x, w.hub[1] - com.y,
                                        w.hub[2] - com.z};
        }
}

// 0x4cb1d0 + 0x4c7d20 (bolts = 0, so the install commits at once).
int part_attach(Runtime &rt, Runtime::Resource *P, int32_t my_slot,
                Runtime::Resource *tref, int32_t t_slot, Runtime::Resource *dest)
{
    if (!tref) return 0;
    // 0x4ec210: reparent under dest; Part.addPart tests getParentID() != root.
    if (dest) rt.reparent(P, dest);
    PartData *pp = part_data(rt, P);
    PartData *tp = part_data(rt, tref);
    Slot *ms = slot_by_part(pp, my_slot);
    Slot *ts = slot_by_part(tp, t_slot);
    if (!ms || !ts) return 0;
    Object *pobj = rt.script_instance(P);
    Object *tobj = rt.script_instance(tref);
    // 0x4cb1d0 step 3: both runtime slots point at the other part's object.
    ms->attached = tobj;
    ms->mate_id = t_slot;
    ts->attached = pobj;
    ts->mate_id = my_slot;
    // 0x4c7d20 with bolts == 0: my slot present (1), target slot occupied (2).
    // shortcut: type slot +0x6c bolt count is not parsed, so the flag-4 pending
    // path never runs (notes/13 §15).
    ms->flags = (ms->flags & ~4u) | 1u;
    ts->flags = (ts->flags & ~4u) | 2u;
    pp->installed++;
    tp->installed++;
    // 0x4c7d20: P's car ref := getInfo(target, 0x2b): the chassis target is the
    // car itself, a sub-part yields its own car_id.
    if (g_cars.count(tref)) {
        pp->car_ref = tref;
        pp->car_id = (int32_t)tref->id;
    } else {
        pp->car_ref = tp->car_ref;
        pp->car_id = tp->car_id;
    }
    // 0x4c7d20: P+0xd0 = 0x4c8b10(P, 0) once the car ref is set.
    pp->wheel_id = (int32_t)wheel_id_of(rt, P, 0, 0);
    // 0x4c7d20 physics tail: 0x501d00/0x4f7b20 put the part body under the
    // target and 0x5032d0 re-sums the compound through car_root.
    // shortcut: the exe wakes the two roots and re-sleeps car_root when it was
    // asleep; shipped installs happen on a sleeping car and only touch defs.
    if (pp->car_ref) {
        auto ci = g_cars.find(pp->car_ref);
        if (ci != g_cars.end() && ci->second->root_body)
            rebuild_compound(rt, pp->car_ref);
    }
    // 0x47dd50: script callback Part.install() (Part.java:707).
    if (pobj) {
        std::string err;
        Value rv;
        rt.call_method(pobj, "install", {}, &rv, &err);
    }
    return 1;
}

}  // namespace

// 0x569970 Math__matToYpr.
void mat_to_ypr(const Mat34 &m, float out[3])
{
    float yaw, pitch, roll;
    if (m.m[10] == 0.0f && m.m[2] == 0.0f) {
        yaw = atan2f(m.m[0], -m.m[1]);
        pitch = 3.1415927f;
        roll = 0.0f;
    } else {
        yaw = atan2f(m.m[2], m.m[10]);
        roll = atan2f(m.m[4], m.m[5]);
        float c = cosf(roll);
        if (c > 0.0f)
            pitch = atan2f(-(m.m[6] * c), m.m[5]);
        else
            pitch = atan2f(m.m[6] * c, -m.m[5]);
    }
    out[0] = yaw;
    out[1] = pitch;
    out[2] = roll;
}

WheelData::WheelData()
{
    for (int i = 0; i < kPacejkaSlots; i++) pacejka[i] = kPacejkaDefaults[i];
}

void WheelData::set_pos(const float pos[3])
{
    float old[3] = {hub[0], hub[1], hub[2]};
    hub[0] = template_pos[0] + pos[0];
    hub[1] = template_pos[1] + pos[1] + base_len + travel;
    hub[2] = template_pos[2] + pos[2];
    for (int i = 0; i < 3; i++) {
        ic_h[i] += hub[i] - old[i];
        ic_l[i] += hub[i] - old[i];
    }
}

void WheelData::set_ypr(const float *ypr)
{
    euler_to_mat(&rot, ypr ? ypr : template_ypr);
    pose = rot;
}

void WheelData::set_hub(float len, const float p1[3], const float p2[3],
                        const float pc[3])
{
    hub_len = len;
    for (int i = 0; i < 3; i++) {
        hub_p1[i] = p1[i];
        hub_p2[i] = p2[i];
        hub_pc[i] = hub[i] + pc[i];
    }
    hub_pc[1] -= base_len + travel;
    if (len == 0.0f) {
        float n = sqrtf(hub_p2[0] * hub_p2[0] + hub_p2[1] * hub_p2[1] +
                        hub_p2[2] * hub_p2[2]);
        if (n != 0.0f) {
            float k = 1.0f / n;
            for (int i = 0; i < 3; i++) hub_p2[i] *= k;
        }
        for (int i = 0; i < 3; i++) hub_pc[i] -= hub_p1[i];
    }
}

void WheelData::set_arm(float len, const float axis[3], const float pivot[3])
{
    arm_len = len;
    float n = sqrtf(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
    if (n != 0.0f) {
        float k = 1.0f / n;
        for (int i = 0; i < 3; i++) arm_axis[i] = axis[i] * k;
    }
    for (int i = 0; i < 3; i++) arm_pivot[i] = hub[i] + pivot[i];
    arm_pivot[1] -= base_len + travel;
}

void WheelData::set_friction(float v)
{
    friction = v;
    pacejka[2] = v;
    friction_eff = v * wear_factor(tyre_wear);
}

void WheelData::set_brake(float v)
{
    brake = v;
    brake_eff = v * wear_factor(brake_wear);
}

void WheelData::set_hbrake(float v)
{
    hbrake = v;
    hbrake_eff = v * wear_factor(brake_wear);
}

void WheelData::set_radius(float v)
{
    radius = v;
    free_len = rest_len + v;
}

void WheelData::set_rest_len(float v)
{
    rest_len = v;
    free_len = v + radius;
}

void WheelData::reset_damage()
{
    flags &= ~0x20u;
    euler_to_mat(&rot, template_ypr);
    pose = rot;
    hub[0] = template_pos[0];
    hub[1] = template_pos[1] + base_len + travel;
    hub[2] = template_pos[2];
    brake_eff = brake;
    hbrake_eff = hbrake;
    drive_eff = drive;
    wobble = 0.0f;
    wobble_phase = 0.0f;
    damage_visual = template_damage;
}

CarData::CarData() = default;

float CarData::torque_at(float rpm) const
{
    return curve_at(torque_rpm, torque_nm, rpm);
}

float CarData::nitro_torque_at(float rpm) const
{
    return curve_at(nitro_rpm, nitro_nm, rpm);
}

std::string wheel_damage(const WheelData &w)
{
    float ypr[3];
    mat_to_ypr(w.rot, ypr);
    char buf[256];
    snprintf(buf, sizeof buf,
             "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", w.hub[0],
             w.hub[1], w.hub[2], ypr[0], ypr[1], ypr[2], w.wobble, 1.0f, 1.0f,
             w.wobble_phase);
    return std::string(buf);
}

void set_wheel_damage(WheelData &w, const std::string &s)
{
    if (s.size() < 2) {
        w.reset_damage();
        return;
    }
    float v[10] = {0, 0, 0, 0, 0, 0, 0, 1, 1, 0};
    sscanf(s.c_str(), "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2],
           &v[3], &v[4], &v[5], &v[6], &v[7], &v[8], &v[9]);
    w.flags |= 0x20u;
    float ypr[3] = {v[3], v[4], v[5]};
    euler_to_mat(&w.rot, ypr);
    w.pose = w.rot;
    for (int i = 0; i < 3; i++) w.hub[i] = v[i];
    w.wobble = v[6];
    w.wobble_phase = v[9];
    w.damage_visual = v[9] * 200.0f + w.template_damage;
}

std::string slot_damage(const Slot &s)
{
    float ypr[3];
    mat_to_ypr(s.rot, ypr);
    char buf[160];
    snprintf(buf, sizeof buf, "%.3f,%.3f,%.3f,%.3f,%.3f,%.3f", s.pos[0],
             s.pos[1], s.pos[2], ypr[0], ypr[1], ypr[2]);
    return std::string(buf);
}

void set_slot_damage(Slot &s, const std::string &str)
{
    if (str.size() < 2) {
        s.flags &= ~0x20u;
        return;
    }
    float v[6] = {};
    sscanf(str.c_str(), "%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3],
           &v[4], &v[5]);
    for (int i = 0; i < 3; i++) s.pos[i] = v[i];
    float ypr[3] = {v[3], v[4], v[5]};
    euler_to_mat(&s.rot, ypr);
    s.flags |= 0x20u;
}

PartData *part_data(Runtime &rt, Runtime::Resource *r)
{
    if (g_rt != &rt) reset(&rt);
    if (!r) return nullptr;
    auto it = g_parts.find(r);
    if (it != g_parts.end()) return it->second.get();
    auto up = std::make_unique<PartData>();
    CfgFile cfg;
    const Package *pkg = nullptr;
    // The car manager embeds the part manager: the chassis' slot list comes from
    // its `car` cfg (0x4c9af0 reads it like a part's).
    if (load_native_cfg(rt, r, "part", &cfg, &pkg))
        fill_part(rt, pkg, *up, cfg);
    else if (load_car_cfg(rt, r, &cfg, &pkg))
        fill_slots(rt, pkg, *up, cfg);
    PartData *p = up.get();
    g_parts[r] = std::move(up);
    return p;
}

CarData *car_data(Runtime &rt, Runtime::Resource *r)
{
    if (g_rt != &rt) reset(&rt);
    if (!r) return nullptr;
    auto it = g_cars.find(r);
    if (it != g_cars.end()) return it->second.get();
    auto up = std::make_unique<CarData>();
    CfgFile cfg;
    if (load_native_cfg(rt, r, "car", &cfg)) fill_car(*up, cfg);
    CarData *p = up.get();
    g_cars[r] = std::move(up);
    return p;
}

bool load_car_cfg(Runtime &rt, Runtime::Resource *r, CfgFile *cfg,
                  const Package **pkg)
{
    if (!r) return false;
    if (load_native_cfg(rt, r, "car", cfg, pkg)) return true;
    auto it = g_cars.find(r);
    if (it == g_cars.end() || !it->second->cfg_resource) return false;
    return load_native_cfg(rt, it->second->cfg_resource, "car", cfg, pkg);
}

bool load_part_cfg(Runtime &rt, Runtime::Resource *r, CfgFile *cfg,
                   const Package **pkg)
{
    if (!r) return false;
    if (load_native_cfg(rt, r, "part", cfg, pkg)) return true;
    // 0x4c1f90: the instance's chain does not reach its type record, so the
    // type create_car_bodies kept on the payload is the fallback.
    if (g_rt != &rt) return false;
    auto it = g_parts.find(r);
    if (it == g_parts.end() || !it->second->type) return false;
    return load_native_cfg(rt, it->second->type, "part", cfg, pkg);
}

// 0x4c1f90 Part__initInstance (body part) + 0x4992e0 Vehicle__createCar
// (body part). The cfg `body` line handler 0x4bf4e0 sets M+0xff0; no body line
// (-1) means neither body exists.
void create_car_bodies(Runtime &rt, Runtime::Resource *inst,
                       Runtime::Resource *type, const std::string &params)
{
    // 0x4c1f90 Part__initInstance: the part manager builds the runtime slot
    // array from the type cfg. The created instance's chain does not reach the
    // type record, so the type link is kept on the payload (create_car_bodies
    // is called with it, 0x523c80).
    if (type && has_native_ref(rt, type, "part")) {
        if (g_rt != &rt) reset(&rt);
        // INVENTED: the typed-object event mask default is not traced; the same
        // assumption as the car below, needed for GameRef.command to reach vf14.
        inst->event_mask |= 0x10u;
        PartData *pd = part_data(rt, inst);
        if (pd) {
            pd->type = type;
            if (pd->slots.empty()) {
                CfgFile part_cfg;
                const Package *pkg = nullptr;
                if (load_native_cfg(rt, type, "part", &part_cfg, &pkg))
                    fill_part(rt, pkg, *pd, part_cfg);
            }
        }
    }
    // 0x523c80 also runs the type record's native manager for `native player`
    // (0x5240e0); src needs its command handler (card 147).
    if (type && has_native_ref(rt, type, "player")) {
        // INVENTED: the typed-object event mask default is not traced (card 040).
        inst->event_mask |= 0x10u;
        if (g_rt != &rt) reset(&rt);  // else the first car_data() wipes g_players
        g_players.insert(inst);
    }
    CfgFile cfg;
    if (!type || !load_native_cfg(rt, type, "car", &cfg) || cfg.bodies.empty()) return;
    CarData *car = car_data(rt, inst);
    if (!car) return;
    // INVENTED: the instance's super chain does not reach the type record, so
    // car_data cannot load the cfg; the exe's type manager vf3 gets it directly.
    if (car->wheels.empty()) fill_car(*car, cfg);
    car->cfg_resource = type;
    if (!car->sim) car->sim = std::make_unique<Car>();
    Car *sim = car->sim.get();
    // 0x49a4ef: engine on at create for a car with a driver slot (M+0x17f0 >= 0)
    // unless Steam.neutralize set the process flag (0x7a75df). car+0x1e6c = -1
    // (0x499511) is the CarData default.
    if (car->driver_slot >= 0)
        car->flags = steam_neutralized ? (car->flags & ~0x4000u)
                                       : (car->flags | 0x4000u);
    sim->gear = 0;
    // INVENTED: the typed-object event mask default is not traced (card 040);
    // the car's native vf14 must be reachable from Vehicle.wakeUp/command, which
    // no script enables with setEventMask. EVENT_COMMAND = 0x10 (GameRef.class).
    inst->event_mask |= 0x10u;
    World &world = process_world();

    // Str__scan(params, "%f,%f,%f,%f,%f,%f,%d"); Vehicle.java:111 passes
    // "0,0,0,0,0,0". Missing fields stay 0.
    float x = 0.0f, y = 0.0f, z = 0.0f, yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    int tail = 0;
    sscanf(params.c_str(), "%f,%f,%f,%f,%f,%f,%d", &x, &y, &z, &yaw, &pitch,
           &roll, &tail);
    float ypr[3] = {yaw, pitch, roll};
    Mat34 mat;
    euler_to_mat(&mat, ypr); // 0x5666d0 ypr_to_matrix

    // 0x4c1f90: "part_comp" body, then PhysBody__sleep(PhysBody__root(body)).
    auto chassis = std::make_unique<RigidBody>();
    // INVENTED: Poly box template of a mesh-less type 6 not ported; the exe's
    // createBox(1.0f, 6, 0, 0, 0, 0) builds a mass-1 body without a mesh.
    chassis->def.shape.type = SHAPE_MESH;
    compute_mass_props(&chassis->def, 1.0f);
    chassis->pos = Vec3{x, y, z};
    mat_to_quat(&chassis->rot, &mat);
    world.add_body(chassis.get());
    car->chassis_body = chassis.get();
    world.sleep(chassis.get()); // 0x506d90
    chassis->def.flags |= 8u;   // body+0x58 |= 8
    car->chassis_body_storage = std::move(chassis);
    car->root_body = nullptr;

    // 0x4992e0: "car_root" box at the chassis root's active pose; the chassis
    // is unlinked and becomes its child. addBody head-inserts into the active
    // root (0x4f7830), so it comes before the chassis root. The box is the
    // simulation body itself (Vehicle__update applies its impulses to it).
    RigidBody *root = car->chassis_body;
    if (root) {
        if (root->asleep) world.wake(root); // 0x507060
        RigidBody *cr = &sim->body;
        cr->set_box(Vec3{1.0f, 1.0f, 1.0f}, 1.0f); // createBox(1.0f, 5, 1, 1, 1, 0)
        // The exe's soup pass reaches the car through its compound root
        // (Collide__bodyVsTriangles 0x4feb20 recurses over compound children,
        // notes/19 §7). src pairs root shapes only and the drive callback's
        // wheels live below this box, so a 1x1x1 collider here would sit the
        // car on the box instead of the springs (card 146 check).
        // shortcut: no compound child collision yet; upgrade with the part-body
        // shapes (wheelbone/a_arm, notes/09 "Wheel/suspension objects").
        cr->def.shape.type = SHAPE_NONE;
        cr->pos = root->pos;
        cr->rot = root->rot;
        world.add_body(cr);
        car->root_body = cr;
        world.unlink(root);                // 0x501d00
        world.attach_child(root, cr);      // 0x4f7b20
    }
    // 0x5032d0 PhysBody__updateCompoundMass on part_comp, then through the
    // parent link on car_root. part_comp's own shape mass is not counted: the
    // compound is the cfg `body` children, the per-wheel wheelbone child, the
    // 0.01 kg trigger-sensor capsule (0x4f3f10) and every installed part, each
    // with its own shape inertia. part_comp itself keeps the cfg body-line sum
    // because 0x4992e0 fixes M+0x1f34 (the spring rate) from it before any part
    // is installed (notes/09).
    float part_mass = 0.0f;
    for (const CfgBody &b : cfg.bodies)
        if (b.mass > 0.0f) part_mass += b.mass;
    if (part_mass > 0.0f) {
        car->chassis_body->def.inv_mass = 1.0f / part_mass;
        car->chassis_body->def.linear_drag = -0.005f * part_mass;
        car->chassis_body->def.angular_drag = -0.01f * part_mass;
    } else {
        car->chassis_body->def.inv_mass = 0.0f;
        car->chassis_body->def.flags |= 4u;  // static
    }
    rebuild_compound(rt, inst);

    // Wheel records -> simulation wheels. mount = W+0x78 hub in car_root space;
    // the suspension lengths are W+0x50/54/58 (rest_ray etc. add the radius at
    // query time), the force/brake shares W+0xcc/d0/d4/e0 and the contact-patch
    // block S+0x9c/a0/a4.
    sim->wheels.resize(car->wheels.size());
    for (size_t i = 0; i < car->wheels.size(); i++) {
        const WheelData &w = car->wheels[i];
        Wheel &s = sim->wheels[i];
        s.radius = w.radius;
        s.friction = w.friction;
        s.friction_eff = w.friction_eff;
        s.rolling_resistance = w.roll_res;
        for (int k = 0; k < kPacejkaSlots; k++) s.pacejka.p[k] = w.pacejka[k];
        s.mount = Vec3{w.hub[0] - car->com[0], w.hub[1] - car->com[1],
                       w.hub[2] - car->com[2]};
        s.rest_len = w.rest_len;
        s.max_len = w.max_len;
        s.min_len = w.min_len;
        // 0x4f3f10 L = W+0x5c, captured at create; the sensor body is never resized.
        s.sensor_len = w.free_len;
        s.ray_len = s.max_ray(); // S+0xf0 starts at the tick's miss value
        s.drive_ratio = w.drive_ratio;
        s.steer_factor = w.steer;
        s.set_drive(w.drive_eff);
        s.set_brake(w.brake_eff);
        s.set_handbrake(w.hbrake_eff);
        s.set_cpatch(w.cpatch_halfwidth, w.cpatch_sin, w.cpatch_offset);
    }

    // 0x4992e0 wheel loop: static sag = travel, so the wheel carries |g| *
    // M+0x1f34 / n over the travel; bump/rebound from the cfg ratio.
    float gravity = vec_length(process_world().gravity);  // |world+0x1c4| = 9.81
    float wheel_count = (float)car->wheels.size();
    for (size_t i = 0; i < car->wheels.size(); i++) {
        WheelData &w = car->wheels[i];
        if (wheel_count > 0.0f && w.travel > 0.0f)
            w.spring_k = gravity * part_mass / wheel_count / w.travel;
        w.bump = w.spring_k * w.damp_ratio;
        w.rebound = 0.7f * w.bump;
        Wheel &s = sim->wheels[i];
        s.spring = w.spring_k;
        s.bump = w.bump;
        s.rebound = w.rebound;
    }
    root = car->root_body ? car->root_body : car->chassis_body; // PhysBody__root
    if (root && !root->asleep) world.sleep(root);               // 0x506d90
}

int car_command(Runtime &rt, Runtime::Resource *car, uint32_t code,
                const Value &param)
{
    if (code != 0x10 || param.kind != Value::Str) return 0; // EVENT_COMMAND
    if (g_rt != &rt || !car) return 0;
    auto it = g_cars.find(car);
    if (it == g_cars.end()) return 0;
    CarData *c = it->second.get();

    // PhysBody__root 0x480700: follow the compound parent chain.
    RigidBody *root = c->chassis_body;
    while (root && root->parent) root = root->parent;
    World &world = process_world();
    const std::string &cmd = param.s;

    if (cmd == "brake") {  // 0x4a3f6d
        c->flags |= 0x40000u;
        return 1;
    }
    if (cmd == "start") {  // 0x4a3fa6
        c->flags &= ~(0x40000u | 0x4u);
        return 1;
    }
    if (cmd == "suspend") {  // 0x4a3fdf PhysBody__sleepRoot 0x4ae5a0
        if (root) world.sleep(root);
        return 1;
    }
    if (cmd == "wakeup") {  // 0x4a4023 PhysBody__wakeRoot 0x48ca50
        if (root) world.wake(root);
        return 1;
    }
    if (cmd == "reset") {  // 0x4a4067
        if (root && !root->asleep) {
            root->vel = Vec3{0.0f, 0.0f, 0.0f};
            root->angular_momentum = Vec3{0.0f, 0.0f, 0.0f};
        }
        c->gear = 0;
        c->gear_ratio = c->ratio[0];
        c->steer_input = 0.0f;
        c->steer_copy = 0.0f;
        c->throttle = 0.0f;
        c->brake_input = 0.0f;
        c->handbrake_input = 0.0f;
        c->rpm_raw = 0.0f;
        c->shaft_speed = 0.0f;
        // +0x1e7c/+0x1e78 are the engine and shaft state in the exe's single
        // struct; src keeps them in the simulation (drive_cars copies back).
        if (c->sim) {
            c->sim->engine.omega = 0.0f;
            c->sim->shaft_speed = 0.0f;
        }
        return 1;
    }
    // 0x4a3360: "activate 0x<player> 0x<camera> 0x<render>" (0x4a1470 slot
    // search): rebind the player ref of the first slot it already owns, else the
    // first free slot. Camera/render ids and the HUD slots are not kept.
    if (cmd.compare(0, 9, "activate ") == 0) {
        uint32_t pid = 0;
        if (!scan_id(cmd, "activate", &pid)) return 0;
        Runtime::Resource *player = rt.resolve(pid);
        if (!player) return 0;
        int slot = -1;
        for (int s = 0; s < c->control_slots && s < 4; s++)
            if (c->slot_player[s] && c->slot_player[s]->id == pid) {
                slot = s;
                break;
            }
        if (slot < 0)
            for (int s = 0; s < c->control_slots && s < 4; s++)
                if (!c->slot_player[s]) {
                    slot = s;
                    break;
                }
        if (slot < 0) return 0;
        c->slot_player[slot] = player;
        return 1;
    }
    // 0x4a37f0: transmission mode -> the gearbox flags. tr&1 is the auto branch;
    // INVENTED: the tr&1 == 0 (manual-clutch) branch is not read, only the 0x30
    // clear is ported for it.
    if (cmd.compare(0, 13, "transmission ") == 0) {
        uint32_t tr = 0;
        scan_id(cmd, "transmission", &tr);
        c->flags &= ~0x30u;
        if (tr & 1) {
            if (tr & 2)
                c->flags &= ~0x40u;
            else
                c->flags |= 0x40u;
            c->flags |= (tr & 4) ? 0x20u : 0x10u;
            c->prev_clutch = c->clutch_input;
        }
        return 1;
    }
    // 0x4a35xx: the multiplier is stored on the player (unused here).
    if (cmd.compare(0, 18, "damage_multiplier ") == 0) return 1;
    // INVENTED: the rest of the 0x4a2e60 chain (cfg keys such as abs, filter,
    // render, reload) is not ported; returning 0 lets the script see it.
    return 0;
}

// 0x4c4400 type_part__vf14 EVENT_COMMAND (0x10): "install" only.
int part_command(Runtime &rt, Runtime::Resource *part, uint32_t code,
                 const Value &param)
{
    if (code != 0x10 || param.kind != Value::Str) return 0; // EVENT_COMMAND
    if (g_rt != &rt || !part || !g_parts.count(part)) return 0;
    const std::string &cmd = param.s;
    // shortcut: the rest of the 0x4c4400 word table (remove, repair, texture,
    // add_part/rem_part, filter, paint, decal, bigdecal, colorscheme, flap_toggle)
    // is not ported; returning 0 leaves those to the script handler.
    if (cmd.compare(0, 7, "install") != 0) return 0;
    if (cmd.size() > 7 && cmd[7] != ' ' && cmd[7] != '\t') return 0;
    // Str__scan "%*s %d %d %d %d %d %f %f %f": cursor, dest, my slot (ignored),
    // target, target slot, optional position.
    int cursor = 0, dest = 0, my_slot = 0, target = 0, t_slot = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f;
    int n = sscanf(cmd.c_str() + 7, "%d %d %d %d %d %f %f %f", &cursor, &dest,
                   &my_slot, &target, &t_slot, &x, &y, &z);
    (void)cursor;
    (void)my_slot;
    Runtime::Resource *tref = rt.resolve((uint32_t)target);
    Runtime::Resource *dst = rt.resolve((uint32_t)dest);
    float pos[3] = {x, y, z};
    Runtime::Resource *used = nullptr;
    int32_t found = install_search(rt, part, tref, dst, &used, &t_slot,
                                   n >= 8 ? pos : nullptr);
    if (!found || !used) return 0; // nothing changes, the script destroys the part
    part_attach(rt, part, found, used, t_slot, dst);
    return 1;
}

// 0x4d2d20 type_player__vf14 EVENT_COMMAND (0x10) -> 0x4d2ab0. The native
// player swallows every command (the Controller script never sees one).
int player_command(Runtime &rt, Runtime::Resource *player, uint32_t code,
                   const Value &param)
{
    if (!player || !g_players.count(player)) return 0;
    if (code == 0x10 && param.kind == Value::Str) {
        uint32_t car_id = 0;
        // "controllable <car id>": send the bind to the car (0x4d2ab0).
        if (scan_id(param.s, "controllable", &car_id)) {
            Runtime::Resource *car = rt.resolve(car_id);
            if (car) {
                char cmd[64];
                // INVENTED: camera/render ids (player+0x218c/+0x219c) are 0, and
                // the player's +0x2310 list / "control" reply are not kept.
                snprintf(cmd, sizeof cmd, "activate 0x%x 0x0 0x0", player->id);
                rt.queue_event(car, nullptr, 0x10, Value::make_str(cmd));
            }
        }
        // Other commands (osd, viewport, camera, renderinstance, device, leave,
        // control): no-op.
    }
    return 1;
}

// 0x47be30 Game__runDriveCallbacks -> 0x47be90 -> type_car vf12
// Vehicle__update 0x49c8e0, once per physics sub-step.
// Called by World::step between the force pass and integrate (0x502060 C);
// sleeping roots are skipped (the exe's sensors never touch the ground while
// the root sleeps, so the spring retires and no force is applied, 0x4f2d70
// step 1). The ray target is the map groundbody.
void drive_cars(float dt)
{
    RigidBody *ground = nullptr;
    for (RigidBody *b : process_world().statics)
        if (b->def.shape.type == SHAPE_MESH && b->def.shape.mesh) {
            ground = b;
            break;
        }
    for (auto &kv : g_cars) {
        CarData *c = kv.second.get();
        if (!c->sim || !c->root_body) continue;
        if (c->root_body->asleep) {
            // 0x49c984 Vehicle__update §0: a sleeping car records its steer and
            // gear so updateControls can tell input from rest.
            if (!(c->flags & 1u)) {
                c->steer_copy = c->steer_input;
                c->gear_copy = c->gear;
                c->flags |= 1u;
            }
            continue;
        }
        Car &sim = *c->sim;
        // INVENTED glue: the exe keeps one struct; src has CarData (natives) and
        // vehicle::Car (simulation), so copy the control inputs and the engine
        // block across per tick (notes/09 "Control path").
        sim.engine.running = (c->flags & 0x4000u) != 0;
        sim.throttle = c->throttle;
        sim.brake = c->brake_input;
        sim.handbrake = c->handbrake_input;
        sim.steer = c->steer_input;
        sim.clutch = c->clutch_input;
        sim.nitro = c->nitro_input;
        sim.gear = c->gear;
        sim.max_steer = c->max_steer;
        // Aero drag (0x49c8e0 §7): e+0x91c / e+0x910.
        sim.c_drag = c->c_drag;
        sim.drag_center = Vec3{c->drag_center[0], c->drag_center[1], c->drag_center[2]};
        sim.final_drive = c->rearend_ratio;
        for (int i = 0; i < kMaxGears; i++) sim.ratios[i] = c->ratio[i];
        Engine &e = sim.engine;
        e.idle = c->idle_omega;
        e.inertia = c->inertia;
        e.starter_torque = c->starter_torque;
        e.friction_fwd = c->friction_fwd;
        e.friction_rev = c->friction_rev;
        e.max_rpm = c->max_rpm;
        e.rpm_limit = c->rpm_limit;
        e.torque_health = c->torque_health;
        e.torque_curve.resize(c->torque_nm.size());
        for (size_t i = 0; i < c->torque_nm.size(); i++) {
            e.torque_curve[i].rpm = i < c->torque_rpm.size() ? c->torque_rpm[i] : 0.0f;
            e.torque_curve[i].value = c->torque_nm[i];
        }
        // INVENTED glue: the WheelRef natives write WheelData and the exe has one
        // struct (every length/force setter reruns Suspension__configure
        // 0x4f4280); src keeps a separate simulation wheel, so copy the wheel
        // fields across before each update. Hub/mount are the compound's
        // (0x5032d0, card 153).
        if (sim.wheels.size() == c->wheels.size())
            for (size_t i = 0; i < sim.wheels.size(); i++) {
                const WheelData &w = c->wheels[i];
                Wheel &sw = sim.wheels[i];
                sw.spring = w.spring_k;
                sw.bump = w.bump;
                sw.rebound = w.rebound;
                sw.rest_len = w.rest_len;
                sw.max_len = w.max_len;
                sw.min_len = w.min_len;
                sw.radius = w.radius;
                sw.friction = w.friction;
                sw.friction_eff = w.friction_eff;
                sw.rolling_resistance = w.roll_res;
                for (int k = 0; k < kPacejkaSlots; k++) sw.pacejka.p[k] = w.pacejka[k];
                sw.set_drive(w.drive_eff);
                sw.set_brake(w.brake_eff);
                sw.set_handbrake(w.hbrake_eff);
                sw.steer_factor = w.steer;
            }
        sim.ground = ground;
        sim.update(dt);
        c->rpm_raw = e.omega;
        c->rpm_smoothed = e.smoothed_omega;
        c->shaft_speed = sim.shaft_speed;
        c->gear_ratio = c->gear >= 0 && c->gear < 8 ? c->ratio[c->gear] : 0.0f;
    }
}

// 0x4b0190 shift timers (SFX and the gearstick bone are not ported).
static void shift_timers(CarData &c, bool up, float now)
{
    if (up) {
        c.shift_t70 = now + 0.5f;
        c.shift_t74 = now + 1.0f;
    } else {
        c.shift_t74 = now + 0.3f;
        c.shift_t70 = now + 0.7f;
    }
}

// 0x4b06b0: upshift when the next gear keeps the engine inside the torque
// band. Returns 1 when a shift started. (e, car, r, T, w).
static int upshift(CarData &c, float r, float T, float w, float now)
{
    if (c.shift_t70 > 0.0f) return 0;
    float G = c.gear_ratio;
    if (std::fabs(G) <= 0.01f ||
        std::fabs(c.rpm_smoothed - w) <= c.rpm_smoothed * 0.1f) {
        int g = c.gear;
        if (g < c.gears) {
            float Gn = c.ratio[g + 1];
            float rn = r / G * Gn;
            float Tn = rn < c.rpm_limit ? c.torque_at(rn) : 0.0f;
            float th = std::min(c.max_rpm * 0.95f, c.rpm_limit * 0.95f);
            if (th < r || G * T < Gn * Tn) {
                c.gear = g + 1;
                c.shift_end = now + 0.3f;
                shift_timers(c, true, now);
                return 1;
            }
        }
    }
    return 0;
}

// 0x4b05a0: downshift when the lower gear pulls harder inside the rev band.
// Returns 1 when a shift started. (e, car, r, T).
static int downshift(CarData &c, float r, float T, float now)
{
    if (c.shift_t74 > 0.0f || c.gear <= 1) return 0;
    float G = c.gear_ratio;
    float Gp = c.ratio[c.gear - 1];
    float rp = r / G * Gp;
    float Tp = rp < c.rpm_limit ? c.torque_at(rp) : 0.0f;
    float th = std::min(c.max_rpm * 0.95f, c.rpm_limit * 0.95f);
    if (rp < th && G * T * 1.15f < Gp * Tp) {
        c.gear--;
        c.shift_end = now + 0.2f;
        shift_timers(c, false, now);
        return 1;
    }
    return 0;
}

// 0x4b19a0: mean kappa of the driven wheels that carry suspension load; 0
// when none. shortcut: the render-object veto (W+0x1e0) is not ported, and
// src has no wheel without a suspension (W+0x30), as in the drive-wheel pass.
static float drive_slip(CarData &c)
{
    if (!c.sim) return 0.0f;
    float sum = 0.0f;
    int n = 0;
    for (const Wheel &w : c.sim->wheels) {
        if (w.drive == 0.0f || !(w.load > 0.0f)) continue;
        sum += w.kappa_prev;
        n++;
    }
    return n ? sum / (float)n : 0.0f;
}

// 0x497930: automatic gearbox (dt unused). Shifts from the shaft speed, the
// throttle/brake difference and the driven-wheel slip.
static void auto_gearbox(CarData &c, float tear, float now)
{
    if (tear <= 0.0f || !(c.flags & 0x4000u) || (c.flags & 0x40004u) ||
        c.shift_end > 0.0f)
        return;
    float lo = c.idle_omega * 1.2f;
    float hi = c.idle_omega * 3.0f;
    float s = c.shaft_speed;
    float thr = c.throttle, brk = c.brake_input;
    float d = thr - brk;
    auto engage = [&] {
        if (c.rpm_smoothed > hi) {
            c.gear = 1;
            shift_timers(c, true, now);
        }
    };
    auto move = [&] {
        float w = c.gear_ratio * s * c.rearend_ratio;
        float r = w * 9.549296f;
        float T = c.rpm_limit <= r ? 0.0f : c.torque_at(r);
        if (r < 0.0f) {
            if (c.shift_t74 > 0.0f) return;
            c.gear = 0;
            c.shift_end = now + 0.2f;
            shift_timers(c, false, now);
            return;
        }
        upshift(c, r, T, w, now);
        downshift(c, r, T, now);
    };
    if (std::fabs(s) <= 1.0f) {  // standstill
        if (c.brake_latch == 0) {
            if (d > 0.1f) {
                if (!c.gears || c.gear != 0) return;
                engage();
                return;
            }
            if (d < -0.1f) {
                if (!c.gears) return;
                c.brake_input = thr;
                c.throttle = brk;
                if (c.gear != 0 || c.rpm_smoothed <= hi) return;
                c.gear = 7;
                shift_timers(c, true, now);
                return;
            }
            if (c.gear == 0 || c.rpm_smoothed >= lo) return;
        } else {
            if (d <= -0.5f) return;
            c.brake_latch = 0;
        }
        c.gear = 0;
        shift_timers(c, false, now);
        return;
    }
    int g = c.gear;
    if (g != 7 && g != 0 && d < 0.0f) {  // braking in a forward gear
        c.brake_latch = 1;
        if (g == 1) return;  // exe: -1 < g && g < 2
        float r = c.gear_ratio * s * c.rearend_ratio * 9.549296f;
        float T = r < c.rpm_limit ? c.torque_at(r) : 0.0f;
        downshift(c, r, T, now);
        return;
    }
    if (d <= 0.1f) {
        if (d < -0.1f) {
            if (!c.gears) return;
            c.brake_input = thr;
            c.throttle = brk;
            if (g == 7 || c.shift_t70 > 0.0f || c.rpm_smoothed <= hi) return;
            c.gear = 7;
            shift_timers(c, true, now);
            return;
        }
        if (c.shift_t74 > 0.0f) return;
        if (g >= 0 && (g < 2 || g == 7)) return;
        move();
        return;
    }
    if (g == 0) {
        if (!c.gears || c.shift_t70 > 0.0f) return;
        engage();
        return;
    }
    if (g == 7) {
        if (c.shift_t74 <= 0.0f) {
            c.gear = 0;
            shift_timers(c, true, now);
            return;
        }
        c.brake_input = thr;
        c.throttle = brk;
        return;
    }
    if (std::fabs(drive_slip(c)) >= 0.2f) return;  // driven-wheel slip too high
    move();
}

// 0x4977a0 (e, car, dt): automatic clutch. Moves clutch_input toward the
// launch target: 1 (open) above the 1.2*limiter shaft speed, else the launch
// curve between idle and the launch rpm.
static void auto_clutch(CarData &c, float tear, float dt)
{
    if (tear <= 0.0f || !(c.flags & 0x4000u)) return;
    float G = c.gear_ratio;
    if (std::fabs(G) < 0.01f) return;
    float rpm = c.rpm_smoothed;
    float pred = (rpm - c.prev_rpm) + rpm;
    c.prev_rpm = rpm;
    float launch = (std::fabs(G / c.ratio[1]) * c.throttle * 0.56f + 0.24000001f) *
                       (c.rpm_max_torque * 0.10471976f - c.idle_omega) +
                   c.idle_omega;
    float ws = c.shaft_speed * G;
    float lim = std::min(c.max_rpm, c.rpm_limit);
    float target = 1.0f;
    if (ws * c.rearend_ratio <= lim * 0.12566371f) {  // 1.2*lim in rad/s
        target = c.clutch_input;
        if (!(rpm < ws * c.rearend_ratio || launch <= pred)) {
            if (c.idle_omega < pred) {
                target = 1.0f - (pred - c.idle_omega) / (launch - c.idle_omega);
                if (target <= c.clutch_input) target = c.clutch_input;
            } else {
                target = 1.0f;
            }
        }
    }
    if (c.clutch_input <= target) {
        float p = c.prev_clutch;
        float k = std::min(dt * (target <= p ? 33.333336f : 200.0f), 1.0f);
        c.clutch_input = k * (target - p) + p;
    }
    c.prev_clutch = c.clutch_input;
}

// 0x498300 _type_car vf11 Vehicle__updateControls: the mode-8 control list
// 0x47bc20 calls it for every car once per physics tick, before the sub-steps.
// Reads the driver slot's Controller axes into CarData, shifts gears and wakes
// a sleeping root on input (0x498599). dt is the tick accumulator.
void control_cars(float dt)
{
    if (!g_rt) return;
    World &world = process_world();
    float now = world.time;
    for (auto &kv : g_cars) {
        CarData *c = kv.second.get();
        if (!c->sim) continue;
        // shortcut: the traffic AI pointer (car+0x2054) is not modelled.
        float old_thr = c->throttle;
        float old_brk = c->brake_input;
        float old_clu = c->clutch_input;
        // The exe's tear is the car part payload's +0xb4 (PartData::tear).
        auto pi = g_parts.find(kv.first);
        float tear = pi != g_parts.end() ? pi->second->tear : 1.0f;
        if (!(tear > 0.0f)) {
            // shortcut: +0x1f64/68/6c (push/lift/roll) are not kept.
            c->handbrake_input = 0.0f;
            c->brake_input = 0.0f;
            c->throttle = 0.0f;
        } else if (c->driver_slot < 0) {
            c->horn_id = 0;
            c->brake_input = 0.0f;
            c->throttle = 0.0f;
        } else if (!c->slot_player[c->driver_slot]) {
            c->throttle = 0.0f;
            c->brake_input = 0.0f;
            c->horn_id = 0;
        } else {
            input::Controller *p =
                natives::controller_of(c->slot_player[c->driver_slot]);
            auto ax = [p](int v) { return p ? p->get_axis_val(v) : 0.0f; };
            // ai(v): the native player answers int(val * 127) (notes/09).
            auto ai = [&ax](int v) {
                return (float)(int)(ax(v) * 127.0f) * (1.0f / 127.0f);
            };
            c->steer_input = ax(1);         // TURN_LEFTRIGHT
            c->throttle = ax(28);
            c->brake_input = ax(29);        // +0x1dec holds the same value
            c->handbrake_input = ax(48);
            c->clutch_input = ai(49);
            c->nitro_input = ai(31);
            float shift = ai(50);           // GEAR_UPDOWN
            float horn_ax = ai(54);         // HORN
            float gearset = ai(51);         // GEAR_SET
            if (c->flags & 0x40000u) {      // "brake" command
                c->throttle = 0.0f;
                c->brake_input = 1.0f;
                c->handbrake_input = 1.0f;
                c->nitro_input = 0.0f;
                shift = 0.0f;
                c->gear = 0;
            }
            if ((horn_ax > 0.1f) != (c->horn_axis > 0.1f))
                c->horn_id = (c->horn_id + 1) & 3;
            c->horn_axis = horn_ax;
            // Gear shift: the 0x400000 latch clears while |shift| <= 0.1.
            if (shift > 0.1f) {
                if (!(c->flags & 0x400000u)) {
                    c->flags |= 0x400000u;
                    if (c->gears) {
                        if (c->gear == 7) {
                            c->gear = 0;
                            c->shift_end = now + 0.2f;
                        } else if (c->gear < c->gears) {
                            c->gear++;
                            c->shift_end = now + 0.3f;
                        }
                        shift_timers(*c, true, now);
                    }
                }
            } else if (shift < -0.1f) {
                if (!(c->flags & 0x400000u)) {
                    c->flags |= 0x400000u;
                    if (c->gears) {
                        if (c->gear == 0) {
                            c->gear = 7;
                            c->shift_end = now + 0.3f;
                        } else if (c->gear != 7) {
                            c->gear--;
                            c->shift_end = now + 0.2f;
                        }
                        shift_timers(*c, false, now);
                    }
                }
            } else {
                c->flags &= ~0x400000u;
            }
            // DAT_005fe31c is 0, so only a positive gearset acts.
            if (gearset > 0.0f && c->gearset_last != gearset) {
                c->gearset_last = gearset;
                int g = (int)(gearset * 7.0f + 0.5f);
                if (g > 7) g = 7;
                if (g != c->gear && ((g >= 0 && g <= c->gears) || g == 7)) {
                    c->gear = g;
                    shift_timers(*c, false, now);
                }
            }
        }
        if (c->flags & 0x40000u) {
            c->brake_input = 1.0f;
            c->handbrake_input = 1.0f;
            c->nitro_input = 0.0f;
            c->gear = 0;
            c->throttle = 0.0f;
        }
        // 0x497930 auto gearbox, 0x4977a0 auto clutch (transmission flags 0x10
        // and 0x40, set by player_transmission 0x4a37f0).
        if (c->flags & 0x10u) auto_gearbox(*c, tear, now);
        if (c->flags & 0x40u) auto_clutch(*c, tear, dt);
        // 0x4b04a0 shift timers.
        if (c->starter_timer >= 0.0f) c->throttle = 0.0f;
        if (c->shift_end > 0.0f) {
            if (c->shift_end <= now) {
                c->shift_end = -1.0f;
                c->shifting = 0;
            } else if (!(c->flags & 0x30u)) {   // manual clutch
                c->throttle = 0.0f;
                c->clutch_input = 1.0f;
                c->shifting = 1;
            } else {
                c->clutch_input = 0.0f;
                c->shifting = 1;
            }
        }
        // shortcut: 0x4b03c0 (shift completion) is not ported.
        if (c->shift_t70 > 0.0f && c->shift_t70 <= now) c->shift_t70 = -1.0f;
        if (c->shift_t74 > 0.0f && c->shift_t74 <= now) c->shift_t74 = -1.0f;
        if (c->shift_t70 < 0.0f && c->shift_t74 < 0.0f) c->shifting = 0;
        // shortcut: semi-auto 0x4ad480 (flags 0x20), speed limiter 0x1000,
        // steer assist 0x2000, flags 0x80/0x100/0x4000000 helpers and the
        // aero/slot-object axis lists are later cards.
        c->flags &= ~0x03000000u;
        float k = std::min(10.0f * dt, 1.0f);
        c->throttle = old_thr + (c->throttle - old_thr) * k;
        c->brake_input = old_brk + (c->brake_input - old_brk) * k;
        float kc = std::min(33.333336f * dt, 1.0f);
        c->clutch_input = old_clu + (c->clutch_input - old_clu) * kc;
        // 0x4aa830 (wheel steer geometry) is vehicle::Car's job, from steer.
        if (!(c->flags & 1u)) {
            if (std::fabs(c->steer_copy - c->steer_input) <= 0.001f) continue;
        } else {
            bool asleep = c->root_body && c->root_body->asleep;
            if (asleep && std::fabs(c->throttle) <= 0.01f &&
                std::fabs(c->steer_input - c->steer_copy) <= 0.01f &&
                c->gear == c->gear_copy)
                continue;
            c->flags = (c->flags & ~1u) | 2u;
            if (asleep) world.wake(c->root_body);  // 0x507060 via 0x498599
        }
        c->steer_copy = c->steer_input;
    }
}

int car_body_instances()
{
    int n = 0;
    for (const auto &kv : g_cars)
        if (kv.second->root_body) n++;
    return n;
}

std::vector<CarData *> car_payloads()
{
    std::vector<CarData *> out;
    for (auto &kv : g_cars) out.push_back(kv.second.get());
    return out;
}

void bind_wheel(Object *wrapper, WheelData *w)
{
    if (wrapper && w) g_wheels[wrapper] = w;
}

WheelData *wheel_data(Object *wrapper)
{
    auto it = g_wheels.find(wrapper);
    return it == g_wheels.end() ? nullptr : it->second;
}

void bind_sfx_table(Object *wrapper, audio::SfxTable *t)
{
    if (wrapper && t) g_sfx_tables[wrapper] = t;
}

audio::SfxTable *sfx_table(Object *wrapper)
{
    auto it = g_sfx_tables.find(wrapper);
    return it == g_sfx_tables.end() ? nullptr : it->second;
}

EngineModel *dyno_data(Object *wrapper)
{
    auto it = g_dynos.find(wrapper);
    return it == g_dynos.end() ? nullptr : it->second.get();
}

EngineModel *attach_dyno(Object *wrapper)
{
    if (!wrapper) return nullptr;
    auto it = g_dynos.find(wrapper);
    if (it == g_dynos.end())
        it = g_dynos.emplace(wrapper, std::make_unique<EngineModel>()).first;
    return it->second.get();
}

void detach_dyno(Object *wrapper)
{
    auto it = g_dynos.find(wrapper);
    if (it == g_dynos.end()) return;
    it->second->free_tables();
    g_dynos.erase(it);
}

// 0x4aebf0 Vehicle__refreshParameters (curve block).
void refresh_engine_curve(CarData &c)
{
    EngineModel *d = c.dyno;
    if (!d) {
        c.rpm_max_torque = 0.0f;
        c.torque_rpm.assign(1, 0.0f);
        c.torque_nm.assign(1, 0.0f);
        c.nitro_rpm.assign(1, 0.0f);
        c.nitro_nm.assign(1, 0.0f);
        return;
    }
    c.rpm_max_torque = d->rpm_max_torque;
    if (d->torque_table[0].empty()) {
        c.torque_rpm.assign(1, 0.0f);
        c.torque_nm.assign(1, 0.0f);
        c.nitro_rpm.assign(1, 0.0f);
        c.nitro_nm.assign(1, 0.0f);
        return;
    }
    size_t n = d->count > 0 ? (size_t)d->count : 0;
    c.torque_rpm.resize(n);
    c.torque_nm.resize(n);
    c.nitro_rpm.resize(n);
    c.nitro_nm.resize(n);
    for (size_t i = 0; i < n; i++) {
        float rpm = (float)i * (float)d->step;
        c.torque_rpm[i] = rpm;
        c.torque_nm[i] = d->torque_table[0][i];
        c.nitro_rpm[i] = rpm;
        c.nitro_nm[i] = d->torque_table[1].empty() ? d->torque_table[0][i]
                                                   : d->torque_table[1][i];
    }
}

// shortcut: payloads keyed by Runtime address; a Runtime destroyed and recreated
// at the same address would see stale entries (tools create one per process).
void reset(Runtime *rt)
{
    // INVENTED: the exe never discards one runtime's bodies; src tools create
    // several. Unlink before the payloads (and their bodies) are freed.
    World &world = process_world();
    for (const auto &kv : g_cars) {
        if (kv.second->chassis_body) world.unlink(kv.second->chassis_body);
        if (kv.second->root_body) world.unlink(kv.second->root_body);
    }
    g_wheels.clear();
    g_sfx_tables.clear();
    g_dynos.clear();
    g_players.clear();
    g_cars.clear();
    g_parts.clear();
    g_rt = rt;
}

}  // namespace game
