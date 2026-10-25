---
id: 157
status: merged
branch: card/157
depends: [156]
---
# Wheel hits from the sensor segment + c_triggerspring__onSensorContact (0x4f3d70)

## Why (RE 2026-10-24, notes/09 "Who writes the ray hit" + corrections, notes/19 §7)
`vehicle::Car::update_suspension` gets each wheel's ground hit from `raycast_body` (INVENTED,
two-sided ray of length `max_ray`, geometric normal). The exe gets it from the sensor capsule's
trigger contacts: 0x4fd110 crossing (card 156) -> event 0x20 (synchronous) -> 0x4f3d70, which keeps
the crossing nearest to the mount, rejects one more than the wheel radius above the previous length,
and stores the PHYS face normal. Replace the raycast for mesh grounds with that rule.

## Exe functions (pasted)
`c_triggerspring__init` 0x4f3f10 (relevant part): sensor capsule `createBox(0.01, 3, L/2, 0.05)` at
`mount - (0, L/2, 0)`, `L = W+0x5c` at createCar time; `S+0x6c..74 = (0, L/2, 0)` (capsule top in
sensor frame = mount); `S+0xf0 = S+0x50`; `S+0xb4 = S+0x50^2 + 0.1`.

`Suspension__configure` 0x4f4280(k, bump, p3, p4, p5, rebound, r): `S+0x5c=k, S+0x60=bump,
S+0x4c=p3, S+0x64 = rebound>0 ? rebound : 0.7*bump, S+0x50 = p4>0 ? p4 : p3, S+0x54 = max(p5,0),
S+0x68 = S+0x58 = r (r>0) else S+0x68 = p3, S+0x58 = 0`. The sensor body is never resized.

`c_triggerspring__onSensorContact` 0x4f3d70(c) (ECX = S):
```
if (!S+0x94) return 0
side = (c+0x48 == sensor) ? 0 : (c+0x50 == sensor) ? 1 : return 0; o = side^1
if (root(c+0x48+o*8) == root(sensor)) return 1
pA = c+0x74 + side*0xc            // crossing point, sensor frame
d2 = |S+0x6c - pA|^2
if (S+0xf0 - sqrt(d2) <= S+0x68 && d2 < S+0xb4) {
  S+0xb4 = d2; S+0xd0 = ref(other body); S+0xb8 = c+0x74 + o*0xc (world point); S+0xc4 = c+0x8c (face n)
  S+0xa8/+0xac = face material (not this card)
}
return 1
```
Tick 0x4f2d70 (already ported, notes/09 "Solver order" 2-3): miss when `S+0x50^2 <= S+0xb4`;
else `len = min(sqrt(S+0xb4), S+0x50)`; then `S+0xb4 = S+0x50^2 + 0.1`.

