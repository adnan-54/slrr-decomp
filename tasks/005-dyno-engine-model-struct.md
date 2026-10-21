---
id: 005
status: merged
branch: card/005
depends: []
---
# dyno: EngineModel struct, ctor/freeTables/setDefaults/updateMixture/updateGeometry/resetCycle

## Goal
Create the native payload of `java.game.parts.DynoData` (0x1ec bytes in the exe,
`notes/10-engine-dyno.md`) as `struct EngineModel` in a new `src/vehicle/dyno.{h,cpp}`
and port the six small setup functions. Later cards add `stepCrank` (006),
`setupRpmPoint/runCycle/buildTables` (007) and the natives (008/009).
Layout is not binary-matched: keep every field as a named `float` (or `int32_t`
where noted) with its exe offset in a comment; the four table pointers become
`std::vector<float>`.

## Struct (put exactly this in `src/vehicle/dyno.h`; default member values = 0)
```cpp
// java.game.parts.DynoData native payload (0x1ec bytes, notes/10-engine-dyno.md).
struct EngineModel {
    float crank_r;          // +0x00 stroke/2
    float bore_r;           // +0x04 bore/2
    float unused08;         // +0x08 1.029, never read
    float conrod;           // +0x0c L = 0.15
    float v_min;            // +0x10 clearance volume
    float cylinders;        // +0x14 float (script field is float)
    float unused18;         // +0x18 1.0, never read
    float time_burn;        // +0x1c
    float burn_rpm_factor;  // +0x20 5e-5
    float rho0;             // +0x24 1.2929
    float cv;               // +0x28 712
    float p_amb;            // +0x2c 101325
    float p_exh;            // +0x30 111457.5
    float t_intake;         // +0x34 293.15
    float t_exh;            // +0x38 293.15
    float mixture_h;        // +0x3c 1e6
    float h_exh;            // +0x40 0
    float in_min, out_min, in_max, out_max;  // +0x44 +0x48 +0x4c +0x50
    float t_loss;           // +0x54 95
    float rpm_limit;        // +0x58 stored only
    float bore, stroke;     // +0x5c +0x60
    float displacement;     // +0x64 cyl*Vd
    float area;             // +0x68 pi R^2
    float v_disp;           // +0x6c per cylinder
    float v_tot;            // +0x70
    float compression;      // +0x74
    float piston_speed_max; // +0x78 45
    float rho_amb, rho_exh; // +0x7c +0x80
    float dt;               // +0x84
    float rpm;              // +0x88
    float max_rpm;          // +0x8c
    float spark;            // +0x90 effective spark time (fraction of cycle)
    float in_open, in_close, out_open, out_close;           // +0x94..+0xa0
    float in_open_base, in_duration, in_vvt_adv, in_vvt_rpm0, in_vvt_rpm1;      // +0xa4..+0xb4
    float out_open_base, out_duration, out_vvt_adv, out_vvt_rpm0, out_vvt_rpm1; // +0xb8..+0xc8
    float spark_min, spark_inc, spark_rpm0, spark_rpm1;     // +0xcc..+0xd8
    float turbo_mul, turbo_opt, turbo_band, turbo_range;    // +0xdc..+0xe8
    float p_turbo_max, p_turbo_waste;                       // +0xec +0xf0
    float afr;              // +0xf4 mixture ratio
    float max_fuel, max_air;// +0xf8 +0xfc kg/s
    float a_in, a_out;      // +0x100 +0x104 current valve coefficients
    float burn;             // +0x108 burn fraction b
    float spark_time;       // +0x10c seconds
    float t_wall, t_wall2;  // +0x110 +0x114 353.15
    float q_loss;           // +0x118
    float unused11c;        // +0x11c
    float t, tc, omega, theta, x, v, rho, m, p, temp, h;    // +0x120..+0x148
    float air_frac, fuel_frac;                              // +0x14c +0x150
    float h_in, p_in, t_in, rho_in;                         // +0x154..+0x160 manifold
    float burn_time;        // +0x164 seconds
    float fuel, air;        // +0x168 +0x16c per cycle
    float piston_speed;     // +0x170
    int32_t cycles;         // +0x174 (int)
    float work, work_last;  // +0x178 +0x17c
    float p_fric, imep, torque, hp, p_ind, p_brake;         // +0x180..+0x194
    float fuel_rate, air_rate, bsfc, ve;                    // +0x198..+0x1a4
    float max_imep, max_torque, max_hp, max_p_ind, max_p_brake;                       // +0x1a8..+0x1b8
    float rpm_max_imep, rpm_max_torque, rpm_max_hp, rpm_max_p_ind, rpm_max_p_brake;   // +0x1bc..+0x1cc
    int32_t step;           // +0x1d0
    int32_t count;          // +0x1d4
    int32_t table_set;      // +0x1d8 0 normal, 1 nitro
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
```
Give every scalar a `= 0` initialiser (one `= 0.0f` per declarator, or value-initialise
in the ctor). `// INVENTED: exe memory from Mem__alloc is uninitialised; zeroed here.`

