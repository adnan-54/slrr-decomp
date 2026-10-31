---
id: 251
status: blocked
branch:
review:
depends: []
---
# traffic: Traffic__findRoute search + haltPath/haltRoad + GroundRef natives

Promotion note (orchestrator, 2026-10-31): **not promoted — `status: blocked`.** The trace is
good evidence but this is not one card:
- it spans ~12 exe functions (`0x417e50`, `0x419c50`, `0x4167a0`, `0x41a8b0`, `0x41a6f0`,
  `0x4120d0`, `0x414e00`, `0x412960`, `0x4196f0`, `0x419980`, `0x419ab0`, `0x419ba0`, plus
  the two natives) against the §3 limit of ~5;
- the UNRESOLVED items are load-bearing, not peripheral: the **parent-chain rebuild loop**
  (`0x4189b0..0x4189ee`) is how the result path is produced, the end-of-route connector
  branches (`0x418777/0x418834/0x418918`) decide when a route ends, and the `Road::k64` snap
  gate's meaning is unknown — a port with those guessed would silently produce wrong routes;
- nothing can verify it today: no harness calls `haltTrafficPath`/`haltTrafficRoad`, and the
  city probe has no script-driven halt.
To unblock: split into (a) the A* core (`0x417e50` + heap/table `0x41a8b0`/`0x41a6f0`/
`0x4167a0` + heuristic `0x419c50`) with the parent-chain loop traced exactly, then (b) the
path build (`0x4120d0`/`0x414e00`/`0x412960`), then (c) the halt propagation + natives.
Each needs its own trace round; keep this card as the index of what is already known.

## Goal
Port the path search behind the two script natives `GroundRef.haltTrafficPath` and
`GroundRef.haltTrafficRoad`: `Traffic__findRoute` 0x417e50 (A*-style cross/road search,
search state = per-cross g-score at cross+0x3c + local open heap / node table),
`Traffic__haltPath` 0x4196f0 and `Traffic__haltRoad` 0x419ba0, their halt-propagation
helpers 0x419980 / 0x419ab0, the process-wide lane worklist at 0x5e6538/0x5e653c/0x5e6540,
and the two natives 0x4dd700 / 0x4de270 (arg unpack + field reads + call order).
The path-build sub-steps whose asm is still ambiguous are listed under UNRESOLVED; do
not guess them, keep the card `draft` until they are resolved.

## Exe evidence (cleaned pseudocode, renamed fields)

Calling/struct conventions used below (all traced):
- `nearestCross(x,y,z,dist,yflag)` = 0x419050 (already src `Graph::nearest_cross`);
  `nearestLanePos(x,y,z,&d,&lat)` = 0x4191d0 (src `Graph::nearest_lane_pos`);
  `pathProject(path,x,z,&d,&lat)` = 0x414830 (src `traffic::path_project`).
- `Traffic` (`T`) chunk arrays: crossEq at T+0x10, crossLight at T+0x28, roads at
  T+0x40, lanes at T+0x58. Element i = `pages[i/page_size] + elem_size*(i%page_size)`;
  crossEq page/eltsize T+0x1c/T+0x20, crossLight T+0x34/T+0x38, road T+0x4c/T+0x50,
  lane T+0x64/T+0x68. src: `Graph::cross_eq/cross_light/roads/lanes`.
- Cross: links array cross+0x14 (stride 8: {Lane*, u8 flag}), nLinks +0x18; paths array
  cross+0x1c (stride 0x60: u8 a at +0, u8 b at +1), nPaths +0x20; pos +0x24..+0x2c;
  radius +0x34; **route g-score +0x3c** (set to -1.0f here, see writes).
  src `Cross::links/paths/pos/radius`; add `route_g`.
- Road: cross0 +0x8, cross1 +0xc (Cross* after load/init), Path +0x28
  (Path: n +0, segs +4, start +8..+0x10, end +0x14..+0x1c, start_dir +0x20..+0x28,
  end_dir +0x2c..+0x34, len +0x38), k64 +0x64.
  src `Road::node[0]=+0x04, node[1]=+0x08, node[2]=+0x0c`, `Road::path`, `Road::k64`.
