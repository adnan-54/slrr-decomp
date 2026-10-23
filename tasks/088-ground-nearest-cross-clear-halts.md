---
id: 088
status: implemented
branch: card/088
depends: [087]
---
# game/script: GroundRef.getNearestCross + clearHalts on the traffic graph

## Goal
Port `Traffic__nearestCross` (0x419050) and `Traffic__clearHalts` (0x417c50) into
`traffic::Graph`, and the natives `getNearestCross(Vector3,float)` (0x4dd880) and
`clearHalts()` (0x4dd6c0) on top of card 087's `Runtime::ground_traffic`. Script users:
`game/Map.java:123,386`, `game/Track.java:1204,1211` (`getNearestCross(pos)` = the
script wrapper with distance 0.0). Trace: notes/19 §2.

## Exe evidence (Claude-traced; 0x4dd880 asm checked)
### 0x4dd880 GroundRef.getNearestCross(Vector3 pos, float dist) Vector3
    getArgs(ctx, &self, &pos, &dist);                 // asm: [ebp-0x14] self, [ebp-0x10] pos, [ebp-0x24] dist
    if (pos == 0) return 0;
    h = instanceToHandle(self); if (h == 0) return 0;
    t = Game__getInfo(h, 0x39, 0); if (t == 0) return 0;
    x = pos."x" (0x5a241c); y = pos."y" (0x5a23e8); z = pos."z" (0x5a23d4);
    c = Traffic__nearestCross(t, x, y, z, dist, 0);   // asm 0x4dd934: push 0 (yflag), [esp+0xc] = dist
    v = new Vector3; v.x = c+0x24; v.y = c+0x28; v.z = c+0x2c;   // 0x5a2458/5c/60 "x","y","z"
    return v;                                         // c is not null-checked (asm 0x4dd995)
### 0x419050 Traffic__nearestCross(x, y, z, dist, yflag) — ECX = Traffic
    best = 0; bestKey = <param yflag slot, unused before first hit>;
    for each crossEq e (array +0x10, count +0x14), then each crossLight e (+0x28, +0x2c):
        if (yflag == 0 || |e.pos.y - y| < 5.0) {
            key = | sqrt((e.pos.x-x)² + (e.pos.y-y)² + (e.pos.z-z)²) - dist |;
            if (best == 0 || key < bestKey) { best = e; bestKey = key; }
        }
    return best;
(`e.pos` = cross +0x24/+0x28/+0x2c; the `yflag` byte is the low byte of the 5th arg.)
### 0x4dd6c0 GroundRef.clearHalts()V
    getArgs(ctx, &self); h = instanceToHandle(self);
    t = Game__getInfo(h, 0x39, 0);
    if (t) Traffic__clearHalts(t);
### 0x417c50 Traffic__clearHalts — ECX = Traffic
    for i < laneCount(+0x5c): lane[i] +0xc4 (byte) = 0;

## Changes
1. `src/game/traffic.h` / `traffic.cpp`, in `Graph`:
   ```cpp
   // 0x419050: crossEq first, then crossLight; first strict minimum wins. null if none.
   const Cross *nearest_cross(float x, float y, float z, float dist, bool yflag) const;
   // 0x417c50: every lane's halt flag (+0xc4) = 0.
   void clear_halts();
   ```
   Distances in float (`std::sqrt` on float, `std::fabs`), the 5.0f band only with `yflag`.
2. `src/script/natives.cpp`: replace the card-085 bodies of `gr_get_nearest_cross` and
   `gr_clear_halts`:
   - `gr_get_nearest_cross`: `want(...,3)`; `a[1]` Null -> null result; `get_o` +
     `vec_read` the position, `get_f(a[2])`; `t = game_rt()->ground_traffic(value_handle(a[0]))`
     (no handle or no graph -> null); `c = t->nearest_cross(x, y, z, dist, false)`;
     `c == nullptr` -> null (INVENTED: the exe dereferences it); else
     `new_point(vm, "java.lang.Vector3", c->pos, false, err)`.
   - `gr_clear_halts`: `t` as above; `if (t) t->clear_halts();` null result.
   Keep the `// 0xADDR` comments; drop the SERVICE MISSING lines of these two.

## Files
- edit: `src/game/traffic.h`, `src/game/traffic.cpp`, `src/script/natives.cpp` (two functions)

## Done when
- `// 0xADDR Name` comments on both Graph methods and both natives; the null-cross guard is
  `INVENTED:`
- `make -C src clean check` green; no unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
