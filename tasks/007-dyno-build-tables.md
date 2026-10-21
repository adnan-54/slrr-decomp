---
id: 007
status: implemented
branch: card/007
depends: [006]
---
# dyno: setupRpmPoint (0x4f4ca0), runCycle (0x4f4560), buildTables (0x4f4640)

## Goal
Port the rpm sweep that turns the cylinder model into the dyno tables. Adds
`EngineModel::setup_rpm_point`, `run_cycle`, `build_tables` to
`src/vehicle/dyno.cpp`; field names from `src/vehicle/dyno.h` (card 005);
`step_crank` exists (card 006). Float math in the given order.

## Exe evidence (cleaned pseudocode, renamed fields)
### 0x4f4ca0 EngineModel__setupRpmPoint
```c
float ff = 1.0f / (afr + 1.0f);
float ra = p_amb * 9.869233e-06f * rho0;
fuel_frac = ff; rho_amb = ra; air_frac = ff * afr;
rho_exh = p_exh * 9.869233e-06f * rho0;
// spark
float d = spark_rpm1 - spark_rpm0, f;
if (d <= 0.0f || (f = (rpm - spark_rpm0) / d) < 0.0f) spark = spark_min;
else spark = (f <= 1.0f ? spark_inc * f : spark_inc) + spark_min;
// intake VVT (unused by calcDyno, but port it)
d = in_vvt_rpm1 - in_vvt_rpm0;
if (d <= 0.0f || (f = (rpm - in_vvt_rpm0) / d) < 0.0f) in_open = in_open_base;
else in_open = (f <= 1.0f ? in_vvt_adv * f : in_vvt_adv) + in_open_base;
in_close = in_duration + in_open;
// exhaust VVT
d = out_vvt_rpm1 - out_vvt_rpm0;
if (d <= 0.0f || (f = (rpm - out_vvt_rpm0) / d) < 0.0f) out_open = out_open_base;
else out_open = (f <= 1.0f ? out_vvt_adv * f : out_vvt_adv) + out_open_base;
p_in = p_amb;
out_close = out_duration + out_open;
h_in = mixture_h; t_in = t_intake; rho_in = ra;
// turbo
if (0.0f < turbo_mul && 0.0f < turbo_range) {
    float g, x = fabsf(rpm * turbo_mul - turbo_opt);
    if (turbo_band <= x) { g = 1.0f - (x - turbo_band) / (turbo_range - turbo_band); if (g <= 0.0f) g = 0.0f; }
    else g = 1.0f;
    float bst = p_turbo_max * g;
    if (p_turbo_waste <= bst) bst = p_turbo_waste;
    float s = sqrtf(bst + 1.0f);
    t_in = t_intake * s; rho_in = ra * s; p_in = s * s * p_amb;
}
// consumption limits use the previous run_cycle's rates
if (0.0f < max_fuel && max_fuel < fuel_rate) h_in = mixture_h / (fuel_rate / max_fuel);
if (0.0f < max_air  && max_air  < air_rate)  p_in = p_in / (air_rate / max_air);
// burn duration
float k = rpm * burn_rpm_factor; if (0.9f <= k) k = 0.9f;
burn_time = (1.0f - k) * time_burn;
```
Note: the air limit divides `p_in` (+0x158), not `rho_in` (notes/10 said ρ; the
decompile writes +0x158). Port +0x158 = `p_in`.

### 0x4f4560 EngineModel__runCycle
```c
fuel = 0; air = 0; q_loss = 0; unused11c = 0; cycles = 0;
do { step_crank(1); } while (cycles == 0);
float inv = 1.0f / tc;
float fr = fuel * inv * cylinders;
air_rate = air * inv * cylinders;
fuel_rate = fr;
bsfc = (fr * 7920.0f) / hp;
ve = (air + fuel) / (v_disp * rho0);
q_loss = inv * q_loss;
```

### 0x4f4640 EngineModel__buildTables(int step)
```c
max_p_ind = 0; max_p_brake = 0; max_imep = 0; max_torque = 0; max_hp = 0;
rpm_max_p_ind = 0; rpm_max_p_brake = 0; rpm_max_imep = 0; rpm_max_torque = 0;
count = (int)max_rpm / step;          // float->int truncation, then int division
rpm_max_hp = 0;
this->step = step;
// (re)allocate table set k = table_set: free old, new float[count]
torque_table[table_set].assign(count, 0.0f);   // exe: Mem__alloc(count*4), uninitialised
power_table[table_set].assign(count, 0.0f);
torque_table[table_set][0] = 0; power_table[table_set][0] = 0;
int rpm_i = step;
for (int i = 1; i < count; i++, rpm_i += step) {   // exe: do/while guarded by 1 < count
    float fr = (float)rpm_i;
    theta = 0;
    rpm = fr;
    tc = 120.0f / fr;
    omega = fr * 0.10471976f;
    t = (120.0f / fr) * 0.0f;
    piston_speed = fr * 0.10471976f * crank_r;
    setup_rpm_point();
    dt = tc * 0.0043290043f;            // 1/231
    reset_cycle();
    run_cycle();
    float ref = imep;
    int left = 10;
    do {
        run_cycle();
        if (fabsf(ref / imep - 1.0f) < 0.05f) break;
        left--;
        ref = ref * 0.4f + imep * 0.6f;
    } while (0 < left);
    if (max_p_ind < p_ind)    { max_p_ind = p_ind; rpm_max_p_ind = rpm; }
    if (max_p_brake < p_brake){ max_p_brake = p_brake; rpm_max_p_brake = rpm; rpm_max_hp = rpm;
                                max_hp = p_brake * 0.001341f; }
    if (max_imep < imep)      { max_imep = imep; rpm_max_imep = rpm; rpm_max_torque = rpm;
                                max_torque = torque; }
    torque_table[table_set][i] = torque;
    power_table[table_set][i] = p_brake;   // watts
}
torque = max_torque;
p_ind = max_p_ind;
p_brake = max_p_brake;
```
Edge cases:
- `step == 0` is an integer-divide fault in the exe (reachable from the nitro pass).
  `// INVENTED: step <= 0 -> count = 0, tables cleared, return.`
- `count <= 0`: exe would `Mem__alloc(0)` and write index 0 out of bounds;
  `// INVENTED: count < 1 -> empty tables, no element 0 write.`
- `count` stays the int computed above even if negative.

## Files
- edit: `src/vehicle/dyno.cpp` — the three functions, each with `// 0xADDR Name`.
- read only: `src/vehicle/dyno.h`

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
