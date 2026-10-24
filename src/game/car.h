// Car / part instance model behind the Chassis, WheelRef and Part natives.
//
// The exe keeps one 0x2284-byte payload per part instance (notes/13 §3), four
// 0x2e4-byte wheel records plus 0xc8-byte templates on the vehicle manager, and
// a per-part runtime slot array parallel to the type-slot list (notes/13 §8).
// The rebuild keys the same data by Runtime::Resource (Part/Chassis payloads)
// and by script object (lightweight WheelRef wrappers).
#pragma once
#include <stdint.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

#include "audio/sfx.h"
#include "engine/math.h"
#include "game/runtime.h"
#include "parts/cfg.h"
#include "physics/physics.h"
#include "vehicle/vehicle.h"

class Vm;
struct Object;
struct EngineModel;
struct CfgFile;

namespace game {

constexpr int kPacejkaSlots = 17;

struct CarData;

// Wheel record W (notes/13 §7). Only the fields the WheelRef natives touch are
// kept; solver state and the embedded suspension object stay out until the
// vehicle simulation owns the wheels.
struct WheelData {
    CarData *car = nullptr;
    int index = 0;

    // Wheel template record (notes/13 §6): mount pos/Ypr, damage offset base,
    // base length and travel used by getPos/setPos.
    float template_pos[3] = {};
    float template_ypr[3] = {};
    float template_damage = 0.0f;
    float template_len = 0.1f;
    float template_travel = 0.0f;
    float bone_mass = 0.0f;     // cfg `type` 1st value = wheelbone child mass

    float base_len = 0.1f;      // +0x38
    float travel = 0.0f;        // +0x3c
    float damp_ratio = 0.0f;    // +0x40 cfg `spring` 3rd value
    float spring_k = 0.0f;      // +0x44
    float bump = 0.0f;          // +0x48
    float rebound = 0.0f;       // +0x4c
    float rest_len = 0.1f;      // +0x50
    float max_len = 0.2f;       // +0x54
    float min_len = -1.0f;      // +0x58
    float free_len = 0.42f;     // +0x5c = rest + radius
    float radius = 0.32f;       // +0x60
    float friction = 1.0f;      // +0x64/+0x68
    float friction_eff = 1.0f;  // +0x6c/+0x70
    float roll_res = 0.0f;      // +0x74
    float hub[3] = {};          // +0x78
    Mat34 rot = {};             // +0x90 (3x4 orientation)
    float spin = 0.0f;          // +0xc0 rad/s
    float drive_ratio = 0.0f;   // +0xc4
    float steer = 0.0f;         // +0xc8
    float drive = 0.0f;         // +0xcc
    float brake = 0.0f;         // +0xd0
    float hbrake = 0.0f;        // +0xd4
    float brake_eff = 0.0f;     // +0xd8
    float hbrake_eff = 0.0f;    // +0xdc
    float drive_eff = 0.0f;     // +0xe0
    float spin_angle = 0.0f;    // +0xe8
    float damage_visual = 0.0f; // +0xf0
    float wobble = 0.0f;        // +0xf4
    float wobble_phase = 0.0f;  // +0xf8
    uint32_t flags = 0;         // +0x100
    Mat34 pose = {};            // +0x110
    float tyre_wear = 0.0f;     // +0x15c
    float brake_wear = 0.0f;    // +0x164
    float half_width = 0.1f;    // +0x1d4
    float pacejka[kPacejkaSlots] = {};

    float arm_len = 0.0f;       // +0x234
    float arm_axis[3] = {};     // +0x238
    float arm_pivot[3] = {};    // +0x244
    WheelData *opp = nullptr;   // +0x250
    float hub_len = 0.0f;       // +0x254
    float hub_p1[3] = {};       // +0x258
    float hub_p2[3] = {};       // +0x264
    float hub_pc[3] = {};       // +0x270
    float ic_h[3] = {};         // +0x27c
    float ic_l[3] = {};         // +0x288

    // Suspension fields targeted by setCPatch (S = W+0x30 in the exe).
    float cpatch_halfwidth = 0.0f;
    float cpatch_sin = 0.0f;
    float cpatch_offset = 0.0f;

