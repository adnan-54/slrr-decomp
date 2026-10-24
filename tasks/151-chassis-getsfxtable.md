---
id: 151
status: todo
branch:
depends: [150]
---
# Chassis.getSfxTable: unblock the engine block's updatevariables (torque curve)

## Why (RE 2026-10-24, debug build in a scratch copy)
After card 150 the engine is installed, but `Chassis.forceUpdate` (natives.cpp `ch_force_update`,
0x4b5890) still gets a 1-point 0 Nm curve. Cause: `rt->call_method(self, "updatevariables")`
fails and the error is dropped:
```
java.game.parts.engines.Einvagen_Duhen_Ishima_Focer.Duhen_D18V_block.updatevariables line 63:
native not implemented (stub): java.game.parts.bodypart.Chassis.getSfxTable(I)Ljava.game.parts.SfxTable;
```
Script chain (shipped): `Chassis.updatevariables` (parts/scripts/bodypart/Chassis.java:643-705)
finds the `Block` among its slots, calls `engine.updatevariables()` and only THEN
`dyno.calcDyno(desired_steps)`. `Duhen_D18V_block.updatevariables` (every engine block does the
same) calls `super.updatevariables()` (Block.java fills dynodata) and then
`the_car.getSfxTable(0/1/2)` -> stub throws -> Chassis never reaches calcDyno -> DynoData native
has count 0 -> `refresh_engine_curve` writes 1 point of 0.
Everything else on the path is already implemented (calcDyno 0x4cba80, slot 401 dynodata lookup,
refresh_engine_curve 0x4aebf0, SfxTable.clear/addItem, setSfxExhaustMinVol). A scratch build with
only getSfxTable implemented gave: Axis/D18V `calcDyno ts=33`, `maxRPM 8250`, `RPM_limit 6750`,
curve 33 points, max 159.05 Nm, all vmboot checks ok, 0 failed threads.

## Exe function (1)
`Chassis.getSfxTable` 0x4b3890:
```
FUN_004b1af0(args, &inst, &id)                 // receiver + int arg
if (id < 0) return null
car = Native__instanceToHandle(inst, DAT_007a2620)   // 0x4ecc70
if (!car || !(n = car+0xc)) return null
if (n[0x1f] != 1) n->vf5(0)                    // shortcut: not ported (same as getWheel 0x4b4150 in src)
if (FUN_0051f4f0(0xa0000000,0,0) < 0) return null
blk = n->vf3(1.0f); if (!blk) return null
e = *(blk+0x4c)->+0x2050                       // engine block
t = id==0 ? e+0xfc : id==1 ? e+0x344 : id==2 ? e+0x58c : return null   // 3 tables, 0x248 apart
o = new java.game.parts.SfxTable (JVM__getClass + 0x549ee0)
FUN_004ed320(o, DAT_007a2620, t)               // bind native handle
return o                                       // a NEW wrapper per call
```

## Files (3 + check)
- src/script/natives.cpp: `ch_get_sfx_table` modelled on `ch_get_wheel` (`car_of(a)`, id 0..2
  else null, `vm.get_class("java.game.parts.SfxTable")`, `vm.new_object`,
  `game::bind_sfx_table(o, &c->engine_sfx.tables[id])`); replace the `nullptr, true` stub entry
  of `Chassis.getSfxTable` in the native table. Cite 0x4b3890; `shortcut:` comment for the
  skipped `n->vf5(0)` / 0x51f4f0 preamble.
- src/game/car.h: fix the stale comment at `bind_sfx_table` ("No producer yet ... always null").
- src/tools/vmboot.cpp: after the existing curve check print `rpm_max_torque`, `rpm_limit`,
  `max_rpm` of `car_payload`; add a hard check: `car_payload->rpm_limit` equals the installed
  block's script field `dynodata.RPM_limit` (read via the VM from the slot-401 object, no
  hand-written number), `car_payload->max_rpm` > `rpm_limit` (Chassis.java:688-692 rounds
  1.25*RPM_limit down to 250), and `0 < rpm_max_torque < max_rpm`.
- Do NOT add the debug print of the `updatevariables` error permanently (optional: a one-line
  stderr log in `ch_force_update` when call_method fails is fine and useful; keep it terse).

## Done when
`tools/vmboot-capped.sh` (use `VMBOOT_MEM=4000000 VMBOOT_TIMEOUT=280`) passes "drive bridge
copies the installed engine's torque curve" with > 1 points and max torque > 0 (expect ~33 points,
~159 Nm for the D18V), the new rpm check passes (RPM_limit 6750 from Duhen_D18V_block.java, max
8250), all other checks green, 0 failed threads, `make -C src` builds clean.

## Rules
- Stop and report after 2 failed vmboot runs on the same check: write what you saw into Result
  and set status `blocked`.
- No unit tests or synthetic inputs; cite the exe address next to the ported code.

## Result
