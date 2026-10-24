---
id: 164
status: todo
branch: card/164
depends: [163]
---
# GameRef.setMatrix on a car (0x4db950 -> 0x4eb3f0 -> type_car vf7 0x4a99b0) + vmboot career race

## Why (RE 2026-10-24, notes/09 "Script placement of a car")
The career flow (Garage CMD_EVENTLIST 133 -> `EventList(MODE_AMATEUR)` -> eGen CMD_RACE ->
`java.game.Track` + `java.game.CircuitRacing`) reaches the Track with 0 failed threads, but 12 drive
checks fail there. The scripts place every car with `GameRef.setMatrix` from the map's grid data
(`Track.applyStartGrid`, Track.java:2901-2941: `player.car.setMatrix(startGridData[0], [1])` + "reset";
freeride: `Track.lockCar` setMatrix(posStart, oriStart), Track.java:1688-1697). The grid itself is script
data of the map's `TrackData.getGrid()` (a mod script), not exe. src `gr_set_matrix` (natives.cpp ~2412)
only writes `Resource::pos/ori`, so the bodies never move.
Scratch probe (vmboot with cmd 133): player Resource pos = grid (40.067 -4.301 22.549), `car_root` still
at (0.010 -0.208 -0.061) stacked with 9 other sleeping cars at the origin; the woken car overlaps their
hulls (105x "Voronoi error (infinite loop)" on stderr), is thrown up (pos.y 51, vel.y -44.5, 0 wheel
hits) -> springs unloaded, speed checks off. Not a map-layout/ground/height problem: the ground mounts
(type-1 master, ground body at params xyz) and the soup is under the start.

## Exe code (rea/Ghidra 12.1.4 + asm, read 2026-10-24)
`0x4db950` vm_gameref_setmatrix:
```
  parseParameters(&self, &vecRef, &yprRef);              // 0x4b1af0
  if (typed(self)) {                                     // 0x4ecc70
    x,y,z = vec ? vec.x/y/z : 0;  yaw,pitch,roll = ypr ? ... : 0;
    Str__format(buf, "ffffff", x, y, z, yaw, pitch, roll);   // 0x56a000
    FUN_004eb3f0(buf);                                   // ECX = the GameRef handle
  } else error "vm_gameref_setmatrix: scripted resource ... not found"
```
`0x4eb3f0`: typed object (0x51f4f0(0xa0000000)) -> type manager -> `vtable+0x1c(typed+0x4c, buf)`.
For a car this is `type_car__vf7 0x4a99b0` (vtable 0x59dcd8 slot 7). Other types: not this card.
`0x4a99b0`(car, str), ECX = M (condensed; see notes/09 for every field):
```
  weight = |gravity(DAT_5dadf0+0x1c4)| / def(car+0x13ec body)+0x2c;   // = g*m
  Str__scan(str, "fffffffffffffffi", pos3, ypr3, vel3, angvel3, steer, spin, pitchadd, dir); // rest 0
  R = ypr_to_matrix(ypr);                                // 0x5666d0
  if (M+0x1fb4 /*wheels*/ < 4) pos += R * M_center;      // M+0x1f1c = cfg `center` (0x48de60), 0 in shipped cfgs
  else {
    for i<4: c_i = (W.x, W.y - ((W+0x60 radius + W+0x50 rest) - (weight/n)/W+0x44 k), W.z);  // W+0x78..0x80 hub
    pitch += atan2((y2+y3-y0-y1)/2, (z2+z3-z0-z1)/2);    // fpatan order checked, asm 0x4a9db6
    roll  -= atan2((y1+y3-y0-y2)/2, (x1+x3-x0-x2)/2);    // asm 0x4a9dc7
    R = ypr_to_matrix(ypr); v = R * mean(c_i);
    pos.x -= v.x; pos.z -= v.z; pos.y -= min(v.y, -(R*M_center).y);   // tyres touch pos
  }
  /* flaps car+0x1518: keep their offsets to the chassis across the move */
  moved = GameRef_placeBody 0x4eac50(pos, R);            // root pos = pos - R*off(0x4e9730), rot = R, both buffers, 0x5030b0
  if (moved) { root vel = vel; root angular = angvel (0x506ce0, both buffers); car+0x12c/+0x138; 0x4c6d30 }
  car+0x1e5c = +0x1e60 = 0; +0x1ddc = steer; +0x1de4/1de8/1dec/1df4 = 0; +0x1e7c = +0x1e78 = +0x1e80 = 0;
  if (|+0x1de0 - steer| > 0.001) { 0x4aa830(car); +0x1de0 = steer; }
  if (moved) for each wheel: W+0xc0 = 0; if (S = W+0x30) { 0x4f4360/0x4f4310; S+0xf4 = 0;
      S+0xf0 = (W+0x60 + W+0x50) - (weight/n)/W+0x44 - W+0x80*(pitchadd/...); /* pitchadd 0 */ car+0x70 |= 0x100000; }
  if (!(root+0x58 & 0x80)) PhysBody__sleep 0x506d90(root);   // placed car sleeps until "wakeup"
  car+0x70 &= ~0x30000;  (dir -1 / 1 -> |= 0x10000 / 0x20000)
```
`0x4eac50`(pos, mat) on the car's GameRef body (part_comp): `0x4e9730` walks part_comp up to its root
and returns its offset `off` in root frame; `root.pos = pos - R*off`; root orientation from R (0x567ea0);
copied to the other state buffer; `0x5030b0` re-derives child poses; returns true if pos/rot changed.

