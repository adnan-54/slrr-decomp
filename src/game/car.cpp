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
#include <utility>

#include "parts/cfg.h"
#include "resource/manager.h"
#include "vehicle/dyno.h"

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

Runtime *g_rt = nullptr;
std::map<Runtime::Resource *, std::unique_ptr<PartData>> g_parts;
std::map<Runtime::Resource *, std::unique_ptr<CarData>> g_cars;
std::map<Object *, WheelData *> g_wheels;
std::map<Object *, std::unique_ptr<EngineModel>> g_dynos;

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
        if (ln.key == "type") {
            WheelData w;
            w.index = (int32_t)c.wheels.size();
            w.car = &c;
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

void bind_wheel(Object *wrapper, WheelData *w)
{
    if (wrapper && w) g_wheels[wrapper] = w;
}

WheelData *wheel_data(Object *wrapper)
{
    auto it = g_wheels.find(wrapper);
    return it == g_wheels.end() ? nullptr : it->second;
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
    g_wheels.clear();
    g_dynos.clear();
    g_cars.clear();
    g_parts.clear();
    g_rt = rt;
}

}  // namespace game
