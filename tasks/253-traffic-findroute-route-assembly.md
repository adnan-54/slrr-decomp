---
id: 253
status: implemented
branch: card/253
depends: []
---
# traffic: Traffic__findRoute route assembly (parent chain + end connectors)

## Goal
Implement `Traffic__findRoute` 0x417e50 (`Graph::find_route`) and the two path-build
helpers it needs, `path_add_straight` (0x4120d0 + 0x414e00) and `path_append` (0x412960),
in `src/game/traffic.cpp`. This card resolves the two findRoute pieces that tasks/251
left open: the found-path parent-chain rebuild loop (0x4189b0..0x4189ee) and the
end-of-route connector branches (0x418777 / 0x418834 / 0x418918, plus the fall-through
0x41872e and the post-loop 0x4189f9..0x418b35). The A* search state they read is
described below. (Road::k64's meaning is still unknown but its branch behaviour is
settled; 0x419980/0x419ab0 belong to the halt cards.)
This pass also corrects two tasks/251 claims: the neighbor skip at 0x4183a9 is a
consecutive-link dedupe against the *previous* link's road (register ESI), not against
`Entry.road`; and the width-1.0f segment at 0x418b0b runs p2-arg -> p1-arg (empty-path
fallback), not start cross -> p1 road.
No script native changes: `GroundRef.findRoute`/`getRouteLength` 0x4ddc50 is a separate
function (route globals 0x5fe450) and stays as-is; this function is only reachable from
`Traffic__haltPath` 0x4196f0 / `Traffic__haltRoad` 0x419ba0 (not in this card).

## Exe evidence (cleaned pseudocode, renamed fields)

### Conventions
- `T` = Graph (exe `Traffic*`, ECX on entry). `Cross`/`Road`/`Lane` are the exe pointers;
  src has node ids, so use the existing file-local `lane_at(g,id)`, `road_by_node(g,id)`,
  `cross_by_node(g,id,0)` (traffic.cpp 690/717/733) and compare the returned pointers.
  exe `Road+0x8` = cross0 = src `Road::node[1]`; exe `Road+0xc` = cross1 = `Road::node[2]`
  (see traffic.h line 52); exe `Road+0x28` = `Road::path`; exe `Cross+0x24..0x2c` =
  `Cross::pos`; exe `Cross+0x14` stride 8 = `Cross::links` (`CrossLink.node` is the lane
  node id, `flag` the byte); exe `Lane+0x10` = `Lane::road` (a road node id);
  exe `Cross+0x3c` = new `Cross::route_g`. `nearest_cross` returns `const Cross *`;
  `const_cast` it for the route_g writes (same pattern as `lane_at`, traffic.cpp 701).
  INVENTED guards: `cross_by_node`/`road_by_node`/`lane_at` can return null for a
  zero/out-of-range node id where the exe has a pointer; return false or skip the
  lookup and mark the guard `INVENTED:`, do not otherwise change behaviour.
- `P` = the returned Path (exe allocates 0x3c bytes, zeroes n/segs/len at 0x411fb0).
- `DAT530` = exe global 0x5e6530, `DAT534` = 0x5e6534 (settled, tasks/251).
- `path_append(P, src, reverse)` = 0x412960; `path_add_straight(P,p0,p1,w,f1,i1,f2,f3,i2)`
  = 0x4120d0 + 0x414e00.
- Float constants (verified bytes):
  `0x3f800000` 1.0f, `0x461c4000` 10000.0f, `0xbf800000` -1.0f,
  `0x593744` -0.5f, `0x593598` 1.0f, `0x593758` 4.0f, `0x593838` 0.2f,
  `0x593858` 30.0f, `0x593570` = sign mask 0x80000000 (XORPS negates a vector),
  `0x593744` is the only compare threshold in the relaxed-cross test.
- Every connector below calls
  `path_add_straight(P, p0, p1, w, 1.0f, 1, 10000.0f, 10000.0f, 0)`:
  stack operand order verified by hand (the vector pushed last is p0, the 0x14-block
  vector is p1; width sits at [esp+0xc] of the 0x14 block).