## Field map exe -> src
| exe | src |
|---|---|
| typed car of the GameRef | `g_cars.find(resource)` (car.cpp:69), `CarData` |
| root / part_comp | `CarData::root_body` / `chassis_body` |
| part_comp offset in root frame (`off`) | `-CarData::com` (rebuild_compound ~675: root->pos is the COM) -> `root->pos = pos + R*com` |
| W+0x78..0x80 hub (chassis frame) | `CarData::wheels[i].hub` |
| W+0x60 + W+0x50 | sim `Wheel::rest_ray()` |
| W+0x44 | sim `Wheel::spring` |
| S+0xf0 | sim `Wheel::ray_len` |
| m (def+0x2c of the car+0x13ec body) | the mass src uses for weight in the drive checks; if part_comp vs car_root is unclear, use car_root `1/def.inv_mass` and say so in Result (UNVERIFIED in notes/09) |
| +0x1ddc/+0x1de0, +0x1e5c, +0x1e7c/+0x1e78 | `steer_input`/`steer_copy`, `gear`, `sim->engine.omega`/`shaft_speed` (as `"reset"` in car_command) |
| 0x5666d0 / 0x567ea0 | `euler_to_mat` / `mat_to_quat` (engine/math.h) |
| 0x506d90 | `World::sleep(root)` |

## Files (<= 3 + vmboot)
- `src/game/car.cpp` (+ one decl in `src/game/car.h`): `bool car_set_matrix(Runtime &rt,
  Runtime::Resource *car, const float pos[3], const float ypr[3])` = 0x4a99b0 with the setMatrix args
  (vel/angvel/steer/spin/pitchadd/dir = 0). Returns false when `car` is not in `g_cars`.
  Shortcut comments (one line each): flaps car+0x1518 (src has none), car+0x12c/+0x138 and 0x4c6d30
  render copies, 0x4aa830, the sensor pose refresh 0x4f4360/0x4f4310 (src rays use the body pose), the
  `M_center` cfg keyword `center` (0 for every shipped cfg; parser not ported). Use the 4-wheel branch
  when the car has >= 4 wheels, else the `< 4` branch with M_center 0.
- `src/script/natives.cpp` `gr_set_matrix`: keep the Resource pos/ori write, then call
  `car_set_matrix` (0x4eb3f0 -> vf7). Other typed objects keep the current behaviour
  (shortcut comment: vf7 of non-car types not ported).
- vmboot (`src/tools/vmboot.cpp`):
  - Garage cmd at `kTestDriveCmd`: 133 (CMD_EVENTLIST, Garage.java:1587-1593) instead of 134; the
    kRaceCmd eGen 0x10E step stays (it already reaches the career Track). Rename comments/labels.
  - Replace "Track.game is the freeride gamemode" with: `Track.game` non-null and `is_a
    java.game.Gamemode`; print its class and the map leaf (corpus table, tools only).
  - Remove the INVENTED fixture that sets `root->pos.y = floor_y + ...` (vmboot ~1097): the script
    places the car now. Keep the floor ray for the print and the existing queued "wakeup".
  - No map name may appear in src/game, src/script, src/physics (grep in Result).

## vmboot checks (shipped data only)
Keep every existing check (131) except the freeride-gamemode one (replaced above). New hard checks at the
drive start (before the 300 settle frames):
1. `Track.startGridData.size() > 0` and the player car_root xz is within 3.0 m of
   `startGridData.elementAt(0)` xz (0x4a99b0 shifts only by the rotated wheel-contact centroid).
2. No other car's root body (active or sleeping, `inv_mass > 0`, no parent) within 2.0 m of the player
   car_root. Print every car root pos (the stacked-at-origin bug).
3. At the drive start `|car_root vel| < 0.5 m/s` (placed, slept per 0x506d90, woken by the script's
   countdown "wakeup": it did not fall or get thrown). No test hooks in src for this.
4. All old drive checks pass on the career map: every wheel rests on its spring, loads within 10% of
   the static share, carried by the springs, settles, V-Clip, sensor crossings and normals, pacejka p13,
   auto gearbox 1st/2nd, final speed beats the 1st-gear top speed, forced-drag run, semi-auto checks
   (the car moves under throttle).

## Done when
`make -C src -j4 check` green; vmboot (`ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck`)
`selfcheck: PASSED`, checks 1-4 pass, 0 failed threads; the Track runs a career Gamemode
(print the class, expected `java.game.CircuitRacing` from the amateur event), the player car rests on
the ground (spring loads ~= weight) and moves under throttle. Result lists: Track.game class, map leaf,
grid pos vs car_root pos, spring loads vs weight, final speed, and the count of "Voronoi error" lines on
stderr (was 105; expected 0).

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). No unit tests or
synthetic inputs; cite exe addresses; `INVENTED:` for any harness step. Do not add rules the scripts hold
(grid choice, countdown, wakeup timing are script). No map names in src/ engine code; the map comes from
the script's GroundRef record.

## Not this card
vf7 of non-car typed objects, `GameRef.setState` (0x4dbf20, same vf7 with vel/angvel), flaps, the cfg
`center` keyword parser, freeride test-drive coverage (the flow now goes career; freeride lockCar uses
the same native), rem_part/uninstall 0x4cb510.

## Result