- Lane: road node +0x10, k18 +0x18, halted flag +0xc4.
  src `Lane::road`, `Lane::k18`, `Lane::halted`.
- Segment: width +0x3c. T+0x312c = max road width (src `Graph::max_road_width`).

### 0x417e50 Traffic__findRoute (ECX=T; stack: p1x,p1y,p1z,p2x,p2y,p2z, u8 flag1, u8 flag2) -> Path* | 0
`flagN != 0` means "the point is already inside a cross, do not snap onto a lane".
Exactly what 0x4196f0 passes (see below).

```
// --- reset search state (0x41826d loop over crossEq pages, 0x4182a2 over crossLight)
for every Cross C in T.crossEq and T.crossLight: C.g (+0x3c) = -1.0f

// --- snap END point p2 (0x417e91..0x417f42)
c2 = nearestCross(p2, 0, 1);  goal = c2; alt = c2; p2road = 0
if (!flag2 && (lane = nearestLanePos(p2,0,0)) != 0) {
    p2road = lane->road (+0x10)                  // src lane_at(...)->road
    pathProject(p2road->path (+0x28), p2x, p2z, &d2, &lat)
    goal = p2road->cross0 (+0x8)                 // src cross_by_node(road->node[1])
    if (p2road->k64 (+0x64) > 0) { p2road = 0; DAT_5e6530 = 0.0f; }   // DAT_5e6534 stays stale
    else {
        alt = p2road->cross1 (+0xc)              // src cross_by_node(road->node[2])
        DAT_5e6534 = d2
        DAT_5e6530 = p2road->path.len (+0x60) - d2
    }
}
if (goal == 0) { free locals; return 0 }         // 0x417f44
// if flag2 or no lane: goal == alt == c2, p2road == 0, DAT_5e6530 = 0, DAT_5e6534 stale

// --- snap START point p1 (0x417f6b..0x418176)
start = nearestCross(p1, 0, 1); p1road = 0
if (!flag1 && (lane = nearestLanePos(p1,0,0)) != 0) {
    p1road = lane->road
    pathProject(p1road->path, p1x, p1z, &d1, &lat)
    start = p1road->cross0 (+0x8)
    if (p1road->k64 > 0) { p1road = 0; goto push_one_start_node }   // 0x417ff0
}
// same-road fast path (0x418025), only when both sides snapped with road->k64 <= 0
if (p2road != 0 && p2road == p1road) {                            // 0x418025
    // one straight connector then the whole road path (both are 0x4120d0/0x412960)
    if (d2 > d1) { p0 = p2road->cross0 (+0x8) pos; p1 = p2road->path.end; w = last seg width }
    else         { p0 = p2road->cross1 (+0xc) pos; p1 = p2road->path.start; w = segs[0].width }
    Path* P = new Path (0x567a20(0x3c) + ctor 0x411fb0)
    path_add_straight(P, p0, p1, w, 1.0f, 1, 10000.0f, 10000.0f, 0)   // 0x4120d0
    path_append(P, p2road->path, /*reverse=*/d2 > d1)                  // 0x412960
    return P
}
// roads differ: push TWO start nodes onto the open heap (0x418109)
push node {parent = -1, cross = p1road->cross0, road = p1road}, g = p1road->path.len - d1,
     h = heuristic(cross0, goal, alt)                               // 0x419c50
push node {parent = -1, cross = p1road->cross1, road = p1road}, g = d1,
     h = heuristic(cross1, goal, alt)
goto main_loop

// --- no start snap / start road k64>0 (0x418176)
if (start == 0) return 0
if (p2road == 0) goto push_one_start_node
if (start == goal) { P = new Path; path_append(P, p2road->path, p2road->cross0 != goal); return P }
if (start == alt)  { P = new Path; path_append(P, p2road->path, p2road->cross0 != alt ); return P }
push_one_start_node:                            // 0x418226
push node {parent = -1, cross = start, road = 0}, g = 0, h = heuristic(start, goal, alt)

// --- A* main loop (0x4182dd..0x4186b2)
while (heap.count > 0) {
    {g0, h0, ni} = heap[0]; heap_pop()          // 0x41a6f0 (min-heap on g+h)
    node = nodeTable[ni]                        // {parent idx, Cross*, Road*}
    if (node.cross == goal || node.cross == alt) goto found
    prev_dir = dir_of(node.road, node.cross)    // (0,0,0) when node.road == 0
    for (i = 0; i < node.cross->nLinks; i++) {
        lane = node.cross->links[i].lane        // src lane_at(cross.links[i].node)
        Rn   = lane->road (+0x10)               // src road_by_node(lane.road)
        if (Rn == node.road) continue           // 0x4183a9: loop-carried prev-road skip
        if (Rn->k64 (+0x64) > 0) continue       // 0x4183b7
        next_dir = dir_of(Rn, node.cross)
        far = (Rn->cross0 == node.cross) ? Rn->cross1 : Rn->cross0  // 0x4183bd..0x4183c5
        g = g0 + (dot(prev_dir, next_dir) + 1.0f) * 30.0f
              + (T->max_road_width (+0x312c) / Rn->path.segs[0].width (+0x3c) + 4.0f)
                * Rn->path.len * 0.2f
        if (far == goal) g += DAT_5e6530
        else if (far == alt) g += DAT_5e6534
        best = far->g (+0x3c)
        if (best >= -0.5f && g >= best) continue      // 0x4184e6..0x418504
        far->g = g
        h = (far == goal || far == alt) ? 0.0f : heuristic(far, goal, alt)
        idx = nodeTable.push({parent = ni, cross = far, road = Rn})   // 0x4167a0
        heap_push({g, h, idx})                                        // 0x41a8b0
    }
}
free nodeTable; free heap; return 0            // 0x4186b5

found:                                          // 0x4186e7
  P = new Path (0x3c + ctor 0x411fb0)
  // assemble P from the goal/parent chain (0x418729..0x418ad7):
  //   - straight connector (0x4120d0) from the found cross to the p2 road end
  //     (start or end chosen by road->cross0 == cross; width = first/last seg width)
  //   - append the p2 road path with 0x412960 (reverse when needed)
  //   - walk the parent chain and append each node's road path with 0x412960,
  //     reverse = (road->cross0 != node.cross)
  //   - if flag1: final connector node (0x418b0b..0x418b35) from the start cross
  //     to the p1 road start/end, 0x4120d0 with width 1.0f
  //   (exact loop-variable updates: UNRESOLVED, see below)
  free nodeTable; free heap; return P
```