    WheelData();

    void set_pos(const float pos[3]);
    void set_ypr(const float *ypr);  // null restores the template Ypr
    void set_hub(float len, const float p1[3], const float p2[3], const float pc[3]);
    void set_arm(float len, const float axis[3], const float pivot[3]);
    void set_friction(float v);
    void set_brake(float v);
    void set_hbrake(float v);
    void set_radius(float v);
    void set_rest_len(float v);
    void reset_damage();
};

// Type + runtime slot pair (notes/13 §8). The type pose is immutable; the
// runtime pose starts as a copy and is what the damage/pose natives rewrite.
struct Slot {
    int32_t id = 0;
    uint32_t flags = 0;          // 0x1 present, 0x10 disabled, 0x20 override, 0x40 hard
    float type_pos[3] = {};
    Mat34 type_rot = {};
    float pos[3] = {};
    Mat34 rot = {};
    int32_t mate_id = 0;
    Object *attached = nullptr;  // installed part script instance
    // Type-slot graph (0x4c9d90): cfg attach/compatible (part id, slot id).
    std::vector<CfgAttach> attaches;
    std::vector<CfgAttach> compatibles;
};

struct PartData {
    std::vector<Slot> slots;
    float tear = 1.0f;           // +0xb4
    float wear = 0.0f;           // +0xb8; getWear returns 1 - wear
    float max_wear_inv = 1.0f;   // +0xbc = 1/max wear
    float inv_mass = 0.0f;       // P+0x1c body -> body+0xb4 -> def+0x5c -> +0x2c (inverse mass)
    int32_t texture = 0;         // +0x84
    int32_t mesh = 0;            // +0x94
    int32_t render_type = 0;     // +0xa4
    int32_t car_id = 0;          // +0xc8
    int32_t wheel_id = 0;        // +0xd0
    float sfx_loop[2] = {1.0f, 1.0f};  // +0x144/+0x148
    Runtime::Resource *car_ref = nullptr;  // +0xcc, typed car Part object
    int32_t installed = 0;               // +0x74, Part__commitInstall 0x4c7d20
    // INVENTED: the exe instance's typed data points at its type record; src has
    // no such link on a created instance, so keep the type the cfg came from
    // (same mapping as CarData::cfg_resource, card 150).
    Runtime::Resource *type = nullptr;
};

struct CarData {
    std::vector<WheelData> wheels;

    uint32_t flags = 0;          // +0x70
    float throttle = 0.0f;       // +0x1de4
    float brake_input = 0.0f;    // +0x1de8
    float handbrake_input = 0.0f;   // +0x1df4
    float steer_input = 0.0f;    // +0x1ddc
    float steer_copy = 0.0f;     // +0x1de0 (written when the body falls asleep)
    float clutch_input = 0.0f;   // +0x1df0
    float nitro_input = 0.0f;    // +0x1df8
    int32_t gear = 0;            // +0x1e5c
    int32_t gear_copy = 0;       // +0x1e60
    int32_t horn_id = 0;         // +0x1e00
    // Control slots / driver slot from the car cfg (M+0x17ec / M+0x17f0,
    // 0x48dee0); slot_player[s] is R_s's player ref (car+0x150 + s*0x4a8).
    int control_slots = 0;
    int driver_slot = -1;
    Runtime::Resource *slot_player[4] = {};
    float starter_timer = -1.0f; // +0x1e6c (0x499511; shift lock while >= 0)
    float shift_end = -1.0f;     // +0x1e68 shift completes at this world time
    float shift_t70 = -1.0f;     // +0x1e70 0x4b0190 timer
    float shift_t74 = -1.0f;     // +0x1e74 0x4b0190 timer
    float horn_axis = 0.0f;      // previous ai(54), horn edge detect
    float gearset_last = 0.0f;   // +0x2280 last ai(51) gearset
    float prev_clutch = 0.0f;    // +0x1fa8 last clutch written by updateControls
    float gear_ratio = 0.0f;     // +0x1e0c ratio[gear] (Vehicle__update 0x498b40)
    float shaft_speed = 0.0f;    // +0x1e78 driveshaft / wheel-average speed
    float rpm_raw = 0.0f;        // +0x1e7c
    float rpm_smoothed = 0.0f;   // +0x1e80
    float engine_temp = 20.0f;   // +0x1ec0  0x4992e0 writes 0x41a00000
    float engine_temp_ambient = 20.0f;  // +0x1ec4  0x4992e0 writes 0x41a00000
    float engine_damage = 0.0f;  // +0x1e9c
    float torque_health = 1.0f;  // +0x1e04
    float nitro_health = 1.0f;   // +0x1e08
    float mileage = 0.0f;        // +0x2250
    float nitro = 0.0f;          // +0x1f5c
    float crime = -1.0f;         // +0x2258