### A* state the rebuilt regions touch
There is **no closed list**. State = three things:
1. `node_table` (exe EBP-0x30 data, -0x2c count, -0x28 cap): append-only vector of
   12-byte `Entry { int32 parent; Cross *cross; Road *road; }`. `Entry.parent == -1`
   marks a start node. Push helper 0x4167a0 appends the 12 bytes and grows cap by 0x400
   (realloc `cap*12` via 0x5679b0); returns the old count (index). The main-loop relaxation
   inlines the same append (grow at 0x418631, store at 0x418658..0x41866e:
   `{parent = popped index, cross = far, road = Rn}`).
2. `open_heap` (exe EBP-0x74 data, -0x70 count, -0x6c cap): min-heap of
   12-byte `{ float g; float h; uint32 node_index; }`; ordering key is `g+h`
   (0x41a8b0 push, 0x41a6f0 pop; both grow by 0x400 entries; no decrease-key,
   stale entries are popped and processed again). Pop reads heap[0] then removes it
   (last element moved to 0, sift down): at 0x4182f0 it copies `g,h` to EBP-0xB4 and
   `node_index` to EBP-0x38; the popped `g` is the value used at 0x4184ab.
3. `Cross::route_g` (+0x3c) is the g-score / visited mark. Reset to -1.0f for every cross
   of `cross_eq` then `cross_light` (0x41826d..0x4182dd loops, BEFORE the main loop and
   AFTER the start nodes are pushed). Relaxation test at 0x4184e6..0x418504:
   ```
   if (-0.5f > far->route_g || g_new < far->route_g) { far->route_g = g_new; push; }
   ```
   (`COMISS [0x593744], route_g; JA`, else `COMISS route_g, g_new; JBE skip`).
   `h` is never stored on the Cross; `g+h` only orders the heap.