Writes: `Cross::route_g (+0x3c)` for every cross in cross_eq and cross_light (reset to
-1.0f) and for every relaxed cross (g); globals `DAT_005e6530` and `DAT_005e6534`
(0x5e6530/0x5e6534); allocates the returned 0x3c-byte Path. It writes no lane, road or
Traffic field, and no route globals 0x5fe450.. (those belong to the separate
GroundRef.findRoute native 0x4ddc50).

### 0x419c50 heuristic (cdecl, RET 0xc: arg1 node, arg2 targetA, arg3 targetB) -> float
```
dA = |arg1.pos - arg2.pos|; dB = |arg1.pos - arg3.pos|
if (arg2 == arg3) return dA
if (dB + DAT_5e6534 <= dA + DAT_5e6530) return dB + DAT_5e6534
return dA + DAT_5e6530
```
(pos at node+0x24..+0x2c; the two DAT offsets make it "distance to whichever road end".)

### 0x4167a0 node-table push / 0x41a8b0 heap push / 0x41a6f0 heap pop
- 0x4167a0(ECX = vector{data,count,cap}, src: 12-byte entry {i32 parent; Cross* cross; Road* road}):
  grow cap by +0x400 entries (realloc 0xc bytes each), append 12 bytes, return old count.
  Entry = `{parent index (-1 = root), cross, road}`.
