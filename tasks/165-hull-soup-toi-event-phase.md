---
id: 165
status: todo
branch: card/165
depends: [164]
---
# Hull-vs-soup contacts created in the event phase with the 0x4fc7e0 TOI bisection

## Why (RE 2026-10-24, notes/11 "Voronoi error in vmboot")
`vmboot --selfcheck` prints 248 `Voronoi error (infinite loop)` lines (stderr). Every one comes from
`vclip_closest` called by `consider_hull_pair` (world.cpp ~859, the only runtime caller), car hull child
(closed, 38..194 features) vs one two-sided road/wall triangle (0x50f000, 8 features), and all of them
happen in the INVENTED forced-drag run (vmboot ~1978), after the car leaves the auto-drive end
(45,-4.8,-402) at 44 m/s and crashes near z -450..-497 (final |v| 1.7 m/s). Normal driving: 0.
Probe (scratch, walk trace): the walk 2-cycles `F(hull)/E(tri) <-> E(hull)/E(tri)` (e.g. F63/E3 <-> E57/E3)
with the hull centre 0..0.6 m from the face plane = the hull deeply penetrates the face.
The message and its budget (`nfeatA + 2*nfeatB`) are the exe's (0x50d230 prints the same string and
returns 1e6), so the string itself is faithful. The cause is how src gets there:
- src creates new hull-vs-soup contacts only in `update_contacts(add_new=true)` at the START pose of the
  step (world.cpp ~1046), with a static sphere filter (`swept_sphere_face(cp.pos, cp.pos, ..)`) and a
  cold V-Clip start (`fa = fb = 0`). A hull moving 0.7 m/step is 0.5 m off the wall at step start (no
  contact), deep inside at the end, and is only seen next step, already penetrating.
- exe creates them in the event phase H of 0x502060, AFTER integration (notes/08 "Exact pass order":
  0x4fc610 -> 0x4ee460 -> 0x4fe6f0 -> 0x4ff6b0 -> 0x4feb20 + 0x4fc7e0), with a swept filter from the
  previous pose and a 10-step TOI bisection prev->current, warm-started from the cached features.
(A scratch port of the exe stepEE 0x50a410 / stepEF 0x50aa60 alone raised the count to 2552: the walk is
not the fix while the hull is deep inside the face. Walk port = separate later card, not this one.)

## Exe code (Ghidra 12.1.4, read 2026-10-24)
`0x502060` (notes/08): `D integrate; loop { F solve; G updateContacts 0x4f9af0; if converged (or iter 19):
H event phase -> new contacts (addContact 0x502830); I re-integrate } while (!converged && iter <= 19)`.

`0x4feb20` Collide__bodyVsTriangles filters (notes/19 "Body-vs-soup narrow phase"):
```
s = prev state of the body (state idx^1 is the integrated one, see 0x506c00 below)
node test: |s.pos - node.c|_k <= node.h_k + br
face: 0x4f9860(s, sweep = (prev pos, motion), br, face AABB)   // swept sphere vs face
OBB = shape+0x14 half extents + |R·motion|·1.5 + 0.05          // SAT box vs triangle 0x4fc450
```
`0x506c00`(A = state^1, B = state, out, t): `out.pos = A.pos*t + B.pos*(1-t)`; `out.q = slerp(B.q, A.q, t)`
(0x5681c0) normalized (0x5665c0); `out[9] = B[9]*(1-t) + A[9]*t`. So t = 1 is the integrated pose,
t = 0 the step-start pose.

