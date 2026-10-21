---
id: 047
status: implemented
branch: card/047
depends: [046]
---
# vehicle: refreshParameters scalar fields (engine/gearbox/nitro/drag) into CarData

## Goal
Finish the script-field part of `Vehicle__refreshParameters` (0x4aebf0) in
`ch_force_update` (card 046): read the engine, gearbox, nitro and drag fields of
the car script after `updatevariables()` and store them in new `CarData` fields
with their exe offsets. No consumer reads them yet (the `src/vehicle` simulation is
not connected to `CarData`); this keeps the data in the exe's shape.

## Exe evidence (cleaned pseudocode, renamed fields)
`getF` = `Instance__getFloatField 0x4ec980` (missing/non-float → 0, logs); `getI` =
`0x4ece80` (missing → 0). Insert points are relative to card 046's code.

### 0x4aebf0, right after updatevariables(), BEFORE the torque_base reset
    e->inertia (+0xe4)          = getF("engine_inertia");
### after the torque_base/nitro_torque_base = 1.0 lines, BEFORE the health factors
    e->starter_torque (+0xd0)   = getF("starter_torque");
    e->friction_fwd (+0xf4)     = getF("engine_friction_fwd");
    e->friction_rev (+0xf8)     = getF("engine_friction_rev");
    r = getF("engine_rpm_idle");
    e->idle_omega (+0xf0)       = (r*0.016666668f)*3.1415927f + (r*0.016666668f)*3.1415927f;  // keep this float order
    m = getF("engine_mass");
    old = car->heat_capacity;                       // car+0x1ebc
    car->heat_capacity          = m * 8000.0f;
    car->cooling_coeff (+0x1ec8) = (old * car->cooling_coeff) / (m * 8000.0f);
### after refresh_engine_curve
    ratio = getObjectField(script, "ratio", "[F");   // float array
    n = min(arrayLength(ratio), 8);
    for i < n: e->ratio[i] (+0x24 + 4i) = ratio[i];
    e->gears (+0x20)            = getI("gears");
    e->rearend_ratio (+0x44)    = getF("rearend_ratio");
    e->max_rpm (+0x18)          = getF("maxRPM");
    e->rpm_limit (+0x1c)        = getF("RPM_limit");
    e->clutch_f (+0xcc)         = getF("ClutchF");
    (SFX_trans_fwd / SFX_trans_rev / SFX_ignition: int ids, resource refs via 0x51fa40 — not modelled)
    e->rpm_trans_fwd (+0x808)   = getF("rpm_trans_fwd");
    e->rpm_trans_rev (+0x80c)   = getF("rpm_trans_rev");
    e->sfx_starter_rpm (+0x810) = getF("sfx_starter_rpm");
    car->horn_id (+0x1e00)      = 0;
    e->tank_nitro (+0xa4c)      = getF("tank_nitro");
    e->consumption_nitro (+0xa58) = getF("consumption_nitro");
    car->nitro (+0x1f5c)        = (e->tank_nitro <= car->nitro) ? e->tank_nitro : car->nitro;
    (wheel slot rebinding loop over slots 0x65+i — not modelled)
    dc = getObjectField(script, "drag_center", "java.lang.Vector3");
    e->drag_center (+0x910..0x918) = getF(dc,"x"), getF(dc,"y"), getF(dc,"z");   // null dc -> 0s
    e->c_drag (+0x91c)          = getF("C_drag");

## Changes
1. `src/game/car.h`, `struct CarData`, "Engine block e" group — new fields:
   ```cpp
   float max_rpm = 6500.0f;         // e+0x18   INVENTED default (template not traced)
   float rpm_limit = 6800.0f;       // e+0x1c   INVENTED default
   int32_t gears = 0;               // e+0x20
   float ratio[8] = {};             // e+0x24..0x40
   float rearend_ratio = 0.0f;      // e+0x44
   float clutch_f = 500.0f;         // e+0xcc  template default (notes/09)
   float starter_torque = 50.0f;    // e+0xd0  template default
   float inertia = 10.0f;           // e+0xe4  template default
   float idle_omega = 83.86f;       // e+0xf0  template default (rad/s)
   float friction_fwd = 1e-4f;      // e+0xf4  template default
   float friction_rev = 0.03f;      // e+0xf8  template default
   float rpm_trans_fwd = 0.0f;      // e+0x808
   float rpm_trans_rev = 0.0f;      // e+0x80c
   float sfx_starter_rpm = 0.0f;    // e+0x810
   float drag_center[3] = {};       // e+0x910
   float c_drag = 0.0f;             // e+0x91c
   float tank_nitro = 0.0f;         // e+0xa4c
   float consumption_nitro = 0.0f;  // e+0xa58
   float heat_capacity = 0.0f;      // car+0x1ebc  INVENTED initial 0
   float cooling_coeff = 0.0f;      // car+0x1ec8  INVENTED initial 0
   ```
   Do not rename or duplicate existing fields (`nitro`, `horn_id`, `torque_base`, ...).
2. `src/script/natives.cpp`, `ch_force_update` (card 046): add the reads at the insert
   points above, using card 009's float-field helper (`grep -n 'dd_get_f' src/script/natives.cpp`
   for its signature; 0x4ec980 semantics) and card 046's `script_int_field`. For `ratio`:
   `vm.get_instance_field(self, "ratio", &v, &e)`; use `v.arr->elems` when `v.kind == Value::Array && v.arr`,
   Float elements as is, Int as `(float)`; anything else 0. For `drag_center`: object field, then the
   float helper on `x`/`y`/`z` when it is a non-null `Value::Obj`, else 0.
   Replace card 046's comment list of not-yet-ported fields with
   `// shortcut: SFX_trans_fwd/rev, SFX_ignition resource refs and wheel slot rebinding not modelled.`

## Files
- edit: src/game/car.h, src/script/natives.cpp

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
