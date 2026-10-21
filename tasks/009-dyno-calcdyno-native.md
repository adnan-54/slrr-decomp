---
id: 009
status: merged
branch: card/009
depends: [007, 008]
---
# natives: DynoData.calcDyno (0x4cba80)

## Goal
Replace the last DynoData stub. `calcDyno(float tablesize)` copies the script
fields into the `EngineModel`, runs the model (cards 005–007) and writes the
results back into the script object. Instance native: `a[0]` = receiver,
`a[1]` = tablesize. Payload lookup: `game::dyno_data(obj)` (card 008).

## Exe evidence (cleaned pseudocode, renamed fields)
Field helpers in the exe:
- `getF(name)` = `Instance__getFloatField 0x4ec980`: missing field, or a field
  whose descriptor is not `F` → logs `vm_get_float_field: ...` and returns **0**.
  Port: `vm.get_instance_field`; Float → value, Int → (float)value (VM may hold a
  float field initialised from an int literal), anything else / missing → 0.0f.
  Do not fail the native.
- `has(name)` = `0x4ecff0(obj, name, NULL)` → field exists.
- `setF(name, v)` = `Instance__setFloatField 0x4ed080`; ignore failures
  (e.g. `table_stepsize` does not exist in 2.3.1 `DynoData.java`).

### 0x4cba80 DynoData.calcDyno(float tablesize) → float
```c
bool temp = false;
EngineModel *e = NULL;
if (find_class("java.lang.Native")) e = dyno_data(self);
if (!e) { temp = true; e = new EngineModel; }          // freed at the end
e->set_defaults();                                     // 0x4f49e0
e->cylinders = getF("cylinders");
e->bore_r    = getF("bore") * 0.5f;
e->crank_r   = getF("stroke") * 0.5f;
e->v_min     = getF("Vmin");
e->in_min  = getF("in_min");   e->in_max  = getF("in_max");
e->out_min = getF("out_min");  e->out_max = getF("out_max");
e->in_open  = getF("time_in_open");  e->in_close  = getF("time_in_close");
e->out_open = getF("time_out_open");
float oc = getF("time_out_close");
e->in_open_base = e->in_open;
e->out_close = oc;
e->in_duration = e->in_close - e->in_open;
e->out_open_base = e->out_open;
e->out_duration = oc - e->out_open;
e->time_burn  = getF("time_burn");
e->rpm_limit  = getF("RPM_limit");
e->spark_min  = getF("time_spark_min");  e->spark_inc  = getF("time_spark_inc");
e->spark_rpm0 = getF("time_spark_RPM0"); e->spark_rpm1 = getF("time_spark_RPM1");
e->turbo_mul  = getF("rpm_turbo_mul");   e->turbo_opt  = getF("rpm_turbo_opt");
e->turbo_range= getF("rpm_turbo_range"); e->turbo_band = 0.0f;
e->p_turbo_max = getF("P_turbo_max");
e->p_turbo_waste = has("P_turbo_waste") ? getF("P_turbo_waste") : e->p_turbo_max;
e->mixture_h = getF("mixture_H");
e->afr       = getF("mixture_ratio");
e->max_air   = getF("max_air_consumption");
e->max_fuel  = getF("max_fuel_consumption");
e->t_intake = 293.15f;
if (has("intercooling")) e->t_intake = e->t_intake * getF("intercooling");
e->update_mixture();                                   // 0x4f5980
e->update_geometry();                                  // 0x4f59f0
float mr = getF("maxRPM");
if (0.0f < mr) e->max_rpm = mr; else setF("maxRPM", e->max_rpm);
float st = e->max_rpm / tablesize;
e->table_set = 0;
if (st <= 1.0f) st = 1.0f;
e->build_tables((int)st);                              // 0x4f4640
if (temp) {
    setF("table_stepsize", st);
    setF("torque", 1.0f);
    fill_array("torquetable", e->torque_table[0]);     // see below
    fill_array("HPtable",     e->power_table[0]);
}
setF("Displacement",  e->displacement);
setF("Compression",   e->compression);
setF("maxTorque",     e->max_torque);
setF("RPM_maxTorque", e->rpm_max_torque);
setF("maxHP",         e->max_hp);
setF("RPM_maxHP",     e->rpm_max_hp);
// nitro pass
e->t_intake = 293.15f;                                 // intercooling dropped
float nh = getF("nitro_H"), nc = getF("nitro_cooling");
if (0.0001f < nh - 1.0f || 0.0001f < nc - 1.0f) {
    e->mixture_h = e->mixture_h * nh;
    e->t_intake  = e->t_intake * nc;
    float keep = e->max_rpm;
    e->update_mixture();
    e->update_geometry();
    e->max_rpm = keep;
    e->table_set = 1;
    e->build_tables((int)(keep / tablesize));          // no >= 1 clamp here
    if (!temp) return 0.0f;                            // persistent: done
    setF("torque2", 1.0f);
    fill_array("torquetable2", e->torque_table[1]);
}
if (temp) delete e;                                    // freeTables + Mem__free
return 0.0f;
```
`fill_array(name, table)`: `arr = Instance__getObjectField(self, name, "[F")`; `n` = the
script array's length; for `i < n`: `arr[i] = max(table[i], 0)` written as
`(v <= 0) ? 0 : v` (`FUN_004ed060`). Port: `vm.get_instance_field(name)`; if it is a
`Value::Array` with `arr`, set `arr->elems[i] = Value::make_float(...)` for
`i < elems.size()`; skip silently when the field is missing (2.3.1 `DynoData.java`
has these arrays commented out).
`// INVENTED: i >= table.size() -> write 0` (the exe reads past the table when the
script array is longer than count).
`(int)(keep / tablesize)` may be 0 → card 007's `build_tables` guard handles it
(the exe faults).
The exe returns 0.0f (`float10 0`); the receiver missing / bad arg → normal native error.

## Files
- edit: `src/script/natives.cpp` — add `dd_calc_dyno` (plus small static helpers
  `dd_get_f`, `dd_has`, `dd_set_f`, `dd_fill_array`) in the
  `// ---- java.game.parts.DynoData ----` section from card 008
  (`grep -n 'java.game.parts.DynoData' src/script/natives.cpp`); flip the
  `calcDyno` registry row to `dd_calc_dyno, false`. Use `std::unique_ptr` for the temp model.
- read only: `src/vehicle/dyno.h`

## Done when
- the native has a `// 0x4cba80 DynoData.calcDyno` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