`0x4fc7e0` Collide__candidatesTOI(body) (condensed; candidates = (body child, triangle) pairs from 0x4feb20):
```
cur = body state at t = 1 (integrated); t = 1.0; step = 0.5; iter = 0
for cand: feat = PairCache 0x4f6ea0(child, tri) +0x28/+0x2c (none -> first feature)
          d = 0x50d230(cur, ground, shape, tri poly, &fA, &fB, &pA, &pB, &n) - margin(shape+0x10)
          best = min d; stop scanning when best <= 0
loop:
  if best.d > 0:
     if best.d < 0.02 and (iter == 0 or n·(worldPt_now - pB_world) < 0 /*approaching, 0x505d60*/): break
     if iter == 0 and best.d >= 0.02: break                  // no contact
     t += step
  else t -= step
  step *= 0.5; if (++iter >= 10) break
  if best.d <= 0: re-evaluate ONLY best at interpolate(t); if still <= 0: next loop turn;
                  else mark it (+0x3c = 1) and fall through
  re-evaluate every unmarked candidate at interpolate(t) (marked: just clear the mark, keep its d);
  best = first strictly smaller d; stop scanning when best <= 0
end:
if best.d <= 0.02:
   if best.d <= -margin:   // deep: min hull vertex (kind-1 features) distance along the face normal
       best.d = min_v n·(R v) + n·(pos - face point); fA = that vertex; pA = it; pB = pA - n·d
   pose = interpolate(t); 0x4fa550 world point; margins (pA -= r·Rᵀn); 
   addContact 0x502830(body, ground, tri, fA, fB, pA, pB, n, d, toi = t, worldPt)
write every candidate's fA/fB back to its pair cache (0x4f6570 creates the cache entry if missing)
```

## Field map exe -> src
| exe | src |
|---|---|
| state^1 / state (t=1 / t=0) | integrated `RigidBody` pose / `StepState::start` (world.cpp ~1032) |
| 0x506c00 | lerp pos + `quat_slerp` (engine/math.h; add nothing new if a slerp exists, else lerp+normalize with a `shortcut:` comment) |
| candidate pair cache +0x28/+0x2c | an existing `Contact` with the same `child_index`/`soup_triangle` (its `feature_a/b`), else 0 |
| deep-penetration vertex pick | existing `hull_plane_distance` (already this rule) |
| addContact toi | add `float toi` to `Contact` only if resolve needs it; otherwise do not add it |

## Files (<= 3 + vmboot)
- `src/physics/world.cpp`:
  1. `consider_hull_pair(a, g, next, start)` takes the body's `StepState::start`; the node/face filters
     use the swept form (prev pos -> current pos, `swept_sphere_face(p0, p1, ..)` already takes two
     points; OBB half extents `+ |Rᵀ·motion|·1.5`), then runs the 0x4fc7e0 bisection above and adds at most
     one contact per body (earliest TOI). Keep the existing INVENTED side gate / support rule only where
     the bisection ends still penetrating (deep branch); say in Result whether the gate is still needed.
  2. In `World::step` (~1046..1075): `update_contacts(*this, true)` before integration keeps its
     body-vs-body/static work but no longer calls `consider_hull_pair`; the hull-vs-soup creation runs
     in the event phase position: inside the sub-iteration loop when `converged` (or `it == 19`), after
     the resolve, before the re-integrate (exe H). New contacts there set `converged = false`
     only as the exe does (0x4ffd20 `resolveImpacts` result; if not ported, `shortcut:` comment).
- `src/physics/vclip.cpp/.h`: no change, except an `int vclip_overruns()` counter incremented next to
  the existing fprintf (INVENTED: harness counter, read only by vmboot).
- vmboot: see checks.

## vmboot checks (shipped data only)
Keep all 135 checks. New:
1. Hard: `vclip_overruns()` over the whole career-race block (auto drive + forced drag + semi-auto) is 0
   (was 248). Print it.
2. Hard: every hull-vs-soup contact created during the forced-drag run has `0 <= toi <= 1` (if `toi`
   was added) and separation `> -0.05` m after the bisection (print the min; 0x4fc7e0 accepts d <= 0.02,
   the deep branch is the exception: print how many contacts took it).
3. Print the crash: frame, car_root pos and |v| when the first wall contact (|n.y| < 0.5) appears.

## Done when
`make -C src -j4 check` green; vmboot (`ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck
2>&1`) `selfcheck: PASSED`, 0 `Voronoi error` lines, all old drive checks still pass (springs carry the
weight at rest, auto/semi drive speeds). Result: overrun count, min separation, deep-branch count, crash
frame/pos, auto/semi final speeds before/after.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). No unit tests or
synthetic inputs; cite exe addresses; `INVENTED:` for any gap filling. Do not change `vclip_closest`'s
walk (separate card). Do not touch the dealer pick (card 166). No map names in src/physics.

## Not this card
Exe walk port (stepEE 0x50a410, stepEF 0x50aa60, clipEdge 0x508fe0, stepVE 0x50be40, stepVF 0x50c830):
decompiled in notes/11 "Voronoi error in vmboot"; body-vs-body TOI (0x4fb370); streamed soup nodes.

## Result