### 0x417e50 setup and A* search (settled, tasks/251; reproduced so the card is usable)
`bool find_route(const float p1[3], const float p2[3], bool flag1, bool flag2, Path *out)`
(ECX=T; stack p1x,p1y,p1z,p2x,p2y,p2z,u8 flag1,u8 flag2; RET 0x20; returns Path*|0).
```
c2 = nearest_cross(p2, 0.0f, 1);  goal = alt = c2;  p2road = 0;
if (!flag2 && (l = nearest_lane_pos(p2, nullptr, nullptr)) != 0) {   // 0x417ee2..0x417f35
    p2road = road_by_node(l->road);                                  // lane+0x10
    path_project(p2road->path, p2x, p2z, &d2, &lat);                 // 0x414830
    goal = cross_by_node(p2road->node[1]);                           // road+0x8
    if (p2road->k64 > 0) { p2road = 0; DAT530 = 0.0f; }              // 0x417f2c..0x417f38
    else { alt = cross_by_node(p2road->node[2]);                     // road+0xc
           DAT534 = d2; DAT530 = p2road->path.length - d2; }         // 0x417ffc..0x418020
}
else DAT530 = 0.0f;
if (goal == 0) return false;                                        // 0x417f44 -> 0x4186b5
start = nearest_cross(p1, 0.0f, 1);  p1road = 0;
lane1 = (!flag1) ? nearest_lane_pos(p1, nullptr, nullptr) : 0;
if (lane1 != 0) {
    p1road = road_by_node(lane1->road);                              // 0x417fc2..0x417fcf
    path_project(p1road->path, p1x, p1z, &d1, &lat);                 // 0x414830, d1 at [EBP-0x5c]
    start = cross_by_node(p1road->node[1]);                          // [EBP-0x3c]
    if (p1road->k64 <= 0) {                                          // 0x417fe4..0x417fee
        // same-road fast path (only here; p2road may be 0, then != p1road):
        if (p2road != 0 && p2road == p1road) {                       // 0x418025
            if (d2 > d1) { add_straight(P, p2road->node[1] pos, p2road->path.end,
                                        p2road->path.segs.back().width, ...);
                           path_append(P, p2road->path, /*reverse=*/true); }
            else         { add_straight(P, p2road->node[2] pos, p2road->path.start,
                                        p2road->path.segs[0].width, ...);
                           path_append(P, p2road->path, /*reverse=*/false); }
            *out = P; return true;                                   // 0x418104
        }
        goto two_start_nodes;                                        // 0x418028 -> 0x418109
    }
    p1road = 0;                                                      // 0x417ff0 (start stays cross0)
}
// start branch: no p1 lane, or p1road->k64 > 0    (0x418173 / 0x418176)
if (start == 0) return false;                                        // 0x418176 -> 0x4186b5
if (p2road == 0) goto push_one_start;                                // 0x41817e -> 0x418226
if (start == goal) { P; path_append(P, p2road->path, p2road->node[1] != goal); *out=P; return true; }
if (start == alt)  { P; path_append(P, p2road->path, p2road->node[1] != alt ); *out=P; return true; }
goto push_one_start;                                                 // 0x4181d9 -> 0x418229
push_one_start:                                                      // 0x418226
    node_table += {parent=-1, cross=start, road=0};  g=0;
    h = heuristic(start, goal, alt);                                 // 0x419c50
    open_heap_push({g, h, index}); goto main_loop;
two_start_nodes:                                                     // 0x418109
    // p1road != 0 here; p2road may be 0 (then goal == alt)
    node_table += {parent=-1, cross=p1road->node[1], road=p1road}; g=p1road->path.length-d1;
    open_heap_push({g, heuristic(cross0,goal,alt), index});          // 0x418139..0x418157
    node_table += {parent=-1, cross=p1road->node[2], road=p1road}; g=d1;
    open_heap_push({g, heuristic(cross1,goal,alt), index});          // 0x418244..0x41825e
// reset route_g of all cross_eq and cross_light to -1.0f   (0x41826d / 0x4182a2)

main_loop: while (open_heap.count > 0) {                             // 0x4182dd..0x4186b2
    {g_cur, h_cur, nidx} = open_heap_top(); open_heap_pop();
    E = node_table[nidx];
    if (E.cross == goal || E.cross == alt) goto found;               // 0x41831c/0x418325
    prev_dir = (0,0,0);
    if (E.road != 0)
        prev_dir = (E.road->node[1] == E.cross) ? E.road->path.end_dir
                                                : -E.road->path.start_dir;
    prev_road = 0;                                                   // ESI, 0x41838f
    for (i = 0; i < E.cross->links.size(); i++) {                    // 0x4183a0
        Lane *l = lane_at(E.cross->links[i].node);
        Road *Rn = road_by_node(l->road);
        if (Rn == prev_road) continue;                               // 0x4183a9 (dedupe)
        prev_road = Rn;                                              // 0x4183b1
        if (Rn->k64 > 0) continue;                                   // 0x4183b7
        next_dir = (Rn->node[1] == E.cross) ? Rn->path.end_dir : -Rn->path.start_dir;
        far = (Rn->node[1] == E.cross) ? cross_by_node(Rn->node[2])
                                       : cross_by_node(Rn->node[1]);
        is_new = (far != goal && far != alt);                        // AL = 1
        g_new = (dot(prev_dir,next_dir) + 1.0f) * 30.0f;             // float order below
        g_new = g_new + g_cur;
        g_new = g_new + (((T.max_road_width / Rn->path.segs[0].width) + 4.0f)
                         * Rn->path.length * 0.2f);
        if (far == goal)      g_new = g_new + DAT530;                // 0x4184c4..0x4184df
        else if (far == alt)  g_new = g_new + DAT534;
        if (!(-0.5f > far->route_g || g_new < far->route_g)) continue;
        far->route_g = g_new;                                        // 0x418504
        h = is_new ? heuristic(far, goal, alt) : 0.0f;               // inlined 0x418511
        idx = node_table.count;                                      // inline append
        node_table += {parent = nidx, cross = far, road = Rn};
        open_heap_push({g_new, h, idx});
    }
}
free node_table; free open_heap; return false;                       // 0x4186b5
```
Float-op order for `g_new` must be kept exactly as written (0x41848e..0x4184bb):
`((dot + 1.0f) * 30.0f) + g_cur + (((max_road_width / segs[0].width) + 4.0f) * path.length * 0.2f)`.
`heuristic(node, goal, alt)` (0x419c50, same body inlined at 0x418511..0x418626):
```
dA = |node.pos - goal.pos|;
if (goal == alt) return dA;
dB = |node.pos - alt.pos|;
return (dB + DAT534 <= dA + DAT530) ? (dB + DAT534) : (dA + DAT530);
```

