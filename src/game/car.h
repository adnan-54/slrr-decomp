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
#include "game/traffic.h"
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
    int32_t slot_type = 0;       // type slot +0x64, cfg `slottype` (0x4c0190)
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
    // 0x4942b0 appends every reported part to the car list car+0xfc.
    std::vector<Runtime::Resource *> part_list;

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
    int32_t brake_latch = 0;     // +0x1e64 0x497930 brake-to-reverse latch
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
    int32_t semi_shifts = 0;     // INVENTED: 0x4ad480 shifts started, vmboot only
    float horn_axis = 0.0f;      // previous ai(54), horn edge detect
    float gearset_last = 0.0f;   // +0x2280 last ai(51) gearset
    float prev_clutch = 0.0f;    // +0x1fa8 last clutch written by updateControls
    float prev_rpm = 0.0f;       // +0x1fac 0x4977a0 previous smoothed rpm
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

// Car snapshot S built by 0x47f980 vf11 and passed to 0x484b90/0x485e10/
// 0x486020 (0xc8 stack bytes). Offsets are the exe's; src keeps it next to the
// bot so vmboot can read the last tick.
struct BotSnapshot {
    int32_t gear = 0;        // +0x00 getInfo(0x36) = car+0x1e5c
    float pos[3] = {};       // +0x04 getInfo(2), y corrected from the wheels
    float vel_dir[3] = {};   // +0x10 getInfo(3), y = 0, xz normalised
    float fwd[3] = {};       // +0x1c getInfo(4) = -(R col 2), xz normalised
    float ang_vel[3] = {};   // +0x28 getInfo(0x33) = R^T omega
    float euler[3] = {};     // +0x34 0x569970(R)
    int32_t wheels = 0;      // +0x40 getInfo(0x35) = min(n, 4)
    float wheel[4][6] = {};  // +0x44 + i*0x18: spin, load, friction, slip xyz
    float speed = 0.0f;      // +0xa4 |v| 3D
    float dt = 0.0f;         // +0xa8 tick accumulator (ctrlAcc)
    float rpm = 0.0f;        // +0xac car+0x1e80 * 9.549296
    float redline = 0.0f;    // +0xb0 engine e+0x1c
    float wheelbase = 0.0f;  // +0xb4 getInfo(0x40), 2.0 without wheels
    float nitro = 0.0f;      // +0xb8 car+0x1f5c / e+0xa4c
    float gear_axis = 0.0f;  // +0xbc bot+0x22a4
    uint8_t c0 = 0;          // +0xc0 planner: no speed-factor field
    float c4 = 0.0f;         // +0xc4 planner output
};

// `native bot` payload (0x4803a0 create, 0x480850 vf14 EVENT_COMMAND). Offsets
// are the bot object's; a spline record is the 0x54-byte one of 0x4122b0.
struct BotData {
    struct SplinePoint {
        Vec3 pos = {}, dir = {};
        // Rows without the optional fields keep the 0x414fe0 defaults
        // (+0x40 1.0, +0x44 1, +0x48 10000, +0x4c 10000, +0x50 0).
        float w = 0.0f, f1 = 1.0f;
        int32_t i1 = 1;
        float f2 = 10000.0f, f3 = 10000.0f;
        int32_t i2 = 0;
    };
    int flag = 0;             // +0x2690 create "%d"
    float skill = 0.5f;       // +0x26a8 = ((clamp01(f) - 0.5) + 1) * 0.5
    std::vector<Runtime::Resource *> controlled;  // +0x2310 + n*0x10
    int active = 0;           // +0x2424 last vehicle ref (0x47c640 getInfo 7 == 5)
    int mode = 0;             // +0x259c
    float begin_race = 0.0f;  // +0x25dc
    uint32_t opponent = 0;    // +0x25e0, resolved target +0x25e8
    uint8_t overtaking = 0;   // +0x25f0 (0x486020 sets, 0x483bd0 clears)
    std::string spline_file;  // command arg, for diagnostics only
    float spline_width = 0.0f;  // command arg of 0x482630
    std::vector<SplinePoint> spline_points;  // +0x2604
    traffic::Path spline;     // +0x2604 ctPath built from the points (0x4122b0)
    int segments = 0;