- 0x41a8b0(ECX = heap{data,count,cap}, src: 12-byte entry {float g; float h; u32 node}):
  grow by +0x400 entries (0xc bytes each), sift up; key = g+h; parent <= new stops.
- 0x41a6f0(ECX = heap): pop heap[0] (last element moved to 0, sift down choosing the
  smaller child key; `count == 0` after decrement = empty).
Both allocations freed with 0x567950 (free) before return.

### 0x4120d0 straight path segment + 0x414e00 segment build
`0x4120d0(ECX=Path*, p0(3f), p1(3f), w, f1, i1, f2, f3, i2)`:
```
append seg 0x54 bytes (count+1, realloc);
0x414e00(seg, p0, p1): straight (curved +0x38 = 0), p0/p1 stored, m0 = normalize(p1-p0),
    m1 = -m0, len/flat_len = |p1-p0|, width = 1.0f, f1 = 1.0f, i1 = 10000, f2 = 10000, f3 = 0;
then seg.width +0x3c = w, seg.f1 +0x40 = f1, seg.i1 +0x44 = i1, seg.f2 +0x48 = f2,
     seg.f3 +0x4c = f3, seg.i2 +0x50 = i2;
path.len (+0x38) += seg.len; if first seg: path.start (+0x8) = p0, path.start_dir (+0x20)
     = seg.m0, path.end (+0x14) = p1, path.end_dir (+0x2c) = -seg.m1;
else path.end = p1, path.end_dir = -seg.m1.
```
src already has `traffic::path_add_segment` (0x4122b0) and `segment_build_hermite`
(0x414fe0): add a straight-segment sibling for 0x4120d0/0x414e00.

### 0x412960 path append / reverse (ECX=Path* dest, arg1=Path* src, arg2=u8 reverse)
```
if (dest.n == 0) copy src start/end/start_dir/end_dir (swapped and negated when reverse)
else if (|dest.end - src.startEndpoint|^2 > 0.001)   // src.end when reverse
    insert a connector Segment with 0x4122b0(dest.end pos, dest.end dir, src start pos,
    src start dir, w = min(dest last seg width, src first seg width), f1=...);
append all src segments (reversed order/mirrored dirs when reverse);
dest.end/end_dir = src end (or src start when reverse); dest.len += src.len.
```
`0x4122b0` is src `traffic::path_add_segment` (Hermite from (pos,dir) pairs).

### 0x4196f0 Traffic__haltPath (ECX=T; stack p1x,p1y,p1z,p2x,p2y,p2z) -> void
```
for every lane in T.lanes: lane.halted (+0xc4) = 0             // clear ALL halts first
flag2 = 0
if (c2 = nearestCross(p2, 0, 1)) {                             // 0x4196f0..0x41975x
    r = 0.7f * c2->radius (+0x34)                              // 0.7f = 0x3f333333
    dx = c2->pos - p2; if (r*r > dx*dx+dy*dy+dz*dz) flag2 = 1
}
flag1 = 0
if (c1 = nearestCross(p1, 0, 1)) { same test -> flag1 }
P = Traffic__findRoute(T, p1, p2, flag1, flag2)                // 0x417e50
if (P != 0) {
    free global worklist (0x567950(data); data=0; count=0; cap=0)       // 0x5e6538/3c/40
    c = nearestCross(P->start (+0x8..0x10), 0, 1); halt_cross_propagate(c)   // 0x419980
    for (i = 0; i < P->n; i++) {                               // segment p1 at seg+0xc..0x14
        c = nearestCross(seg[i].p1, 0, 1); halt_cross_propagate(c)           // 0x419980
    }
    free(P->segs); free(P);                                    // 0x511140 then 0x56a42f(P,0x3c)
    for (i = 0; i < worklist.count; i++) {                     // count re-read every step
        lane = worklist[i];
        if (lane->halted (+0xc4) == 0) halt_lane_propagate(lane)  // 0x419ab0
    }
}
free global worklist (data/count/cap = 0)                      // 0x5e6538/3c/40
```
Worklist growth: push appends a Lane* at `0x5e6538 + 4*count`; when count == cap,
cap += 0x400 and `realloc(data, cap*4)` (0x5679b0).