### NEW: found entry, 0x4186e7..0x41872c
At `found`, register state from 0x4182fa/0x418310: `ESI` = found node index,
`EDI` = goal, `[EBP-0x18]` = alt, `[EBP-0x14]` = p2road, `[EBP-0x34]` = p1road,
`[EBP-0x3c]` = start cross, `[EBP-0x10]` = T until overwritten by P below,
`[EBP+0x20]` = flag1, `[EBP+0x24]` = flag2 (arg slots are reused as scratch only on
branches that return immediately; on this path they still hold the two flags).
```
0x4186e7  P = new Path (0x3c; ctor 0x411fb0)
0x418703  EDX = P; [EBP-0x10] = P
0x41870f  ECX = found*3;  ESI = node_table.data   // CMP byte [EBP+0x24],0 at 0x41870f
0x41871d  parent = node_table[found].parent;  [EBP-0x1c] = parent
0x418723  if (flag2 == 0) goto B_0x418834
0x418729  if (parent == -1) goto C_0x418777
          goto D_0x41872e
```
Note `[EBP-0x1c]` is repurposed here from d1 to `parent`, then advanced by the loop.
The three branch targets are the ones below; all emit into the same `P`.

### NEW: branch B, 0x418834 — flag2 == 0 (p2 snapped to a lane, or no p2 lane)
```
0x418834  EAX = p2road ([EBP-0x14]);
0x418837  if (p2road == 0) goto E_0x418918;
0x41883f  EDI = node_table.data;
0x418842  ESI = p2road->node[1];                       // cross0
0x418866  if (ESI == node_table[found].cross) {        // CMP ESI,[EDI+ECX*4+4], ECX=found*3
0x4188b3      // cross0 == found_cross
              w = p2road->path.segs[0].width;          // [segs+0x3c]
              add_straight(P, p0 = cross_by_node(p2road->node[2])->pos,   // cross1
                              p1 = p2road->path.start, w, ...);
          } else {
0x41886c      // cross0 != found_cross (found is at cross1)
              w = p2road->path.segs.back().width;      // [segs + n*0x54 - 0x18]
              add_straight(P, p0 = cross_by_node(p2road->node[1])->pos,   // cross0
                              p1 = p2road->path.end, w, ...);
          }
0x4188f3  reverse = (p2road->node[1] != node_table[found].cross);        // SETNZ
0x41890e  path_append(P, p2road->path, reverse);
0x418913  goto CHAIN (0x41899b) with parent = node_table[found].parent;  // [EBP-0x1c]
```
So the p2-road piece is appended so that it *ends* at the found cross; the
`add_straight` connector bridges the other cross' centre to the corresponding path
endpoint (call it with p0 = cross centre, p1 = path endpoint).

### NEW: branch C, 0x418777 — flag2 != 0 and found == start node (parent == -1)
```
0x418777  if (p1road == 0) goto B_0x418834;            // [EBP-0x34]
0x418781  EDX = p1road;  EDI = goal (still)
0x4187a8  if (p1road->node[1] == goal) {                // CMP [EDX+8],EDI
0x4187ee      w = p1road->path.segs.back().width;
              add_straight(P, p0 = goal->pos, p1 = p1road->path.end, w, ...);
          } else {
0x4187ad      w = p1road->path.segs[0].width;
              add_straight(P, p0 = goal->pos, p1 = p1road->path.start, w, ...);
          }
0x4187e7  EDX = 0;  goto POST (0x4189f6)
```
`goal == alt` here (flag2 != 0 means p2road == 0, so alt stays c2 = goal). The
gate picks the endpoint on the goal side; the post-loop then appends p1road so that
it *starts* at the same cross (continuity checked: found == goal because found is a
start node whose cross equals goal/alt).

