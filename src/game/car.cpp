// Car / part payloads behind the Chassis, WheelRef and Part natives.
// Offsets, defaults and setter formulas: notes/13-car-structs.md §6-§8 and the
// native summaries in §12. Wheels are built from the car cfg `type`/`spring`/
// `wheel`/`friction`/`wheelpars` lines (createCar), slots from the part cfg.
#include "game/car.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <memory>
#include <set>
#include <utility>

#include "parts/cfg.h"
#include "resource/manager.h"
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
                     CfgFile *cfg)
{
    for (Runtime::Resource *n = r; n; n = n->parent) {
        if (!n->package || !n->record) continue;
        for (const DataRef &ref : rt.resources().refs(*n->package, *n->record)) {
            if (ref.kind != DataRef::Native || ref.native != native ||
                !ref.is_cfg || !ref.file || ref.path.empty())
                continue;
            std::string err;
            if (rt.resources().load_cfg(ref, cfg, &err)) return true;
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

void fill_part(PartData &p, const CfgFile &cfg)
{
    if (!cfg.textures.empty()) p.texture = (int32_t)cfg.textures[0].id;
    if (!cfg.meshes.empty()) p.mesh = (int32_t)cfg.meshes[0].id;
    if (!cfg.renders.empty()) p.render_type = (int32_t)cfg.renders[0].id;
    // INVENTED: which cfg body line P+0x1c binds to is UNRESOLVED; use the first.
    // 0x4bf4d0 passes this mass to Physics__createBody, whose inverse mass is 1/m.
    if (!cfg.bodies.empty() && cfg.bodies[0].mass > 0.0f)
        p.inv_mass = 1.0f / cfg.bodies[0].mass;
    for (const CfgSlot &cs : cfg.slots) {
        Slot s;
        s.id = cs.id;
        s.type_pos[0] = cs.xform.px;
        s.type_pos[1] = cs.xform.py;
        s.type_pos[2] = cs.xform.pz;
        float ang[3] = {cs.xform.rx, cs.xform.ry, cs.xform.rz};
        euler_to_mat(&s.type_rot, ang);
        for (int i = 0; i < 3; i++) s.pos[i] = s.type_pos[i];
        s.rot = s.type_rot;
        for (const CfgAttach &at : cs.attaches)
            s.attaches.push_back(at.part);
        p.slots.push_back(std::move(s));
    }
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
    if (load_native_cfg(rt, r, "part", &cfg)) fill_part(*up, cfg);
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

bool load_car_cfg(Runtime &rt, Runtime::Resource *r, CfgFile *cfg)
{
    if (!r) return false;
    if (load_native_cfg(rt, r, "car", cfg)) return true;
    auto it = g_cars.find(r);
    if (it == g_cars.end() || !it->second->cfg_resource) return false;
    return load_native_cfg(rt, it->second->cfg_resource, "car", cfg);
}

// 0x4c1f90 Part__initInstance (body part) + 0x4992e0 Vehicle__createCar
// (body part). The cfg `body` line handler 0x4bf4e0 sets M+0xff0; no body line
// (-1) means neither body exists.
void create_car_bodies(Runtime &rt, Runtime::Resource *inst,
                       Runtime::Resource *type, const std::string &params)
{
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
    // parent link on car_root. The body's own shape mass is not counted: only
    // the cfg `body` children, the per-wheel wheelbone child and the 0.01 kg
    // trigger-sensor capsule (0x4f3f10).
    // shortcut: no child bodies exist yet, so the children's own shape inertia
    // and the COM re-centring of child offsets/pose are not ported; the mass
    // and the point-mass parallel-axis terms are summed from the cfg records.
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
    float mass = part_mass;
    Vec3 com{0.0f, 0.0f, 0.0f};
    for (const CfgBody &b : cfg.bodies) {
        if (b.mass <= 0.0f) continue;
        com += Vec3{b.xform.px, b.xform.py, b.xform.pz} * b.mass;
    }
    for (const WheelData &w : car->wheels) {
        float m = (w.bone_mass > 0.0f ? w.bone_mass : 0.0f) + 0.01f;
        mass += m;
        com += Vec3{w.hub[0], w.hub[1], w.hub[2]} * m;
    }
    if (mass > 0.0f) com = com * (1.0f / mass);
    Mat34 inertia{};
    for (const CfgBody &b : cfg.bodies)
        if (b.mass > 0.0f)
            add_point_inertia(&inertia, b.mass,
                              Vec3{b.xform.px, b.xform.py, b.xform.pz} - com);
    for (const WheelData &w : car->wheels) {
        float m = (w.bone_mass > 0.0f ? w.bone_mass : 0.0f) + 0.01f;
        add_point_inertia(&inertia, m, Vec3{w.hub[0], w.hub[1], w.hub[2]} - com);
    }
    if (car->root_body) {
        if (mass > 0.0f) {
            car->root_body->def.inv_mass = 1.0f / mass;
            car->root_body->def.linear_drag = -0.005f * mass;
            car->root_body->def.angular_drag = -0.01f * mass;
            mat_inverse(&car->root_body->def.inv_inertia, &inertia);
        } else {
            car->root_body->def.inv_mass = 0.0f;
            car->root_body->def.flags |= 4u;  // static
        }
    }
    car->com[0] = com.x;
    car->com[1] = com.y;
    car->com[2] = com.z;

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
        s.mount = Vec3{w.hub[0] - com.x, w.hub[1] - com.y, w.hub[2] - com.z};
        s.rest_len = w.rest_len;
        s.max_len = w.max_len;
        s.min_len = w.min_len;
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
// INVENTED: the call runs before World::step's force pass instead of between
// runControllers and integrate (src integrates from the pre-force snapshot, so
// the impulses must land first); sleeping roots are skipped (the exe's sensors
// never touch the ground while the root sleeps, so the spring retires and no
// force is applied, 0x4f2d70 step 1). The ray target is the map groundbody.
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
        if (!c->sim || !c->root_body || c->root_body->asleep) continue;
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
        sim.ground = ground;
        sim.update(dt);
        c->rpm_raw = e.omega;
        c->rpm_smoothed = e.smoothed_omega;
        c->shaft_speed = sim.shaft_speed;
        c->gear_ratio = c->gear >= 0 && c->gear < 8 ? c->ratio[c->gear] : 0.0f;
    }
}

int car_body_instances()
{
    int n = 0;
    for (const auto &kv : g_cars)
        if (kv.second->root_body) n++;
    return n;
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