### 0x419980 halt_cross_propagate(ECX = Cross C) -> void
```
halt_cross(C, 0.001f, /*all=*/true)                        // 0x40de70(0x3a83126f, 1)
for (i = 0; i < C.nLinks; i++) {
    lane = C.links[i].lane
    lane->halted = 1
    Rn = lane->road (+0x10); cross2 = *(Rn + 0x8 + lane->k18*4)   // other end cross
    for (j = 0; j < cross2->nLinks; j++) {
        if (cross2->links[j].flag == 0) continue
        if (cross2->links[j].lane->halted != 0) continue
        for (k = 0; k < cross2->nPaths; k++)
            if (cross2->paths[k].a == j &&
                cross2->links[cross2->paths[k].b].lane->halted == 0) goto skip_push
        worklist_push(cross2->links[j].lane)
      skip_push: ;
    }
}
```
(src: cross2 = cross_by_node(*this, Rn->node[1 + lane->k18], ...); link lane = lane_at).

### 0x419ab0 halt_lane_propagate(ECX = Lane L) -> void
Same as the inner block of 0x419980, but starts from L's far cross and does NOT call
halt_cross:
```
L->halted = 1
Rn = L->road; cross2 = *(Rn + 0x8 + L->k18*4)
for (j...) { if flag && !lane->halted && no path k with a==j && paths[k].b lane
             not halted: worklist_push(lane) }
```

### 0x419ba0 Traffic__haltRoad (ECX=T; stack p1x,y,z,p2x,y,z) -> void
```
c1 = nearestCross(p1, 0, 0)          // yflag = 0, unchecked pointer use below
c2 = nearestCross(p2, 0, 0)
for (i = 0; i < c1->nLinks; i++)
  for (j = 0; j < c2->nLinks; j++) {
      lane = c1->links[i].lane
      if (lane == c2->links[j].lane && lane->halted == 0) halt_lane_propagate(lane)  // 0x419ab0
  }
```
No halt reset, no halt_cross call, no findRoute. A null c1/c2 dereferences (the exe has
no null check); src must guard (INVENTED guards acceptable, note them).

### 0x4dd700 GroundRef.haltTrafficPath (ctx) -> void
```
Native__getArgs(ctx, 0, &self, &p1, &p2)         // 0x4b1af0: arg0 self, arg1 Vec3, arg2 Vec3
if (p1 == 0 || p2 == 0) return
h = Native__instanceToHandle(self)               // 0x4ecc70
T = Game__getInfo(h, 0x39, 0)                    // 0x47c640
if (T == 0) return
p1x = getFloatField(p1,"x") (0x5a0500); p1y = getFloatField(p1,"y") (0x5a0510);
p1z = getFloatField(p1,"z") (0x5a0534)           // 0x4ec980
p2x = getFloatField(p2,"x") (0x5a0568); p2y = getFloatField(p2,"y") (0x5a0598);
p2z = getFloatField(p2,"z") (0x5a05bc)
Traffic__haltPath(T, p1x,p1y,p1z, p2x,p2y,p2z)   // 0x4196f0
return (void)
```
Note: `self` is not null-checked before 0x4ecc70; the two Vector3 args are.

### 0x4de270 GroundRef.haltTrafficRoad (ctx) -> void
Identical shape, but field strings are 0x5a0788 "x", 0x5a07e0 "y", 0x5a07f0 "z"
(p1) and 0x5a0814 "x", 0x5a081c "y", 0x5a082c "z" (p2), and the tail call is
`Traffic__haltRoad(T, p1, p2)` (0x419ba0). Same arg/nulls/getInfo order.

