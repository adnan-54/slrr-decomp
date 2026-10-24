---
id: 162
status: todo
branch: card/162
depends: [161]
---
# Semi-auto gearbox 0x4ad480 (transmission flag 0x20)

## Why (RE 2026-10-24, notes/09 "Semi-auto 0x4ad480")
`updateControls` 0x498300 calls `0x4ad480(e, car, dt)` when `car+0x70 & 0x20` (set by
`"transmission 5"` = `Vehicle.TRANSMISSION_SEMIAUTO`, Vehicle.java:27, through 0x4a37f0, already ported in
car.cpp ~1276). src skips it (car.cpp ~1787 `shortcut: semi-auto 0x4ad480`), so a player with
`Config.player_transmission = 5` never shifts after engaging 1st.

## Exe code (rea/Ghidra 12.1.4, decompile + asm 0x4ad480..0x4ad5fc, read 2026-10-24)
Constants (read_bytes): `0x593838` = 0.2 (`cdcc4c3e`), `0x59ab98` = 9.549296 (`ebc91841`). `ESI` = car,
`EDI` = e (car+0x2050). `dt` is not read. The pushed 1.3f (`0x3fa66666`) is not read by 0x4b19a0.
```
void FUN_004ad480(int e, int car) {
  if (0.0 < car+0xb4 /*tear*/ && (car+0x70 & 0x4000) && !(car+0x70 & 0x40004) &&
      e+0x20 /*gears*/ != 0 && car+0x1e5c /*gear*/ != 7 && car+0x1e5c != 0 &&
      ABS(FUN_004b19a0(e, car /*,1.3f*/)) < 0.2) {                 // 0x4ad4ff comiss 0.2, JBE -> return
    w = car+0x1e78 /*shaft*/ * car+0x1e0c /*ratio[gear]*/ * e+0x44 /*rearend*/;
    r = w * 9.549296;
    T = (e+0x1c /*RPM_limit*/ <= r) ? 0.0 : FUN_004a2dd0(r, e);   // 0x4ad532 comiss, JBE -> T = 0
    if (0.0 <= r) {                                               // 0x4ad570 comiss, JC -> negative branch
      FUN_004b06b0(e, car, r, T, w);                              // upshift
      FUN_004b05a0(e, car, r, T);                                 // downshift
      return;
    }
    if (car+0x1e74 <= 0.0) {                                      // 0x4ad5ca JA -> return
      car+0x1e5c = 1;                                             // gear 1 (the auto box 0x497930 uses 0 here)
      car+0x1e68 = *now(0x5dadf0) + 0.2;
      FUN_004b0190(e, car, 0);
    }
  }
}
```
## Field map exe -> src (all callees already ported in src/game/car.cpp)
| exe | src |
|---|---|
| `car+0xb4` tear | `tear` argument (same as `auto_gearbox`, car.cpp ~1670) |
| `car+0x70`, `+0x1e5c`, `+0x1e0c`, `+0x1e78`, `+0x1e74`, `+0x1e68` | `c.flags`, `c.gear`, `c.gear_ratio`, `c.shaft_speed`, `c.shift_t74`, `c.shift_end` |
| `e+0x20`, `e+0x1c`, `e+0x44` | `c.gears`, `c.rpm_limit`, `c.rearend_ratio` |
| 0x4b19a0 / 0x4a2dd0 / 0x4b06b0 / 0x4b05a0 / 0x4b0190(…,0) | `drive_slip(c)` / `c.torque_at(r)` / `upshift(c, r, T, w, now)` / `downshift(c, r, T, now)` / `shift_timers(c, false, now)` |

## Files (<= 3 + vmboot)
- `src/game/car.cpp`: `static void semi_auto(CarData &c, float tear, float now)` = the code above, next to
  `auto_gearbox`; call `if (c->flags & 0x20u) semi_auto(*c, tear, now);` between the `auto_gearbox` and
  `auto_clutch` calls (exe order 0x10 -> 0x20 -> 0x40). Drop "semi-auto 0x4ad480 (flags 0x20)," from the
  shortcut comment. Count shifts for vmboot: `CarData::semi_shifts` (int, `INVENTED:` instrumentation, no
  exe field) += return values of upshift/downshift + 1 for the negative-r branch.
- `src/game/car.h`: the `semi_shifts` field.
- `src/audio/sfx.h` (comment only, line ~98-99, notes/14 §2.3): `exhaust_resource[2]` are nodes
  `M+0x1f84 / M+0x1f94` (car type), `exhaust_timer[0]` = `car+0x224c` one-shot (cleared by vf13),
  `exhaust_timer[1]` = `car+0x2248` aero drag magnitude (written by Vehicle::update 0x49ff20, read by
  0x494910 at 0x495193 as a wind-noise intensity). No code change in sfx.
- vmboot (`src/tools/vmboot.cpp`): checks below, placed after the card 161 drag run (end of the drive section).

## vmboot checks (exe gates + shipped data only)
Keep every existing check. INVENTED harness: send `"transmission 5"` with `game::car_command(*rt, car_res,
0x10, ...)` (Track.enter sends `"transmission " + Config.player_transmission`, default 1; 5 is the
shipped `Vehicle.TRANSMISSION_SEMIAUTO`), then `"reset"` and wait for `|shaft_speed| <= 1` as card 152 does.
1. `(flags & 0x70) == 0x60` after the command (0x4a37f0: tr 5 -> 0x20 | 0x40 auto clutch).
2. Neutral stays: hold Up (200) 60 frames with no shift key: `gear == 0` on every frame (0x4ad480 returns
   when gear == 0; the auto box 0x10 is off). Release Up.
3. Engage: A (0x1e) 10 frames then release (0x498300 shift block, as card 147); `gear == 1` (only if
   `gears > 0`, else stop and report).
4. Drive: hold Up 600 frames, no shift key. Hard checks: `semi_shifts > 0`, max gear reached >= 2, every
   observed gear change between frames is +-1 and the number of observed changes `<= semi_shifts`
   (the only shifter running), `gear` never becomes 0 or 7. Print max gear, semi_shifts, final |vel| and
   final rpm; hard check final |vel| <= v_top (same bound as card 159/160).
5. Restore `"transmission " + Config.player_transmission` afterwards and re-check `(flags & 0x70)` equals
   the existing check's `tr_bits`.

## Done when
`make -C src -j4 check` green; vmboot (`ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck`)
`selfcheck: PASSED`, checks 1-5 pass, 0 failed threads. Result lists the printed values.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). If check 4 never shifts
because the slip gate (`|drive_slip| >= 0.2`) holds on every frame, print the slip per 60 frames and
stop and report (do not loosen the gate). No unit tests or synthetic inputs; cite exe addresses; no
tuning constants; `INVENTED:` for the transmission-5 harness and `semi_shifts`.

## Not this card
Manual-clutch branch of 0x4a37f0 (`tr & 1 == 0`), 0x4b03c0 shift completion, speed limiter 0x1000,
sfx vf13 0x494910 port, type-7 / mesh-less type-6 inertia (0x5056a0, notes/11).

## Result