### NEW: branch D, 0x41872e — flag2 != 0, found is not a start node
```
0x41872e  ESI = node_table[found].road;                // no null test (safe: relaxed nodes
0x418737  EAX = ESI->path.segs;                        //        always store a road)
0x418756  if (ESI->node[1] == goal) {                  // CMP [ESI+8],EDI
0x41895c      w = segs[n-1].width;  p1 = ESI->path.end;
          } else {
0x41875f      w = segs[0].width;    p1 = ESI->path.start;
          }
          add_straight(P, p0 = goal->pos, p1, w, ...);
0x418998  EDI = node_table.data;  goto CHAIN (0x4189a0) with parent = [EBP-0x1c]
```
No p2-road piece is appended in this branch (flag2 means p2road == 0).

### NEW: branch E, 0x418918 — no p2 road (p2road == 0)
Reached from B (flag2 == 0 and p2road == 0) or from C's `p1road == 0` fall-through.
```
0x418918  if (flag2 != 0) goto CHAIN_SETUP (0x418998);       // CMP byte [EBP+0x24],0
0x41891e  ESI = node_table[found].road;                      // ESI was node_table.data
0x418922  if (ESI == 0) { ESI = p1road; if (ESI == 0) goto CHAIN_SETUP; }  // 0x418926
0x418953  if (ESI->node[1] == goal) {                        // CMP [ESI+8],EDI
0x41895c      w = segs[n-1].width;  p1 = ESI->path.end;
          } else {
0x41875f      w = segs[0].width;    p1 = ESI->path.start;
          }
          add_straight(P, p0 = goal->pos, p1, w, ...);
0x418993  goto CHAIN_SETUP (0x418998) with parent = [EBP-0x1c]
```
`goal == alt` here (p2road == 0), and found cross == goal/alt. `node_table[found].road`
is null only for the single start node, which has parent == -1 (loop then skips).

### NEW: parent-chain rebuild loop, 0x4189b0..0x4189ee
Preconditions at 0x418998/0x41899b (or 0x41899b from B) and 0x4189a0:
`EDI` = node_table.data, `ESI` = parent = node_table[found].parent (EBP-0x1c),
`ECX` = P (EBP-0x10). Registers: EAX = *current node index*, ESI = its parent index,
`[EBP+0x24]` = scratch holding the last appended road (arg slot is dead here).
```
0x41899e  EDX = 0                              // last_road = 0
0x4189a0  if (ESI == -1) goto 0x4189f0         // root: skip loop
0x4189a5  EAX = [EBP-0x38]                     // current = found node index
0x4189b0  LEA EAX,[EAX+EAX*2]                  // loop: EAX = current*3
0x4189b3  ECX = node_table.data[EAX*4 + 8]     // road(cur)
0x4189b7  LEA ESI,[ESI+ESI*2]                  // ESI = parent*3
0x4189ba  [EBP+0x24] = ECX                     // last_road = road(cur)
0x4189bd  EAX = ECX->node[1]                   // road(cur)->cross0 (exe +0x8)
0x4189c0  CMP EAX, node_table.data[ESI*4 + 4]  // vs entry[parent].cross
0x4189c4  AL = SETNZ                           // reverse = (road0 != parent cross)
0x4189ca  PUSH AL
0x4189cb  LEA EAX,[ECX+0x28]                   // &road(cur)->path
0x4189ce  ECX = [EBP-0x10]                     // P
0x4189d1  PUSH EAX
0x4189d2  path_append(P, road(cur)->path, reverse)
0x4189d7  ESI = node_table.data[ESI*4]         // ESI = entry[parent].parent (grand)
0x4189da  EAX = [EBP-0x1c]                     // EAX = parent (the node just consumed)
0x4189dd  [EBP-0x1c] = ESI                     // parent = grand
0x4189e0  if (ESI != -1) goto 0x4189b0
0x4189e5  ESI = node_table.data
0x4189e8  EDI = P
0x4189eb  EDX = [EBP+0x24]                     // last_road
0x4189ee  goto 0x4189f9
```
Semantics: `cur` starts at the found node and walks `parent` links; each iteration
appends `node_table[cur].road->path` so that the piece **ends at**
`node_table[parent].cross` (`reverse = road->node[1] != parent.cross`). The loop stops
with `cur` = the root node (parent == -1); **the root node's own road is NOT appended**
(root is a cross; it has no incoming road). At exit EAX = root node index (for the
post-loop reverse test), EDX = road of the last appended piece (0 when the loop was
skipped: found itself was the root).