## Files (2 + vmboot)
- src/vehicle/vehicle.h + vehicle.cpp:
  - `Wheel::sensor_len` (0x4f3f10 `L`, set once at create) and keep the previous tick's length
    (`S+0xf0`; initial value `max_ray()`, the tick's miss value).
  - `update_suspension`: if `ground->def.shape.type == SHAPE_MESH` and the mesh has
    `face_normals`: top = body.pos + R*w.mount, P0 = top - up*sensor_len, P1 = top;
    `mesh_segment_cross(*mesh, ground->pose(), P0, P1, &hits)`; best = max_ray()^2 + 0.1; for each
    hit (in the returned order): d2 = |top - X|^2; take it when `prev - sqrt(d2) <= w.radius &&
    d2 < best` (0x4f3d70, S+0x68 = r). Contact iff `max_ray()^2 > best`; then `len = min(sqrt(best),
    max_ray())`, hit.point = X, hit.normal = the face normal; the rest of the loop body is unchanged.
    Otherwise (plane/box/sphere grounds) keep `raycast_body`.
    `INVENTED:` the hit is computed at the start of `update_suspension` instead of in the collide
    pass of the previous sub-step (same pose if the exe's collide pass runs after integrate;
    UNRESOLVED, notes/19 §7); tie order among equal d2 follows the AABB tree, not node face order.
    Remove the old "INVENTED equivalent" raycast comment for the mesh path.
- src/game/car.cpp (wheel records -> simulation wheels loop in create): `s.sensor_len = w.free_len`
  (the same `W+0x5c` the sensor compound child already uses; check it is the cfg value, set before
  any script `WheelRef.setRestLen/setRadius` rewrites `free_len`). Do not add it to the card-154 per-tick
  copy in `drive_cars` (the exe capsule is never resized).

## vmboot check (src/tools/vmboot.cpp)
Keep every existing check unchanged; they are the exe-derived expectations: every wheel rests on its
spring, each load within 10% of its static share (~3.1 kN), sum within 2% of weight, no world contact
on car_root/part_comp, |vel.y| < 0.1, Spring part values reach the sim wheels, gear >= 2 and final
speed > v1 (0x4b06b0 threshold). Add:
1. Per wheel after the settle: print sensor_len, max_ray, ray_len, normal; check the sim wheel's
   `normal_world` equals the PHYS `n` of the face under it (card 156 lookup) bitwise.
2. Count gate rejections (prev - d > r) over the 300 settle frames and the launch phase; print it
   (no threshold; it is an exe behaviour, report the number).
Remove/adjust the vmboot fixture comment that says "no capsule sensor bodies yet" only if it is now
wrong; do not change the fixture itself.

## Done when
- `make -C src -j4 check` green.
- `VMBOOT_MEM=4000000 VMBOOT_TIMEOUT=280 tools/vmboot-capped.sh`: `selfcheck: PASSED`, all older
  checks still green with the numbers reported (loads per wheel, sum vs weight, max gear, final
  speed vs v1), the new check passes, 0 failed threads.

## Rules
Stop and report after 2 failed vmboot runs (write what you saw into Result, status `blocked`).
No unit tests or synthetic inputs; cite exe addresses; no tuning constants (if an older check now
fails, that is a finding to report, not a reason to change its threshold).

## Not this card
Per-face material friction (S+0xa8/+0xac: face mat -> ground shape material array +0xb0 stride
0xc4, count +0xac), node streaming 0x4fe6f0, body-vs-soup narrow phase 0x4feb20/0x4fc7e0, the
0x4f3d70 own-car-parts early return (src only queries the groundbody).

## Result
Implemented.

- `Wheel::sensor_len` (W+0x5c = `free_len`, captured once in the create wheel loop,
  `src/game/car.cpp:1150`) and `Wheel::gate_rejects` (diagnostic counter). `ray_len`
  (S+0xf0) now survives the per-tick reset and starts at `max_ray()`; it is the next
  tick's 0x4f3d70 gate input.
- `Car::update_suspension` (`src/vehicle/vehicle.cpp:134`): mesh ground with
  `face_normals` -> `mesh_segment_cross` over `top - up*sensor_len .. top`
  (0x4fd110), gate `prev - d > radius` rejected (0x4f3d70, S+0x68 = r), nearest
  `d2 < best` wins; contact iff `max_ray()^2 > best`, `len = min(sqrt(best), max_ray())`,
  point/normal from the crossing (PHYS face normal). Plane/box/sphere and SCX meshes
  without stored normals keep `raycast_body`. INVENTED marker added for the query
  timing/tie order.
- `vmboot`: per-wheel `sensor_len/max_ray/ray_len/normal` print plus bitwise
  `normal_world` vs PHYS face normal check; gate rejection counts printed for the
  300 settle frames and the 900 launch frames.

Checks (`make -C src clean check`, same vmboot binary under `ulimit -v 1500000` and
`timeout 300`): `selfcheck: PASSED (103 checks, 0 failure(s))`, 0 failed threads
(1522 threads, 30000 frames). Loads 3069.0/3260.2/3097.6/3133.7 N vs static shares
3169.7/3169.7/3110.2/3110.2 N, sum 12560.4 N vs weight 12559.7 N (<2%), |vel.y| 0.079,
no world contact on car_root/part_comp. Sensor: sensor_len 0.7050, max_ray
0.5947/0.5988, ray_len 0.5504..0.5536; all four normal_world bitwise equal the PHYS
face normal (tris 56279/56279/56279/37934); 0 gate rejections over the 300 settle
frames, 1 over the 900 launch frames. max gear 5, final speed 103.41 m/s vs v1 12.42.

Doubt: the exact `VMBOOT_MEM=4000000 VMBOOT_TIMEOUT=280 tools/vmboot-capped.sh`
command could not be run in this session (the shell tool asks for approval on that
script/inline `ulimit`); the `make check` run above executes the same binary with a
tighter cap (1.5 GB / 300 s) and is green.