    // Engine block e (notes/13 §9). The exe reaches it as `e = *(car+0x2050)`;
    // its aggregate is modelled here like exhaust_min_vol/heat_capacity.
    audio::EngineSfx engine_sfx;
    float max_steer = 0.7f;      // +0xa20
    int32_t material_index = -1; // +0xc2c
    EngineModel *dyno = nullptr; // +0xc30 DynoData native (owned by the DynoData object, card 008)
    float rpm_max_torque = 0.0f; // +0x14 (from dyno +0x1c0)
    int32_t shifting = 0;        // +0x10
    float cooling_low = 10.0f;   // +0xa5c
    float cooling_air = 50.0f;   // +0xa60
    float cooling_speed = 0.01f; // +0xa64
    float exhaust_min_vol = 0.0f;    // +0x7d4
    float steer_wheel_radius = 0.0f; // +0xa28
    float steer_wheel_angle = 0.0f;  // +0xa2c
    float ackermann = 0.0f;      // +0xa30
    float torque_base = 1.0f;    // +0xd4
    float nitro_torque_base = 1.0f;  // +0xd8
    float max_rpm = 6500.0f;     // +0x18   INVENTED default (template not traced)
    float rpm_limit = 6800.0f;   // +0x1c   INVENTED default
    int32_t gears = 0;           // +0x20
    float ratio[8] = {};         // +0x24..0x40
    float rearend_ratio = 0.0f;  // +0x44
    float clutch_f = 500.0f;     // +0xcc  template default (notes/09)
    float starter_torque = 50.0f;    // +0xd0  template default
    float inertia = 10.0f;       // +0xe4  template default
    float idle_omega = 83.86f;   // +0xf0  template default (rad/s)
    float friction_fwd = 1e-4f;  // +0xf4  template default
    float friction_rev = 0.03f;  // +0xf8  template default
    float rpm_trans_fwd = 0.0f;  // +0x808
    float rpm_trans_rev = 0.0f;  // +0x80c
    float sfx_starter_rpm = 0.0f;    // +0x810
    float drag_center[3] = {};   // +0x910
    float c_drag = 0.0f;         // +0x91c
    float tank_nitro = 0.0f;     // +0xa4c
    float consumption_nitro = 0.0f;  // +0xa58
    // Car thermal starts, Vehicle__createCar 0x4992e0 (puVar8[0x7af..]).
    float heat_capacity = 320000.0f;  // +0x1ebc  0x489c4000
    float cooling_coeff = 9.375e-05f; // +0x1ec8  0x38c49ba6 = 30 / 320000

    // Vehicle__createCar 0x4992e0: "part_comp" body (car+0x13ec) and the
    // "car_root" box it hangs under. Owned here; the process world links them.
    // car_root IS sim->body, so Vehicle__update's impulses land on it.
    std::unique_ptr<RigidBody> chassis_body_storage;
    std::unique_ptr<Car> sim;   // 0x47d560 drive list -> Vehicle__update 0x49c8e0
    RigidBody *chassis_body = nullptr;
    RigidBody *root_body = nullptr;
    // Compound COM offset (0x5032d0), car_root local (card 146).
    float com[3] = {};
    // INVENTED: the exe instance's typed data points at its type record; src has
    // no such link on a created instance, so keep the type the cfg came from.
    Runtime::Resource *cfg_resource = nullptr;

