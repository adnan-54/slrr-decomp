// DynoData engine model: native payload of java.game.parts.DynoData
// (0x1ec bytes in the exe). Layout, constants and formulas are documented in
// notes/10-engine-dyno.md; offsets below are the exe byte offsets.
#pragma once
#include <stdint.h>
#include <vector>

// INVENTED: exe memory from Mem__alloc is uninitialised; zeroed here.
// java.game.parts.DynoData native payload (0x1ec bytes, notes/10-engine-dyno.md).
struct EngineModel {
    float crank_r = 0.0f;          // +0x00 stroke/2
    float bore_r = 0.0f;           // +0x04 bore/2
    float unused08 = 0.0f;         // +0x08 1.029, never read
    float conrod = 0.0f;           // +0x0c L = 0.15
    float v_min = 0.0f;            // +0x10 clearance volume
    float cylinders = 0.0f;        // +0x14 float (script field is float)
    float unused18 = 0.0f;         // +0x18 1.0, never read
    float time_burn = 0.0f;        // +0x1c
    float burn_rpm_factor = 0.0f;  // +0x20 5e-5
    float rho0 = 0.0f;             // +0x24 1.2929
    float cv = 0.0f;               // +0x28 712
    float p_amb = 0.0f;            // +0x2c 101325
    float p_exh = 0.0f;            // +0x30 111457.5
    float t_intake = 0.0f;         // +0x34 293.15
    float t_exh = 0.0f;            // +0x38 293.15
    float mixture_h = 0.0f;        // +0x3c 1e6
    float h_exh = 0.0f;            // +0x40 0
    float in_min = 0.0f, out_min = 0.0f, in_max = 0.0f, out_max = 0.0f;  // +0x44 +0x48 +0x4c +0x50
    float t_loss = 0.0f;           // +0x54 95
    float rpm_limit = 0.0f;        // +0x58 stored only
    float bore = 0.0f, stroke = 0.0f;     // +0x5c +0x60
    float displacement = 0.0f;     // +0x64 cyl*Vd
    float area = 0.0f;             // +0x68 pi R^2
    float v_disp = 0.0f;           // +0x6c per cylinder
    float v_tot = 0.0f;            // +0x70
    float compression = 0.0f;      // +0x74
    float piston_speed_max = 0.0f; // +0x78 45
    float rho_amb = 0.0f, rho_exh = 0.0f; // +0x7c +0x80
    float dt = 0.0f;               // +0x84
    float rpm = 0.0f;              // +0x88
    float max_rpm = 0.0f;          // +0x8c
    float spark = 0.0f;            // +0x90 effective spark time (fraction of cycle)
    float in_open = 0.0f, in_close = 0.0f, out_open = 0.0f, out_close = 0.0f;           // +0x94..+0xa0
    float in_open_base = 0.0f, in_duration = 0.0f, in_vvt_adv = 0.0f, in_vvt_rpm0 = 0.0f, in_vvt_rpm1 = 0.0f;      // +0xa4..+0xb4
    float out_open_base = 0.0f, out_duration = 0.0f, out_vvt_adv = 0.0f, out_vvt_rpm0 = 0.0f, out_vvt_rpm1 = 0.0f; // +0xb8..+0xc8
    float spark_min = 0.0f, spark_inc = 0.0f, spark_rpm0 = 0.0f, spark_rpm1 = 0.0f;     // +0xcc..+0xd8
    float turbo_mul = 0.0f, turbo_opt = 0.0f, turbo_band = 0.0f, turbo_range = 0.0f;    // +0xdc..+0xe8
    float p_turbo_max = 0.0f, p_turbo_waste = 0.0f;                       // +0xec +0xf0
    float afr = 0.0f;              // +0xf4 mixture ratio
    float max_fuel = 0.0f, max_air = 0.0f;// +0xf8 +0xfc kg/s
    float a_in = 0.0f, a_out = 0.0f;      // +0x100 +0x104 current valve coefficients
    float burn = 0.0f;             // +0x108 burn fraction b
    float spark_time = 0.0f;       // +0x10c seconds
    float t_wall = 0.0f, t_wall2 = 0.0f;  // +0x110 +0x114 353.15
    float q_loss = 0.0f;           // +0x118
    float unused11c = 0.0f;        // +0x11c
    float t = 0.0f, tc = 0.0f, omega = 0.0f, theta = 0.0f, x = 0.0f, v = 0.0f,
          rho = 0.0f, m = 0.0f, p = 0.0f, temp = 0.0f, h = 0.0f;    // +0x120..+0x148
    float air_frac = 0.0f, fuel_frac = 0.0f;                              // +0x14c +0x150
    float h_in = 0.0f, p_in = 0.0f, t_in = 0.0f, rho_in = 0.0f;           // +0x154..+0x160 manifold
    float burn_time = 0.0f;        // +0x164 seconds
    float fuel = 0.0f, air = 0.0f; // +0x168 +0x16c per cycle
    float piston_speed = 0.0f;     // +0x170
    int32_t cycles = 0;            // +0x174 (int)
    float work = 0.0f, work_last = 0.0f;  // +0x178 +0x17c
    float p_fric = 0.0f, imep = 0.0f, torque = 0.0f, hp = 0.0f, p_ind = 0.0f, p_brake = 0.0f;         // +0x180..+0x194
    float fuel_rate = 0.0f, air_rate = 0.0f, bsfc = 0.0f, ve = 0.0f;      // +0x198..+0x1a4
    float max_imep = 0.0f, max_torque = 0.0f, max_hp = 0.0f, max_p_ind = 0.0f, max_p_brake = 0.0f;                       // +0x1a8..+0x1b8
    float rpm_max_imep = 0.0f, rpm_max_torque = 0.0f, rpm_max_hp = 0.0f, rpm_max_p_ind = 0.0f, rpm_max_p_brake = 0.0f;   // +0x1bc..+0x1cc
    int32_t step = 0;              // +0x1d0
    int32_t count = 0;             // +0x1d4
    int32_t table_set = 0;         // +0x1d8 0 normal, 1 nitro
    std::vector<float> torque_table[2];  // +0x1dc / +0x1e0
    std::vector<float> power_table[2];   // +0x1e4 / +0x1e8 (watts)

    EngineModel();               // 0x4f44a0
    void free_tables();          // 0x4f44e0
    void set_defaults();         // 0x4f49e0
    void update_mixture();       // 0x4f5980
    void update_geometry();      // 0x4f59f0
    void reset_cycle();          // 0x4f4fc0
    void step_crank(int n);      // 0x4f50e0  (card 006)
    void setup_rpm_point();      // 0x4f4ca0  (card 007)
    void run_cycle();            // 0x4f4560  (card 007)
    void build_tables(int step); // 0x4f4640  (card 007)
};
