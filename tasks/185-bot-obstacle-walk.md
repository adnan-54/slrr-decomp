---
id: 185
status: implemented
branch: card/185
depends: [180, 181, 184]
---
# `native bot` 0x483bd0 obstacle walk: records -> solver circles (types 1 static / 2 dynamic)

## Why (notes/19 s8.2)
Last piece of the obstacle path: between the solver reset and resolve in `bot_steer` (card 181), walk the bot's
obstacle records (cards 183/184) and add one solver circle per accepted sub-record. With this the bots steer
around other cars (and around static bodies, if the sensor ever reports one).

## Exe evidence (0x483bd0, ECX bot; S = snapshot; F = (S.pos.x + S.fwd.x, S.pos.z + S.fwd.z))
Globals `0x5da4c4` = `0x5da4c5` = 1 (switches "c4", "c5"); `angle(e, p)` = 0x40a7d0 (card 181);
`nxz(v)` = normalise (v.x, v.z), zero when the length <= 1e-5. Solver call sites: 0x484864 (type 1),
0x4845d1 (type 2), both `add(x, z, 0, 0, radius)`.
```
for (rec in list order) {                                  // list = append order (card 183)
  if (rec.body == 0) { delete rec; continue }
  if (mode == 4 && root(rec.body).id == +0x25a8) continue  // mode-4 follow target (mode 4 not ported: no-op)
  if (rec.type == 1) for (sub in chain order) {            // world coordinates
    cand = |sub.P.y - S.pos.y| < sub.h + 1.5
    if (c4 && cand && sub.ang > -0.5) {
      a = angle((sub.n.x, sub.n.z), nxz(F - sub.A.xz))
      if (|a| < sub.ang || |a| > 3.1415927 - sub.ang) cand = 0
    }
    if (!cand) continue
    if (c5 && sub.ang > -0.5) {
      if (nxz(sub.B.xz - F) . nxz(sub.A.xz - F) > 0.99) continue
    }
    solver.add(sub.P.x, sub.P.z, 0, 0, sub.r)
  }
  if (rec.type == 2) {                                     // body-local coordinates
    st = rec.body state: pos, linear vel, rotation R
    shrink = 0; scale = 1
    isOpp = (root(rec.body).id == +0x25e8)                 // the AI_spline opponent (card 180)
    if (isOpp && +0x25f0 /*overtaking, card 180*/) continue
    if (isOpp) shrink = 0.2
    O = st.pos
    if (!isOpp) {
      rel = st.pos - S.pos                                 // 3D
      if (S.fwd . rel > 0) {                               // S.fwd is the flattened unit forward
        w = st.vel - S.vel_dir * S.speed                   // 3D, S.vel_dir has y = 0
        q = w . w
        if (q > 1) {
          scale = (q - 1) * 0.4 + 1; if (scale >= 1.3) scale = 1.3
          if (S.fwd . w > 1 && S.fwd . rel > 5) continue   // pulling away ahead: whole record skipped
          t = -(w . rel) / q
          if (t > 0) O = st.pos + st.vel * t               // predicted closest approach
        }
      }
    }
    for (sub in chain order) {
      P = O + R * sub.P
      cand = |P.y - S.pos.y| < sub.h + 1.5
      if (c4 && cand && sub.ang > -0.5) {
        nw = R * sub.n; Aw = O + R * sub.A
        a = angle((nw.x, nw.z), nxz(F - Aw.xz))
        if (|a| < sub.ang || |a| > 3.1415927 - sub.ang) cand = 0
      }
      if (!cand) continue
      if (c5 && sub.ang > -0.5) {
        Aw = O + R * sub.A; Bw = O + R * sub.B
        if (nxz(Bw.xz - F) . nxz(Aw.xz - F) > 0.99) continue
      }
      rr = (sub.r - shrink) * scale
      if (rr > 0) solver.add(P.x, P.z, 0, 0, rr)
    }
  }
}
// then (card 181): solver.resolve(&V); +0x269c/+0x26a0 = V; ... ; +0x25f0 = 0; 0x482910(S, -ang, max(0, v2))
```
(`root(b)`: follow parent while body flag 4, notes/11; the id is the body's owner resource id, src
`RigidBody::owner_id`. In the exe the opponent/controlled ids at `+0x25e8`/`+0x2318` are ResourceRef ids.)

## Files
- edit: src/game/car.cpp — `bot_steer`: the walk between `reset` and `resolve`; opponent id = `b.opponent`,
  overtaking = `b.overtaking` (card 180); body pose from src `RigidBody` (`pos`, `vel`, `rotation()`)
- read only: src/game/car.h, src/game/bot_avoid.h

## Done when
- `// 0x483bd0 walk` comment with the two solver call-site addresses; nothing INVENTED expected
- `make -C src clean check` green; vmboot --race still laps all bots (existing checks); no new unit tests
- card frontmatter: status implemented, branch card/185
