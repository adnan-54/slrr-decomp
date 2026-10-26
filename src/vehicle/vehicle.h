// Vehicle simulation: semantic port of the car/wheel/engine structures and
// the per-tick Vehicle::update (0x49c8e0, notes/09-vehicle.md). Engine curve
// semantics (dyno table interpolation, friction torque) follow notes/10.
// Generic names: this is the game-specific layer, not the engine core.
//
// Axes in chassis-local space: x = lateral, y = up, z = forward (notes/09).
#pragma once
#include <vector>

#include "physics/physics.h"

static const int kPacejkaSlots = 17;
static const int kMaxGears = 8;

// 0x4b0ff0 Vehicle__healthFactor: 1 - w*w*0.6, floored at 0.100000024 once
// w >= 1. Shared by the car payload setters (car.cpp) and §1 wear.
float health_factor(float w);

// 0x4afb50 Vehicle__damageRoll: wear += rate * amount, clamped to [0, 1].
int wear_add(float *wear, float rate, float amount);

// 17 magic-formula coefficients, exe layout W+0x1e8 + 4*i.
// p2 is overwritten per tick with ground friction * tyre friction and p13 with
// ground rolling resistance + wheel rolling resistance (notes/09 §6).
struct Pacejka {
    float p[kPacejkaSlots] = {1.4f, 0.0f, 1.49f, 0.0f, 15.2f, 0.0f, -0.0f, 0.0f,
                              -1.0f, 0.0f, 0.0f, 8000.0f, 1.0f, 0.015f, 0.4f, 0.0f, 0.0f};
};

// One magic-formula evaluation (notes/09 §6, the pure part).
struct TyreSample {
    float sigma = 0.0f;   // combined slip, after the p12 longitudinal scale
    float load = 0.0f;    // Fz after the p14/p11 load smoothing
    float force = 0.0f;   // combined force magnitude F
    float f_lat = 0.0f;   // F*sin(alpha)/sigma
    float f_long = 0.0f;  // F*cos(alpha)*p12*kappa/sigma
    float mz = 0.0f;      // aligning torque; stored only (no consumer in exe)
};

// normal_load is the suspension load F_N; mu is the per-tick p2
// (ground friction * tyre friction).
TyreSample tyre_magic(const Pacejka &pj, float normal_load, float camber, float kappa,
                      float alpha, float mu);

// One wheel record (W, stride 0x2e4). Fields not modelled: wear/damage, heat,
// skid contexts, render link, arm/hub/opposite-wheel geometry, instant centres.
struct Wheel {
    // wheel record
    float spin_inertia = 50.0f;        // e+0xec base wheel inertia
    float radius = 0.32f;              // W+0x60
    float friction = 1.0f;             // W+0x64 base tyre friction
    float friction_eff = 1.0f;         // W+0x6c effective (wear factor applied)
    float rolling_resistance = 0.015f; // W+0x74
    Pacejka pacejka;                   // W+0x1e8
    Vec3 mount{0.0f, 0.0f, 0.0f};      // W+0x78 hub/mount, chassis local

    // suspension (W+0x50/54/58; the spring lengths include the wheel radius)
    float rest_len = 0.10f;
    float max_len = 0.25f;   // ray limit = max_len + radius
    float min_len = -1.0f;   // bump stop = min_len + radius (<0 disables)
    float spring = 30000.0f; // W+0x44
    float bump = 1500.0f;    // W+0x48
    float rebound = 1050.0f; // W+0x4c (0.7 * bump by default)
    float patch_halfwidth = 0.0f; // S+0x9c = |halfwidth| (setCPatch)
    float patch_angle = 0.0f;     // S+0xa0 = |angle|, also the n.x divisor
    float patch_offset = 0.0f;    // S+0xa4

    // drivetrain (W+0xcc base / W+0xe0 effective share / W+0xc4 ratio sign)
    float drive = 0.0f;
    float drive_share = 0.0f;
    float drive_ratio = -1.0f; // shaft->wheel: -1/ratio is positive forward

    // brakes (W+0xd0/d4 base, W+0xd8/dc effective, W+0xfc extra, W+0xf0 bearing)
    float brake = 0.0f;
    float handbrake = 0.0f;
    float brake_eff = 0.0f;
    float handbrake_eff = 0.0f;
    float extra_brake = 0.0f;
    float bearing = 0.0f;

    // brake thermal + wear state (0x4992e0 create values: W+0x184 cap,
    // W+0x188 temp, W+0x18c ambient, W+0x190 cooling; wear/rate W+0x164/68).
    float brake_wear = 0.0f;
    float brake_rate = 0.0f;
    float brake_temp = 20.0f;
    float brake_ambient = 20.0f;
    float brake_heat_cap = 50000.0f;
    float brake_cool = 0.001f;

