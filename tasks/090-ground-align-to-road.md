---
id: 090
status: todo
branch: card/090
depends: [087, 089]
---
# game/script: road projection + GroundRef.alignToRoad

## Goal
Port segment/path projection (0x414480, 0x414830), the lane spatial-index lookup
`Traffic__nearestLanePos` (0x4191d0), `Traffic__alignToRoad` (0x419410) and the native
`alignToRoad(Vector3)Vector3[]` (0x4dd470). Script user: `game/City.java:289-460`
(`alignedPos = map.alignToRoad(pS)` -> `[0]` position snapped to the road, `[1]` road
direction). Trace: notes/19 §2.

## Exe evidence (Claude-traced; 0x414480, 0x4191d0, 0x419410 asm checked)
Constants (bytes checked): 0x593588 double 1e-5, 0x593560 double 0.01, 0x593568 double
0.1, 0x59370c 0.9f, 0x593598 1.0f, 0x593580 0.0f, 0x593590 0.5f, 0x593854 20.0f,
0x593738 5.0f, 0x593570 sign mask.

### 0x414480 ctSegment__project(x, z, float *d, float *lat) -> bool — ECX = segment
    if (!curved) {                                         // 0x414763
        dir = (m0.x, m0.z); n = sqrt(dir.x² + dir.z²) (float); if ((double)n > 1e-5) dir *= 1.0f/n;
        dx = x - p0.x; dz = z - p0.z;
        proj = dir.z*dz + dir.x*dx;
        if (proj < 0.0f) return 0; if (flatLen(+0x34) < proj) return 0;
        *d = proj / flatLen * len;
        *lat = m1.z*dz + m1.x*dx;
        return 1;
    }
    dir = (m0.x, m0.z) normalized as above; nrm = (-m1.x, -m1.z) normalized the same way;
    a = (z - p0.z)*dir.z + (x - p0.x)*dir.x;
    b = (z - p1.z)*nrm.z + (x - p1.x)*nrm.x;
    if (a < 0 || b < 0) return 0;                          // JC: also false on NaN
    *d = len * a / (b + a); prevErr = 0.0f; k = 1.0f;
    pos = ctSegment__pos(*d); dir3 = ctSegment__dir(*d);   // card 089
    err = (x - pos.x)*dir3.x + (z - pos.z)*dir3.z;
    while (!((double)|err| < 0.01)) {
        if (0 > err*prevErr) { k = k * 0.9f; if ((double)k < 0.1) break; }
        old = *d; prevErr = err;
        *d = err*k + old;
        if (*d < 0) { *d = 0; if (old == 0) break; }
        if (*d > len) { *d = len; if (old == len) break; }
        pos = ctSegment__pos(*d); dir3 = ctSegment__dir(*d);
        err = (x - pos.x)*dir3.x + (z - pos.z)*dir3.z;
    }
    side = ctSegment__side(*d);
    *lat = (x - pos.x)*side.x + (z - pos.z)*side.z;        // pos from the last evaluation
    return 1;
### 0x414830 ctPath__project(x, z, float *d, float *lat) -> bool — ECX = path
    best = -1; acc = 0;
    for i < n:
        s = &seg[i];
        if (!ctSegment__project(s, x, z, &sd, &sl)) {
            d0 = (s.p0.z - z)² + (s.p0.x - x)²;  d1 = (s.p1.z - z)² + (s.p1.x - x)²;
            if (d1 <= d0) { sl = sqrt(d1); sd = s.len; } else { sl = sqrt(d0); sd = 0; }
        }
        if (best == -1 || |sl| < |*lat|) { *lat = sl; *d = sd + acc; best = i; }
        acc = s.len + acc;
    return best != -1;