    // Engine torque curves: (rpm, torque) pairs, e+0/e+4 and e+8/e+0xc.
    std::vector<float> torque_rpm, torque_nm;
    std::vector<float> nitro_rpm, nitro_nm;

    float torque_at(float rpm) const;
    float nitro_torque_at(float rpm) const;

    CarData();
};

// Lazy per-resource payloads, created on first native access.
PartData *part_data(Runtime &rt, Runtime::Resource *r);
CarData *car_data(Runtime &rt, Runtime::Resource *r);
// The `car` cfg on the record/super-record chain (same walk as car_data).
bool load_car_cfg(Runtime &rt, Runtime::Resource *r, CfgFile *cfg,
                  const Package **pkg = nullptr);
// 0x4c1f90 (Part__initInstance) + 0x4992e0 (Vehicle__createCar) body parts:
// create the car's "part_comp" and "car_root" bodies in the process world,
// asleep. No-op unless the type chain's car cfg has a `body` line
// (0x4bf4e0 sets M+0xff0; -1 = no body line).
void create_car_bodies(Runtime &rt, Runtime::Resource *inst,
                       Runtime::Resource *type, const std::string &params);
// 0x4a2e60 type_car__vf14 EVENT_COMMAND (0x10) branch: "brake", "start",
// "suspend", "wakeup", "reset". Returns 1 when the command was consumed (the
// script handleEvent is then suppressed), 0 for anything else.
int car_command(Runtime &rt, Runtime::Resource *car, uint32_t code,
                const Value &param);
// 0x4c4400 type_part__vf14 EVENT_COMMAND (0x10) branch: "install ..."
// (Part__installSearch 0x4c96c0 + Part__attach 0x4cb1d0 subset, card 150).
// Returns 1 when the command was consumed (the script handleEvent is then
// suppressed), 0 for anything else.
int part_command(Runtime &rt, Runtime::Resource *part, uint32_t code,
                 const Value &param);
// 0x4d2d20 type_player__vf14 EVENT_COMMAND: "controllable <car id>" queues the
// "activate ..." bind to the car. Returns 1 for every code on a native player
// (the Controller script never gets commands), 0 otherwise.
int player_command(Runtime &rt, Runtime::Resource *player, uint32_t code,
                   const Value &param);
// 0x47be30 Game__runDriveCallbacks: Vehicle__update 0x49c8e0 for every awake
// car, once per physics sub-step.
void drive_cars(float dt);
// 0x47bc20 Game__runControlCallbacks -> 0x498300 Vehicle__updateControls for
// every car, once per physics tick before the sub-steps.
void control_cars(float dt);
// Number of CarData payloads that own car bodies (vmboot body_count check).
int car_body_instances();
void bind_wheel(Object *wrapper, WheelData *w);
WheelData *wheel_data(Object *wrapper);
// SfxTable native handle payload. No producer yet (Chassis.getSfxTable 0x4b3890
// is a stub), so sfx_table() is always null.
void bind_sfx_table(Object *wrapper, audio::SfxTable *t);
audio::SfxTable *sfx_table(Object *wrapper);
// DynoData native handle (Native__instanceToHandle 0x4ecc70 / FUN_004ed320).
EngineModel *dyno_data(Object *wrapper);
EngineModel *attach_dyno(Object *wrapper);  // creates if missing, returns it
void detach_dyno(Object *wrapper);          // frees it
// Curve block of Vehicle__refreshParameters 0x4aebf0 (dyno -> e+0..0xc, e+0x14).
void refresh_engine_curve(CarData &c);
// Drop all payloads when a new Runtime takes over (tests/tools create several).
void reset(Runtime *rt = nullptr);

std::string wheel_damage(const WheelData &w);
void set_wheel_damage(WheelData &w, const std::string &s);
std::string slot_damage(const Slot &s);
void set_slot_damage(Slot &s, const std::string &s2);

// 0x569970 Math__matToYpr (shared by the slot/wheel damage strings).
void mat_to_ypr(const Mat34 &m, float out[3]);

}  // namespace game