    // Ctor 0x47ed10 / reset 0x484a40 scalars (notes/19 s8.1). The axis table is
    // bot+0x21dc + v*4, raw -127..127; v1 steer is +0x21e0.
    float axis[0x4c] = {};       // +0x21dc
    float look_dist = 55.0f;     // +0x2554
    float look_speed = 10.0f;    // +0x2558
    float look_base = 20.0f;     // +0x255c
    float grid_speed = 10.0f;    // +0x2578
    float grid_range = 180.0f;   // +0x257c
    float max_lat_accel = 6.0f;  // +0x2580
    float grip_base = 0.4f;      // +0x2584
    float brake_mul = 6.0f;      // +0x2588
    float grip_speed = 0.4f;     // +0x258c
    int drive_type = 0;          // +0x2590
    float half_width = 1.4f;     // +0x2594
    float steer_rate = 510.0f;   // +0x2598
    int engine_running = 0;      // +0x25d4
    float max_grip = 0.0f;       // +0x25d8
    float param_floor = -1.0f;   // +0x2608
    float target_d = 0.0f;       // +0x268c (0x485e10: d - look)
    float wheel_grip[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // +0x2710 + i*4
    float curvature_enable = -1.0f;  // +0x2724
    // Card 178 controls state (0x482910 -> 0x482da0/0x483220/0x4830a0).
    float roll_time = 0.0f;       // +0x25c8 rolled-for time
    int32_t reverse_state = 0;    // +0x25bc 1..3 stuck, 4..6/7 reverse out
    int32_t shift_wait = 0;       // +0x25c1 requested a gear pulse
    float last_move = -1.0f;      // +0x25c4 world time of the last gate pass
    int32_t reverse_req = 0;      // +0x25cc reverse-out requested
    float last_reverse = -1.0f;   // +0x25d0 world time of the last reverse-out
    int32_t launch = 0;           // +0x26a4 0x4830a0 launch after AI_spline
    float dec_prev = 0.0f;        // +0x25f8 previous speed target
    float dec_cur = 0.0f;         // +0x25fc deceleration estimate
    float dec_dt = 0.1f;          // +0x25f4 dt of the estimate (reset 0.1)
    float prev_stc = 0.0f;        // +0x2600 previous clamped steer
    Runtime::Resource *self = nullptr;  // bot instance (+0xc parent, controlled list)
    // 0x486020 last plan (card 176): projection d/lat, lateral offset, target
    // point T and the spline direction D there (card 177 input).
    float plan_d = 0.0f;
    float plan_look = 0.0f;
    float plan_lat = 0.0f;
    float plan_off = 0.0f;
    float plan_T[3] = {};
    float plan_D[3] = {};
    // Card 177: step-5 speed limit, reach, the 0x483bd0 bottleneck vector
    // (+0x269c/+0x26a0) and the steer/speed pair handed to 0x482910 (card 178
    // writes the axes). +0x25d8 is max_grip above.
    float plan_vmax = 0.0f;
    float plan_reach = 0.0f;
    float plan_steer = 0.0f;
    float plan_speed = 0.0f;
    float plan_aim[2] = {};
    BotSnapshot snap;
    bool snap_valid = false;
    int control_ticks = 0;   // control_cars bot-branch counter (vmboot)
};

// Lazy per-resource payloads, created on first native access.
PartData *part_data(Runtime &rt, Runtime::Resource *r);
CarData *car_data(Runtime &rt, Runtime::Resource *r);
// The `car` cfg on the record/super-record chain (same walk as car_data).
bool load_car_cfg(Runtime &rt, Runtime::Resource *r, CfgFile *cfg,
                  const Package **pkg = nullptr);
// The `part` cfg of an instance: its own chain, else the type record kept by
// create_car_bodies (0x4c1f90; the instance chain does not reach the type).
bool load_part_cfg(Runtime &rt, Runtime::Resource *r, CfgFile *cfg,
                   const Package **pkg = nullptr);
// 0x4c1f90 (Part__initInstance) + 0x4992e0 (Vehicle__createCar) body parts:
// create the car's "part_comp" and "car_root" bodies in the process world,
// asleep. No-op unless the type chain's car cfg has a `body` line
// (0x4bf4e0 sets M+0xff0; -1 = no body line).
void create_car_bodies(Runtime &rt, Runtime::Resource *inst,
                       Runtime::Resource *type, const std::string &params);
// 0x4a99b0 type_car vf7 (reached from GameRef.setMatrix 0x4db950 -> 0x4eb3f0):
// place the car so its tyres touch `pos` with orientation `ypr` (yaw, pitch,
// roll), zero its velocities and put it to sleep (0x506d90). Returns false when
// the resource is not a car payload (g_cars lookup).
bool car_set_matrix(Runtime &rt, Runtime::Resource *car, const float pos[3],
                    const float ypr[3]);
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
// 0x480850 type_bot__vf14 EVENT_COMMAND: controllable/control/AI_suspend/
// AI_BeginRace/AI_level/AI_spline. Returns 1 for every code on a native bot.
int bot_command(Runtime &rt, Runtime::Resource *bot, uint32_t code,
                const Value &param);
// `native bot` payload of a created bot resource, or null (vmboot).
const BotData *bot_data(Runtime::Resource *bot);
// 0x47be30 Game__runDriveCallbacks: Vehicle__update 0x49c8e0 for every awake
// car, once per physics sub-step.
void drive_cars(float dt);
// 0x47bc20 Game__runControlCallbacks -> 0x498300 Vehicle__updateControls for
// every car, once per physics tick before the sub-steps.
void control_cars(float dt);
// 0x47f980 native bot vf11 on the same mode-8 control list: per physics tick,
// build the controlled car's snapshot S and run the mode switch 0x484b90.
void control_bots(float dt);
// Number of CarData payloads that own car bodies (vmboot body_count check).
int car_body_instances();
// Every live CarData payload (vmboot: shipped C_drag corpus, card 161).
std::vector<CarData *> car_payloads();
void bind_wheel(Object *wrapper, WheelData *w);
WheelData *wheel_data(Object *wrapper);
// SfxTable native handle payload, bound by Chassis.getSfxTable 0x4b3890 to one
// of the car's three engine_sfx tables.
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