### NEW: post-loop, 0x4189f9..0x418ab4
```
0x4189f9  if (flag1 != 0 && EDX != 0) {
0x418a0b      start_cross = [EBP-0x3c];
0x418a27      if (last_road->node[1] == start_cross) {      // CMP [EDX+8],EAX
0x418a73          w = last_road->path.segs[n-1].width;  p1 = last_road->path.end;
              } else {
0x418a37          w = last_road->path.segs[0].width;    p1 = last_road->path.start;
              }
              add_straight(P, p0 = start_cross->pos, p1, w, ...);   // 0x418974..0x418993
          }
          else if (p1road != 0) {                                   // 0x418ab4
0x418abb      root = EAX;                                            // root node index
0x418ac1      reverse = (p1road->node[1] == node_table[root].cross); // SETZ
0x418ad2      path_append(P, p1road->path, reverse);
          }
```
Different rule from the chain loop: the terminal p1-road piece is appended so that it
**starts at** the root cross (SETZ, not SETNZ). With flag1 != 0 p1road is always 0, so
the two arms are mutually exclusive. `EAX` is the root node index in both cases
(loop-skip path sets it from `[EBP-0x38]` at 0x4189f6).

### NEW: empty-path fallback, 0x418ad7..0x418b35
```
0x418ad7  if (P->count == 0) {                             // CMP [EDI],0
0x418ae0      // p0 = the second Vector3 argument (EBP+0x14 = p2), width = 1.0f,
              // p1 = the first Vector3 argument (EBP+0x8 = p1),      f1 = 1.0f
              add_straight(P, p0 = p2arg, p1 = p1arg, w = 1.0f, 1.0f,
                           1, 10000.0f, 10000.0f, 0);
          }
0x4186b7  return P
```
This corrects tasks/251 line 151: the 0x418b0b width-1.0f segment runs from the
original p2 argument to the original p1 argument (operand order verified from the
stack layout), not from the start cross to a p1 road end.

### Whole-function control summary (for the implementer)
```
P = new Path
if flag2 == 0:
    p2-piece: emit cross->path connector at the non-found cross, append p2road path
              ending at found_cross ; goto CHAIN
else if parent == -1:
    if p1road: emit goal->p1road connector ; EDX = 0 ; goto POST
    else: goto B
else:
    emit goal->found_road connector ; goto CHAIN_SETUP
// (branch E when B sees p2road == 0)
CHAIN_SETUP: parent = node_table[found].parent ; EDI = table.data
CHAIN: append roads found..(child of root), each ending at its parent cross
POST:  if flag1 && last_road: connector start_cross->last_road endpoint
       else if p1road: append p1road starting at root cross
       if P empty: add_straight(p0=p2arg, p1=p1arg, width=1.0f, f1=1.0f)
       return P
```