    // steering (W+0xc8) and wheel frame (W+0x90): the sensor-body local matrix
    // is base_rot * Ry(steer_angle), written by 0x4aa830/0x566a70.
    float steer_factor = 0.0f;
    Mat34 base_rot{{1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                    0.0f, 0.0f, 1.0f, 0.0f}};

    // per-tick state
    float spin = 0.0f;            // W+0xc0 rad/s, +forward
    float spin_angle = 0.0f;      // W+0xe8
    float inv_inertia_eff = 0.0f; // W+0xec = 0.1 / effective inertia
    float kappa_prev = 0.0f;      // W+0x14c
    float alpha_prev = 0.0f;      // W+0x150
    float sigma = 0.0f;           // W+0x154
    float mz = 0.0f;              // W+0x158
    Vec3 slip_velocity{0.0f, 0.0f, 0.0f}; // W+0x140 (wheel frame)
    float steer_angle = 0.0f;     // W+0xe4 (0x4aa830, Ackermann)

    // suspension object (S) results for this tick
    bool contact = false;
    float sensor_len = 0.0f;      // S capsule length L = W+0x5c, fixed at create
    unsigned gate_rejects = 0;    // 0x4f3d70 crossings dropped by prev - d > r
    float ray_len = 0.0f;         // S+0xf0; also the next tick's gate input
    float load = 0.0f;            // S+0xf4
    float susp_velocity = 0.0f;   // S+0xf8 (+ extends)
    Vec3 contact_local{0.0f, 0.0f, 0.0f}; // S+0xd8
    Vec3 normal_world{0.0f, 1.0f, 0.0f};  // S+0xc4
    Vec3 ground_velocity{0.0f, 0.0f, 0.0f}; // Wheel__contactQuery
    float ground_friction = 0.0f; // S+0xac material
    float ground_rolling_resistance = 0.0f; // gm+0x54 of the same material

    float rest_ray() const { return rest_len + radius; }
    float max_ray() const { return max_len + radius; }
    float bump_ray() const { return min_len + radius; }
    bool driven() const { return drive_share > 0.0f; }

    void set_drive(float v) { drive = drive_share = v; }
    void set_brake(float v) { brake = brake_eff = v; }
    void set_handbrake(float v) { handbrake = handbrake_eff = v; }
    void set_cpatch(float halfwidth, float angle, float offset)
    {
        patch_halfwidth = fabsf(halfwidth);
        patch_angle = fabsf(angle);
        patch_offset = offset;
    }
};

// Engine block (e, 0xc34) plus the car-side engine state (car+0x1e7c...).
struct TorquePoint {
    float rpm = 0.0f;
    float value = 0.0f;
};

struct Engine {
    // dyno tables (notes/10): Nm and brake watts, both stepped by the dyno.
    std::vector<TorquePoint> torque_curve;
    std::vector<TorquePoint> power_curve;
    float max_rpm = 6500.0f;        // e+0x18
    float rpm_limit = 6800.0f;      // e+0x1c
    float idle = 83.86f;            // e+0xf0 rad/s
    float inertia = 10.0f;          // e+0xe4
    float shaft_inertia = 1000.0f;  // e+0xe8 drivetrain (wheel-side) inertia
    float starter_torque = 50.0f;   // e+0xd0
    float friction_fwd = 1e-4f;     // e+0xf4
    float friction_rev = 0.03f;     // e+0xf8
    float torque_health = 1.0f;     // e+0xd4
    float nitro_health = 1.0f;      // e+0xd8
    bool running = true;

    // state
    float omega = 83.86f;          // car+0x1e7c rad/s
    float smoothed_omega = 83.86f; // car+0x1e80 (getRPM reports this * 9.549296)
    bool cranking = false;         // car+0x1dfc

    float torque_at(float rpm) const;
    float power_at(float rpm) const;
    float friction_torque(float omega_rad_s) const;
};

// One car: chassis body in physics::World, N raycast wheels, engine/gearbox.
struct Car {
    RigidBody body;            // chassis (car+0x13ec); shape/mass set by caller
    std::vector<Wheel> wheels; // car+0x1414
    Engine engine;
    RigidBody *ground = nullptr; // ray target (S+0xd0)

    // gearbox (notes/09 §3/e+0x20..)
    int gear = 1; // car+0x1e5c, index into ratios; gear 0 is neutral here
    float ratios[kMaxGears] = {0.0f, 3.5f, 2.1f, 1.4f, 1.0f, 0.8f, 0.65f, 0.55f};
    float final_drive = 3.3f;  // e+0x44
    float clutch = 0.0f;       // input, 1 = open; k = 1 - clutch
    float diff_lock = 0.1f;    // e+0xa38
    float max_steer = 0.7f;    // e+0xa20
    float ackermann = 0.0f;    // e+0xa30