### 0x4191d0 Traffic__nearestLanePos(x, y, z, float *d, float *lat) -> Lane* — ECX = Traffic (RET 0x14, result in EAX)
    lanes = array +0x58 (count +0x5c); entry fields: k78 = index_min_x, k7c = index_run_max_x,
    k80 = index_max_x, k84 = index_lane (card 086).
    lo = 0; hi = n - 1; mid = -1;
    while (lo <= hi) { mid = (lo + hi) / 2;            // signed, truncating
                       if (x + 20 > lanes[mid].k78) lo = mid + 1; else hi = mid - 1; }
    best = 0; bestD2, bestD, bestLat;
    for (i = mid; i >= 0; i--) {
        if (!(lanes[i].k7c > x - 20)) break;
        if (lanes[i].k80 > x - 20) {
            L = &lanes[lanes[i].k84];
            if (ctPath__project(&L.path(+0x3c), x, z, &sd, &sl)) {
                pos = ctPath__posAt(&L.path, sd);       // card 089
                dy = pos.y - y;
                if (5.0f > |dy|) {
                    d2 = (pos.x - x)² + dy² + (pos.z - z)²;      // summed in this order
                    if (best == 0 || bestD2 > d2) { bestD = sd; best = L; bestD2 = d2; bestLat = sl; }
                }
            }
        }
    }
    if (best) { if (d) *d = bestD; if (lat) *lat = bestLat; }
    return best;
### 0x419410 Traffic__alignToRoad(float pos[3] in/out, float dir[3] out or 0) -> bool
    L = Traffic__nearestLanePos(pos.x, pos.y, pos.z, 0, 0); if (!L) return 0;
    R = L->road(+0x10);                                      // Road*, path at R+0x28, length R+0x60
    ctPath__project(&R.path, pos.x, pos.z, &d, &lat);        // result ignored
    if (dir) { v = ctPath__dirAt(&R.path, d);
               dir = (R.path.length * 0.5f > d) ? v : -v; }  // asm 0x419495 COMISS; JBE -> negate
    pos = ctPath__posAt(&R.path, d);
    return 1;
(If the projection fails `d` keeps the stale stack slot; with ≥1 segment it cannot fail.
INVENTED: d = 0 then.)
### 0x4dd470 GroundRef.alignToRoad(Vector3)Vector3[]
    getArgs(ctx, &self, &v);
    if (v == 0) return 0; h = instanceToHandle(self); if (h == 0) return 0;
    t = Game__getInfo(h, 0x39, 0); if (t == 0) return 0;
    p = (v."x" 0x5a25d0, v."y" 0x5a25c8, v."z" 0x5a25c0); dir = <uninitialised locals>;
    Traffic__alignToRoad(t, p, &dir);                       // result ignored
    a = new Vector3(p) ("x","y","z" 0x5a2638/3c/40); b = new Vector3(dir) (0x5a2660/68/94);
    arr = new "[java.lang.Vector3"(2); arr[0] = a; arr[1] = b; return arr;

## Changes
1. `src/game/traffic.h` / `traffic.cpp`:
   ```cpp
   bool segment_project(const Segment &s, float x, float z, float *d, float *lat);  // 0x414480
   bool path_project(const Path &p, float x, float z, float *d, float *lat);        // 0x414830
   // in Graph:
   const Lane *nearest_lane_pos(float x, float y, float z, float *d, float *lat) const;  // 0x4191d0
   bool align_to_road(float pos[3], float dir[3]) const;                               // 0x419410
   ```
   `align_to_road` resolves `L->road` (a node id, card 086) to `roads[id - 1 - n_eq - n_light]`;
   a lane whose `road` id is not a road -> return false (INVENTED guard). The Newton loop
   keeps the double compares (`(double)fabsf(err) < 0.01`, `(double)k < 0.1`) and the
   clamp-break order exactly as above.
2. `src/script/natives.cpp`: replace the card-085 body of `gr_align_to_road`: `want(...,2)`;
   `a[1]` Null -> null; `vec_read`; graph via `ground_traffic(value_handle(a[0]))` (none ->
   null); `float dir[3] = {0, 0, 0};` (INVENTED: the exe leaves it uninitialised when no
   lane is found); `t->align_to_road(p, dir)`; build two `new_point` Vector3s and
   `Array *arr = vm.new_array("Ljava.lang.Vector3;", 2)`, `arr->elems[0/1] = Value::make_obj(...)`,
   `*out = Value::make_array(arr)`.

## Files
- edit: `src/game/traffic.h`, `src/game/traffic.cpp`, `src/script/natives.cpp` (one function)

## Done when
- `// 0xADDR Name` comments on all five; stale-d, uninitialised-dir and the road-id guard
  are `INVENTED:`
- `make -C src clean check` green; no unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