### Helper 0x4120d0 + 0x414e00 (straight add; settled, tasks/251)
`path_add_straight(P, p0, p1, w, f1, i1, f2, f3, i2)`: append one 0x54-byte Segment
built straight from p0 to p1: store p0/p1; `dir = normalize(p1-p0)`; `m0 = dir`,
`m1 = -dir`; `len = flat_len = |p1-p0|`; `curved = false`; then set
`width(+0x3c)=w, k40(+0x40)=f1, i1(+0x44)=i1, f2(+0x48)=f2, f3(+0x4c)=f3, i2(+0x50)=i2`.
Path update: `length(+0x38) += seg.len`; first segment sets `start=p0`, `start_dir=m0`;
always sets `end=p1`, `end_dir=-m0` (= m1). src Segment fields match (traffic.h 19-29).
If `|p1-p0| == 0` the exe normalizes a zero vector; do not invent a different value.

### Helper 0x412960 (path append/reverse; settled, tasks/251)
`path_append(P, src, reverse)` (never early-returns; the segment copy always runs):
- if `P.count != 0` and the squared 3D distance between `P.end` and the src endpoint
  to attach (`src.end` when reverse, else `src.start`) is `> 0.001`: insert a connector
  with `path_add_segment` (0x4122b0, src `traffic::path_add_segment`) from
  `P.end/P.end_dir` to that src endpoint (endpoint dir negated when reverse),
  `w = min(P last seg width(+0x3c), src first seg width(+0x3c))`,
  `f1 = min(P last seg k40(+0x40), src first seg k40(+0x40))`, then i1=1,
  f2=10000.0f, f3=10000.0f, i2=0.
- grow `P.segs` by `src.segs.size()` and copy all src segments, forward or in reverse
  order. Reverse copy: p0 = old p1, p1 = old p0; curved flag copied; if the src seg is
  not curved `m0=-old.m0, m1=-old.m1` (= swapped because a straight seg has
  m1=-m0), else `m0=-old.m1, m1=-old.m0`; width/k40/i1/f2/f3/i2 copied.
- then set `P.end = src.end` and `P.end_dir = src.end_dir` (when reverse: `P.end =
  src.start` and `P.end_dir = -src.start_dir`) and `P.length += src.length`.
  `P.start/start_dir` are only written when `P.count == 0` (head pre-copy above).
Use tasks/251 §0x412960 for the per-field reverse copy (already traced there).

### Helpers 0x4167a0 / 0x41a8b0 / 0x41a6f0 (settled, tasks/251)
Node-table push appends 12 bytes `{parent,cross,road}` and returns the old count;
heap push/pop as described in the state section (min-heap on g+h, pop moves the last
element to the root). They are small local helpers; no global state in src.

## Files
- edit: `src/game/traffic.h`
  - `struct Cross`: add `float route_g = -1.0f;  // +0x3c findRoute g-score (0x417e50)`
  - `struct Graph`: add `bool find_route(const float p1[3], const float p2[3], bool flag1,
    bool flag2, Path *out);` (non-const: it writes `Cross::route_g`; return false for the
    exe's null return at 0x4186b5).
- edit: `src/game/traffic.cpp`
  - `Graph::find_route` (0x417e50), reusing `nearest_cross`, `nearest_lane_pos`,
    `path_project`, `lane_at`, `road_by_node`, `cross_by_node`.
  - file-local `path_add_straight` (0x4120d0 + 0x414e00) and `path_append` (0x412960);
    reuse `traffic::path_add_segment` (0x4122b0) for the append connector.
  - local node-table / open-heap structs and push/pop inside find_route.
  - exe globals 0x5e6530/0x5e6534 become file-local `static float` (process-wide, BSS),
    not function locals: DAT530 is always assigned before A*, but DAT534 is only
    assigned when a p2 lane was snapped with `p2road->k64 <= 0`; every other call path
    reuses the previous call's DAT534 (tasks/251 documents this staleness).
- No script native edits. Do not touch the `GroundRef.findRoute` stub.

## Done when
- each ported function has a `// 0xADDR Name` comment (`0x417e50`, `0x4120d0`, `0x412960`)
  and the parent-chain loop / connector branches keep their exe addresses in comments
- exact constants and float-op order from this card are kept; no invented formulas
- `Cross::route_g` keeps the `+0x3c` offset comment; node-table/heap layouts keep
  `{parent,cross,road}` / `{g,h,node}` comments
- `make -C src check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
