---
id: 171
status: merged
branch: card/171
depends: [169]
---
# Hull-vs-soup contact refresh as 0x4f9af0: no stale deep wall contacts (spline driver past cp1)

## Why (RE 2026-10-24, notes/11 "Contact refresh 0x4f9af0", notes/19 s9)
Scratch probe: the card-169 spline driver, left running after cp0, passes cp1 (nextTrigger[0] 2 at frame 5182),
then brushes a wall at ~7 m/s (f6011, -38.9 -3.9 -101.9). One hull-vs-soup contact (sep -0.53, n (0,0,-1)) is
refreshed for 4 frames with the SAME world point while the car moves 8 m: |v| 6 -> 116 m/s, the car is launched,
and a second such contact later (f8801) sends it through the map. src `refresh_hull_contacts`
(src/physics/world.cpp ~1112) does `Contact c = old` (old points/normal, only `separation` replaced) and uses the
INVENTED `hull_plane_distance` fallback against the infinite face plane. Exe recomputes points, normal and features
from 0x50d9d0 on every refresh and drops the pair on a 0.0 result.
Script does: lap/checkpoint rules. Exe provides (this card): contact refresh.

## Exe code (rea/Ghidra 12.1.4, 0x4f9af0, hull pairs; r = rA + rB = 0 for mesh-vs-mesh, keep the r terms)
```
st = integrated state (idx^1) of A and B; shB = pair+0x58 override (+0x2c) if set
d = 0x50d9d0(stA, stB, shA, shB, &fA, &fB, &ptA_local, &ptB_local, &n)   // writes features, points, normal
if (d == 0 || d > r + 0.05) delete pair
if (d < r + 0.02) wake both roots
if (d > r || d > -r || old_sep >= r) {
    if (d <= r) {  // same bisection as 0x4fc7e0, both bodies via 0x506c00, t = 0.5, step = 0.25, 10 iters
        loop: d = 0x50d9d0(interp(t)); if d > r { if d < r+0.02 && (iter==0 || n.(pA_w - pB_w) < 0) break; t += step }
              else t -= step; step *= 0.5
        if (d <= r) d = 0x50d9d0(integrated states)
    }
    ptA -= rA*n_A_local; ptB += rB*n_B_local; sep = d - rA - rB; world_point = R_A*ptA + posA
} else {           // deep now and deep last refresh
    same margin offsets; sep = d - rA - rB; world_point unchanged
}
```

## Files
- `src/physics/vclip.h`, `src/physics/vclip.cpp`: `vclip_check` also returns `point_a`, `point_b`, `normal` (the
  `Hit` it already computes; local points the way `Contact::local_a/local_b` store them).
- `src/physics/world.cpp`: `refresh_hull_contacts` follows the block above; delete the `hull_plane_distance`
  fallback and the side gate there (keep them in `consider_hull_pair`, card 165).
- `src/tools/vmboot.cpp`: checks below.

## vmboot (spline driver block, card 169)
After the existing cp0 check, keep the same driver loop running (same INVENTED rules) until `nextTrigger[0] == 3`
or 9000 frames. Print frames, final nt, current_lap, max |vel|, min pos.y, max spline distance.
1. **Hard check**: `nextTrigger[0]` reaches 3 (cp0, cp1, cp2 accepted by CircuitRacing.handleCheckpoint).
2. INVENTED bound: max |vel| in the block < 60 m/s and pos.y never below spawn y - 20 m (no launch/fall-through).
3. All existing checks pass (cp0 check, forced drag, card 168, V-Clip overruns 0), 0 failed threads.

## Done when
`make -C src -j4 check` green; `ulimit -v 4000000; timeout 290 src/build/vmboot --selfcheck` PASSED with 1-3.
Result: frames, nt, max |v|, min y, deep-branch count, overruns.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). If the car no longer launches but
the harness cannot take a corner (stuck/off route), report that as a harness limit, do not tune more than once.
No unit tests, no map names in engine code, every harness rule `INVENTED:`.

## Not this card
Lap 2 / finish window / exit (card 172), native bot, 0x50d9d0 handlers line by line.

## Result (2026-10-24)
`make -C src clean check` green: 153 checks, 0 failures. Spline driver: 7823 frames, `nextTrigger[0]` 3
(cp0 accepted at frame 1856, 2.21 m from the trigger), rpTotal 24, current_lap 1, max |v| 30.83 m/s,
min y -5.22 (spawn -3.72), max spline distance 4.52 m, deep-branch contacts 179, vclip overruns 0.
Before the fix (main physics) the same block launched at f1501+ and hit 121.67 m/s / y -114.45.

One harness tune was needed (INVENTED, vmboot only): with the stale contacts gone the driver entered the
Norisring hairpin (route_1.spl points 31-33, the z -571 U-turn) at ~18 m/s, lost the back and rolled the
car at f~1461-1601, never reaching cp0. Braking now starts up to ~70 m before a corner (`turn_ahead(s+30)`)
and corners are taken at 8 m/s instead of 12. Car and contact code unchanged by the tune.