## Files
- edit: src/game/traffic.h
  - `Cross`: add `float route_g = -1.0f;   // +0x3c findRoute g-score`
  - `Graph`: `Path *find_route(const float p1[3], const float p2[3], bool flag1, bool flag2) const;`
    `void halt_path(const float p1[3], const float p2[3]);`
    `void halt_road(const float p1[3], const float p2[3]);`
  - declare (traffic.cpp can keep file-local): `path_add_straight`, `path_append`,
    `halt_cross_propagate`, `halt_lane_propagate`.
- edit: src/game/traffic.cpp
  - implement find_route (0x417e50), path_add_straight (0x4120d0 + 0x414e00 via a straight
    segment builder), path_append (0x412960), halt_path (0x4196f0), halt_cross_propagate
    (0x419980), halt_lane_propagate (0x419ab0), halt_road (0x419ba0), plus the local
    node-table/heap push/pop (0x4167a0/0x41a8b0/0x41a6f0) and a namespace-scope
    `std::vector<Lane *> halt_worklist;  // exe 0x5e6538/0x5e653c/0x5e6540`.
  - reuse existing `nearest_cross`, `nearest_lane_pos`, `path_project`, `halt_cross`,
    `lane_by_node`, `cross_by_node`, `road_by_node`, `path_add_segment` (0x4122b0).
- edit: src/script/natives.cpp
  - replace the `gr_halt_traffic_path` / `gr_halt_traffic_road` stubs (keep the registry
    rows unchanged): read a[1]/a[2] as Vector3 with `dd_get_f(vm, obj, "x"/"y"/"z")`
    (non-object/null arg -> silent no-op, like the exe); graph =
    `game_rt()->ground_traffic(value_handle(a[0]))`; if graph:
    `t->halt_path(p1,p2)` / `t->halt_road(p1,p2)`; out = `Value::make_null()`.
    The missing-service case: the exe's `getInfo(h,0x39)` returning 0 makes the native a
    silent no-op (void); this matches `ground_traffic()` returning nullptr.
- read only: src/game/traffic.h (struct offsets already cited above).

## UNRESOLVED
- 0x4189b0..0x4189ee parent-chain rebuild: which node index is advanced each iteration is
  not clear from the listing; raw lines:
  `0x4189a5: MOV EAX, dword ptr [EBP + -0x38]; 0x4189b3: MOV ECX, dword ptr [EDI + EAX*4 + 0x8];
   0x4189d7: MOV ESI, dword ptr [EDI + ESI*4]; 0x4189dd: MOV dword ptr [EBP + -0x1c], ESI`
  Do not guess; re-trace before implementing the found-path assembly.
- 0x418777 / 0x418834 / 0x418918 end-of-route connector branches (flag1/flag2 interaction,
  which endpoint of the p2 road is used, when the 0x4120d0 patch at 0x418b0b runs). Raw:
  `0x418837: TEST EAX,EAX; 0x418866: CMP ESI, dword ptr [EDI + ECX*4 + 0x4];
   0x418918: CMP byte ptr [EBP + 0x24], 0x0; 0x418a27: CMP dword ptr [EDX + 0x8], EAX`.
- Meaning of `Road::k64 (+0x64)` in the snap gate (it selects the >0 branch that keeps
  cross+8 and zeroes DAT_5e6530 while leaving DAT_5e6534 stale). Raw:
  `0x417f2c: CMP dword ptr [ECX + 0x64], ESI; 0x417fe4: CMP dword ptr [ECX + 0x64], 0x0`.
  The behaviour above is traced; the field's semantics are not.
- 0x419980/0x419ab0 the `paths[k]` "open path" test uses `path.b` as a link index and reads
  links[path.b].lane.halted; the meaning of a/b beyond that is not traced. Raw:
  `if ((*pbVar3 == uVar5) && (*(char *)(*(int *)(iVar2 + (uint)pbVar3[1] * 8) + 0xc4) == '\0'))`.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- new fields keep their exe offsets in comments (`Cross::route_g // +0x3c`)
- `make -C src check` green; no new unit tests or hand-made expected values
- no guessed path-build/scalar value: the UNRESOLVED items are either resolved by an asm
  re-check in a follow-up card or left explicitly marked
- card frontmatter: status implemented, branch name
