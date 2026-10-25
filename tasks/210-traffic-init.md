---
id: 210
status: implemented
branch: card/210
depends: []
---
# Traffic: Traffic__init after load + ground animate registration

## Goal
Port the init pass the exe runs right after `Traffic__load` (notes/27 §1): sim time, cross radius
and cycle, light phases, road widths and lane speed factors, road-group enable flags, and the
`Game__registerCallback(ground, 0x1c)` call. Replaces the shortcut comment at
src/game/runtime.cpp:435. No traffic cars (the pool and per-frame step are not ported), so the
lane init takes its no-car branch only.

## Exe evidence (cleaned pseudocode; T = Traffic, ids are src node ids)
Node ids are 1-based into [cross_eq..., cross_light..., roads..., lanes...], 0 = null; a lane id
maps to `lanes[id - 1 - eq - light - roads]`. Where the exe follows a pointer that must be a lane,
skip ids that are not lane ids (`INVENTED:` guard).

### 0x4bade0 type_ground vf3, traffic block (asm 0x4bb2a8..0x4bb2e8)
```
T = new Traffic; ground+0x88 = T; T+0x3130.bind(ground+0xc);   // sink, not needed by this card
Traffic__load(T, buf);
Traffic__init(T, sim_time);           // *0x5dadf0 = process world time
Game__registerCallback(ground, 0x1c);
```
### 0x419da0 Traffic__init(T, t)
```
T+0x84 = t;                                   // Graph::sim_time
if (T+0x3118) {...}                           // player pseudo-car: null after the ctor, skip
T+0x312c = 0;                                 // Graph::max_road_width
for c in cross_eq:    cross_init(c)           // 0x415650
for c in cross_light: light_init(c)           // 0x415700
for r in roads:       road_init(r)            // 0x40e260
for l in lanes:       lane_init(l)            // 0x40e150
m = 0; for r in roads: if ((int)r.k64 > m) m = r.k64;
road_enabled = byte[m+1]; road_enabled[0] = 0; road_enabled[1..m] = 1;   // T+0xc4
```
### 0x415650 cross_init (0x40e0d0 inlined)
```
c.k38 = 0; c.radius = 0;                                  // +0x38, +0x34
for link in c.links:
    L = lane(link.node); e = link.flag ? L.v28 : L.v1c;   // lane +0x28 / +0x1c
    d = |e - c.pos|; if (d > c.radius) c.radius = d;      // strict >
c.path_state = vector<uint64_t>(paths.size(), 0);         // +0x58, nPath*8 zeroed (review fix)
mx = 0; mn = paths[0].c;                                  // u16 path field +2 (INVENTED: no paths -> mn = 0)
for p in c.paths: mx = max(mx, p.c); mn = min(mn, p.c);   // unsigned
mx -= mx % 10; if (mx == (mn / 10) * 10) mx += 10;
c.cycle = mx;                                             // +0x5c
```
### 0x415700 light_init
```
cross_init(c); c.phase_time = T.sim_time;                 // +0x6c
c.phase = vector<int32_t>(links.size()); cnt = 0;         // +0x70
prev = lane(links[0].node).road;                          // lane +0x10
for i: if (!links[i].flag) c.phase[i] = -1;               // prev unchanged
       else { r = lane(links[i].node).road; if (r != prev) cnt++; c.phase[i] = cnt; prev = r; }
c.phase_count = cnt + 1; c.cycle = 0;                     // +0x74, +0x5c
```
### 0x40e260 road_init (read from asm; consts 1.1f, 0.9090909f, 1.75f)
```
n = 0; for (l = lane(road.node[0]); l; l = lane(l.node14)) n += 1 - (l.node88[0] != 0) - (l.node88[1] != 0);
for dir in 0..1:
    f = 1.0f; for l in road lanes: if (l.k18 == dir && !l.node88[0] && !l.node88[1]) f *= 1.1f;
    for l in road lanes: if (same test) { f *= 0.9090909f; l.k98 *= f; }   // k98 is a float
w = (float)n * 1.75f;
for s in road.path.segs: s.width = w;                     // 0x415440, seg +0x3c
if (w > T.max_road_width) T.max_road_width = w;
```
### 0x40e150 lane_init (no-car branch; the car branch calls 0x4107c0, not ported)
```
l.kc8 = 0; l.kc0 = T.sim_time; l.kcc = l.path.length; l.halted = false;   // +0xc8 +0xc0 +0xcc +0xc4
```

## Files
- edit: src/game/traffic.h: `Lane::k98` becomes `float` (+0x98, the exe multiplies it); add
  `Lane::kc0, kc8, kcc` floats; `Cross::radius, k38, cycle` + `path_state`, `phase_time`, `phase`,
  `phase_count`; `Graph::sim_time, max_road_width, road_enabled`; `void init(float t)`.
- edit: src/game/traffic.cpp: read k98 with `r.f32` (same bytes); `Graph::init` + the four
  helpers, each with its `// 0xADDR` comment.
- edit: src/game/runtime.cpp load_ground: after `g->load` call `g->init(process_world().time)`
  then `rt.register_callback(<the ground's handle>, 0x1c)` if a handle to `r` is at hand; if not,
  leave a `shortcut:` line for the registration only. Remove the old shortcut comment.

## Done when
- each ported function has a `// 0xADDR` comment; guards are `INVENTED:`.
- `make -C src clean check` green (the card-086 city.trc check still parses); vmboot output
  unchanged (vmboot never loads a city, notes/27 §0); no new unit tests or hand-made values.
- card frontmatter: status implemented, branch name.
