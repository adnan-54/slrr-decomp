---
id: 046
status: implemented
branch: card/046
depends: [045, 041, 042, 009]
---
# vehicle: Chassis.forceUpdate (0x4b5890) — refreshParameters core: updatevariables, dyno hookup, engine health

## Goal
`Chassis.forceUpdate` is `Vehicle__refreshParameters` (0x4aebf0). Port its core so
the dyno engine model reaches the car: gate on `suspend_update`, reset wheel wear,
run the script's `updatevariables()` synchronously (that is where `Chassis.java`
runs `dyno.calcDyno(...)`), resolve the engine block's dyno pointer the way
`Vehicle__onPartAttached` (0x4942b0) does, reset the torque multipliers/health and
rebuild the torque curves (card 045). The remaining scalar fields are card 047.

## Exe evidence (cleaned pseudocode, renamed fields)
Receiver `a[0]` = the car's script object (typed+0x50); `c = car_of(a)` (CarData);
`e` = engine block fields in CarData.

### 0x4b5890 Chassis.forceUpdate()
    Vehicle__refreshParameters(manager, car);       // 0x4aebf0, nothing else

### 0x4aebf0 Vehicle__refreshParameters (core, in order)
    car->flags &= ~0x800000;                          // car+0x70
    (clone engine block if it is still the template — not modelled)
    script = typed+0x50; if (!script) return;
    if (int field "suspend_update" of script != 0) goto tuning;   // 0x4ece80: missing -> 0
    for each wheel W: W.wear fields +0x15c/+0x160/+0x164/+0x168/+0x16c/+0x170 = 0;
                      W.pacejka (+0x1e8, 17 floats) = manager template M+0x2ec4;
    call script.updatevariables();                    // 0x47dd50(typed, "updatevariables", 0x4f, 0x4f): no args, sync
    (unlink car+0x20c0 ref; re-apply installed parts' wear records 0x4a8060 — not modelled)
    e->inertia = getF("engine_inertia");             // card 047
    e->torque_base (+0xd4) = 1.0;  e->nitro_torque_base (+0xd8) = 1.0;
    ... starter/friction/idle/engine_mass (card 047) ...
    f = (engine_damage >= 1.0) ? 0.100000024f : 1 - engine_damage^2 * 0.6;    // car+0x1e9c
    car->torque_health (+0x1e04) = e->torque_base * f;
    car->nitro_health  (+0x1e08) = f * e->nitro_torque_base;
    <curve block>                                     // card 045: refresh_engine_curve
    ... ratio/gears/.../C_drag (card 047) ...
  tuning:
    Vehicle__applyTuningOverrides(manager, car);     // 0x498020 — not ported

### 0x4942b0 Vehicle__onPartAttached (dyno part only)
    s = findRuntimeSlot(car, 401);                    // 0x4c6fd0(car, 0x191)
    if (s == null) dd = script.dynodata;              // getObjectField(script, "dynodata", "java.game.parts.DynoData")
    else if (s holds the part being attached) dd = part.script.dynodata;
    if (dd && class java.lang.Native exists) e->dyno = instanceToHandle(dd);   // e+0xc30
    (also car->flags |= 0x4000 unless DAT_007a75df)

## Changes (src/script/natives.cpp)
1. New native `ch_force_update` (`// 0x4b5890 Chassis.forceUpdate -> 0x4aebf0 Vehicle__refreshParameters`)
   near `ch_set_torque` (`grep -n 'bool ch_set_torque' src/script/natives.cpp`):
   - `want(...,1)`; `c = car_of(a)`; `self = a[0].kind == Value::Obj ? a[0].obj : nullptr`;
     `rt = game_rt()`; if `!c || !self || !rt` → `*out = Value::make_null(); return true;`
   - `c->flags &= ~0x800000u;`
   - small static helper `int script_int_field(Vm &, Object *, const char *name)` (0x4ece80:
     `vm.get_instance_field`; Int → value, anything else / missing → 0).
     If `script_int_field(vm, self, "suspend_update") != 0` skip to the end.
   - wheels: `for (game::WheelData &w : c->wheels) { w.tyre_wear = 0; w.brake_wear = 0;
     std::copy(std::begin(game::WheelData().pacejka), std::end(...), w.pacejka); }`
     with `// shortcut: suspension wear (+0x16c/+0x170) and the +0x160/+0x168 rates are not modelled`
     and `// INVENTED: WheelData() defaults stand in for the manager template M+0x2ec4`.
   - `Value r; std::string e; rt->call_method(self, "updatevariables", {}, &r, &e);` (ignore failure; card 041).
     `// shortcut: installed parts' wear records (0x4a8060) not re-applied.`
   - `c->torque_base = 1.0f; c->nitro_torque_base = 1.0f;`
     `float f = health_factor(c->engine_damage); c->torque_health = c->torque_base * f; c->nitro_health = f * c->nitro_torque_base;`
   - dyno pointer (0x4942b0, resolved here): `game::Slot *s = slot_by_id(part_of(a), 401);`
     `Object *src = nullptr;` if `!s` → `src = self`; else if `s->attached` → `src = s->attached`;
     if `src`: `Value dv; if (vm.get_instance_field(src, "dynodata", &dv, &e) && dv.kind == Value::Obj && dv.obj && vm.find_class("java.lang.Native")) c->dyno = game::dyno_data(dv.obj);`
     Mark `// INVENTED: resolved at refresh time; the exe stores e+0xc30 in Vehicle__onPartAttached 0x4942b0 (no part-attach events yet).`
   - `game::refresh_engine_curve(*c);`
   - end: `// shortcut: Vehicle__applyTuningOverrides 0x498020 not ported.` `*out = Value::make_null(); return true;`
2. Flip the row `{"java.game.parts.bodypart.Chassis", "forceUpdate", "()V", nullptr, true}` to `ch_force_update, false`.

## Files
- edit: src/script/natives.cpp
- read only: src/game/car.h (CarData, WheelData, refresh_engine_curve, dyno_data), src/game/runtime.h (call_method)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