## Exe evidence (cleaned pseudocode, renamed fields)
### 0x4f44a0 EngineModel ctor
`t = 0; torque_table[0..1], power_table[0..1] = null; table_set = 0;` → clear the vectors.
### 0x4f44e0 freeTables
frees `torque_table[k]` and `power_table[k]` for k = 0,1 → `clear()` + `shrink_to_fit()` all four.

### 0x4f49e0 setDefaults (order matters: reset_cycle is called in the middle)
```c
rho0 = 1.2929f; p_amb = 101325.0f; p_exh = 111457.5f; afr = 14.0f;
crank_r = 0.053175f; bore_r = 0.049f; conrod = 0.15f; v_min = 1e-4f; cylinders = 4.0f;
time_burn = 0.003f; burn_rpm_factor = 5e-5f; unused08 = 1.029f; unused18 = 1.0f;
mixture_h = 1e6f; h_exh = 0; cv = 712.0f; t_intake = 293.15f; t_exh = 293.15f;
in_min = 0.005f; out_min = 0.005f; in_max = 0.8f; out_max = 0.8f; t_loss = 95.0f;
t_wall = 353.15f; t_wall2 = 353.15f;
spark = 0.48f; in_open = 0.001f; in_close = 0.25f; out_open = 0.75f; out_close = 0.99f;
spark_min = 0.48f; spark_inc = -0.05f; spark_rpm0 = 2000.0f; spark_rpm1 = 5000.0f;
in_open_base = 0.001f; in_duration = 0.249f; in_vvt_adv = 0; in_vvt_rpm0 = 0; in_vvt_rpm1 = 0;
out_open_base = 0.75f; out_duration = 0.24f; out_vvt_adv = 0; out_vvt_rpm0 = 0; out_vvt_rpm1 = 0;
t = 0; theta = 0;
rpm = 1000.0f; turbo_mul = 20.0f; turbo_opt = 85000.0f; turbo_range = 30000.0f;
turbo_band = 0; p_turbo_max = 0; p_turbo_waste = 3.0f;
fuel_frac = 1.0f / (afr + 1.0f);
air_frac  = fuel_frac * afr;
rho_amb = p_amb * 9.869233e-06f * rho0;
rho_exh = p_exh * 9.869233e-06f * rho0;
reset_cycle();                                  // 0x4f4fc0
max_p_ind = 0; max_p_brake = 0; max_imep = 0;
dt = tc * 0.005f;
rpm_max_p_ind = 0; rpm_max_p_brake = 0; rpm_max_imep = 0;
max_air = 100.0f; max_fuel = 100.0f;
fuel = 0; air = 0;
```
(Decoded from the hex constants: 0x3d59ce07 = 0.053175, 0x3d48b439 = 0.049,
0x3e7ef9db = 0.249, 0x3e75c290 = 0.24, 0x3f83b646 = 1.029.)
Note: `fuel_rate/air_rate` are **not** reset here (the first rpm point of the next
`buildTables` sees the previous run's rates; keep that).

### 0x4f5980 updateMixture
```c
fuel_frac = 1.0f / (afr + 1.0f);
air_frac  = fuel_frac * afr;
rho_amb = p_amb * 9.869233e-06f * rho0;
rho_exh = p_exh * 9.869233e-06f * rho0;
```
### 0x4f59f0 updateGeometry
```c
float R = bore_r, r = crank_r;
h_in = mixture_h; p_in = p_amb;
float A = R * R * 3.1415927f;
bore = R + R; stroke = r + r;
float vd = (r + r) * A;
t_in = t_intake; area = A; v_disp = vd;
piston_speed_max = 45.0f;
rho_in = rho_amb;
v_tot = vd + v_min;
compression = (vd + v_min) / v_min;
max_rpm = (45.0f / r) * 9.549296f;
piston_speed = omega * r;
displacement = cylinders * vd;
```
### 0x4f4fc0 resetCycle
```c
float r = crank_r, L = conrod;
h = h_in; p = p_in;
t = 0; theta = 0; a_in = 0; a_out = 0; burn = 0; q_loss = 0; unused11c = 0;
work = 0; work_last = 0; imep = 0; p_ind = 0;
temp = t_in;
fuel = 0; air = 0;
float xx = sqrtf(L * L - r * 0.0f * r * 0.0f) + r;   // sin(0) = 0: piston at TDC
rho = rho_in;
x = xx;
v = ((xx - L) - r) * area + v_min;                  // = v_min
m = v * rho_in;
```

## Files
- create: `src/vehicle/dyno.h` (struct above; `#pragma once`, `<stdint.h>`, `<vector>`;
  header comment citing notes/10).
- create: `src/vehicle/dyno.cpp` — the six functions above, each with `// 0xADDR Name`.
  Leave `step_crank`, `setup_rpm_point`, `run_cycle`, `build_tables` **undefined**
  (cards 006/007 add them; nothing calls them yet so it links).
- edit: `src/Makefile` — add `vehicle/dyno.cpp` to `LIB_SRC` after `vehicle/vehicle.cpp`.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