    // config factors (Config__loadGameplaySettings defaults)
    float engine_inertia_factor = 4.0f;
    float wheel_gndfeedback_factor = 2.8f;
    float wheel_brake_factor = 4.0f;
    float brake_torque_factor = 1.0f;     // e+0xdc (1.0 in a cloned block)
    float handbrake_torque_factor = 1.0f; // e+0xe0

    // control inputs (car+0x1de4..+0x1df8)
    float throttle = 0.0f;
    float brake = 0.0f;
    float handbrake = 0.0f;
    float steer = 0.0f;
    float nitro = 0.0f;

    // ABS/ASR flags from the car cfg parser (e+0xa3c/0xa40). Inert: no
    // consumer exists in Vehicle::update (notes/09 "Open questions").
    bool abs = false;
    bool asr = false;
    float abs_strength = 0.0f;
    float asr_strength = 0.0f;

    // per-tick drivetrain state
    float shaft_speed = 0.0f;       // car+0x1e78 (wheel-average speed)
    float clutch_slip = 0.0f;       // car+0x1e88
    float net_torque = 0.0f;        // car+0x1e84
    float combustion_torque = 0.0f; // car+0x1e8c
    float last_rpm = 0.0f;          // car+0x1fb0

    // Engine thermal + damage block (§1 0x49cfd0..0x49d2a5, notes/34 §3).
    float engine_temp = 20.0f;        // car+0x1ec0
    float engine_ambient = 20.0f;     // car+0x1ec4
    float heat_capacity = 320000.0f;  // car+0x1ebc
    float cooling_coeff = 9.375e-05f; // car+0x1ec8 = c / heat_capacity
    float cool_low = 10.0f;           // e+0xa5c
    float cool_air = 50.0f;           // e+0xa60
    float cool_speed = 0.01f;         // e+0xa64
    float engine_dmg = 0.0f;          // car+0x1e9c
    float engine_rate = 5.0e-8f;      // car+0x1ea0
    float radiator_dmg = 0.0f;        // car+0x1eac
    float radiator_rate = 5.0e-6f;    // car+0x1eb0
    float trans_wear = 0.0f;          // car+0x1ea4
    float trans_rate = 5.0e-8f;       // car+0x1ea8
    float clutch_wear = 0.0f;         // car+0x1eb4
    float clutch_rate = 5.0e-8f;      // car+0x1eb8
    float torque_base = 1.0f;         // e+0xd4
    float nitro_base = 1.0f;          // e+0xd8
    float k_dmg = 1.0f;               // Config.internal_damage (0x5dacd4)
    bool wear_off = false;            // car+0x70 & 0x400: skip heating/wear

    // Aero drag (Vehicle::update §7, 0x49fb34): e+0x91c / e+0x910, copied in
    // by the per-tick glue. F = -C_drag*|v|^2*v_hat at drag_center.
    float c_drag = 0.0f;
    Vec3 drag_center{0.0f, 0.0f, 0.0f};
    // Last update_drag record (vmboot checks 2 and 4).
    Vec3 drag_velocity{0.0f, 0.0f, 0.0f}; // v at drag_center
    Vec3 drag_force{0.0f, 0.0f, 0.0f};    // applied F
    float drag_m_eff = 0.0f;
    float drag_dt = 0.0f;
    bool drag_clamped = false;
    float drag_magnitude = 0.0f;    // car+0x2248

    float gear_ratio() const
    {
        return gear > 0 && gear < kMaxGears ? ratios[gear] : 0.0f;
    }
    float total_ratio() const { return final_drive * gear_ratio(); }
    void shift(int g) { gear = g; }

    // Documented split/correction factor: a shaft value (torque, or a speed
    // delta) becomes the per-wheel value delta*(share/D)*(-1/ratio), with
    // D = sum |share|. Returns D. Used by the clutch coupling and tests.
    float split_drive(float shaft_value, float *wheel_value) const;

    // Force pass; the physics step calls this between its own force pass and
    // integrate (notes/09 §1-§6).
    void update(float dt); // exposed for tests; does not advance the world

private:
    void update_suspension(float dt);
    void update_engine_and_drivetrain(float dt);
    void update_tyre_forces(float dt);
    void update_drag(float dt);
    void apply_contact_forces();

    struct PendingForce {
        Vec3 force{0.0f, 0.0f, 0.0f};
        Vec3 point{0.0f, 0.0f, 0.0f};
    };
    std::vector<PendingForce> pending_;
};
