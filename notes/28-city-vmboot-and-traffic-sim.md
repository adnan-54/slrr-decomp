# 28 — Card 210 review, vmboot City entry, per-frame traffic sim (RE 2026-10-25)

## 1. Card 210 review (commit 50471fc, fix d719eca)
Checked against Ghidra pseudocode/asm of every cited function.

| function | verdict |
|---|---|
| 0x419da0 Traffic__init | matches (road_enabled: signed max of road+0x64, `[0]=0`, `[1..m]=1`) |
| 0x415650 / 0x40e0d0 cross init | **bug fixed**: `cross+0x58` is `realloc(cross+0x20 * 8)` and zeroed in the *paths* loop (`+0x1c` paths, stride 0x60, count `+0x20`); links are `+0x14`/`+0x18`. src sized it per link. Renamed `Cross::path_state`, sized `paths.size()`; notes/27 §1 corrected. Radius (strict `<`, endpoint `lane+0x1c + flag*0xc`) and cycle math match. |
| 0x415700 light init | matches (cnt/prev logic, `+0x74 = cnt+1`, `+0x5c = 0`) |
| 0x40e260 road init | matches. asm 0x40e2b0..0x40e32d: `XMM1 = 1.0 [0x593598]`; pass 1 `XMM1 *= 1.1 [0x593714]` per matching lane; pass 2 per matching lane `XMM1 *= 0.9090909 [0x593710]` (0x40e312) then `lane+0x98 = lane+0x98 * XMM1` (0x40e316). So the factor is the successive product, as src does: with k matching lanes the i-th (1-based) gets `1.1^k * 0.9090909^i`, i.e. ~`1.1^(k-i)`: first lane fastest, last ~1.0. Lane count `n` via the two CMOVZ decrements (0x40e272..0x40e28f) matches; `w = (float)n * 1.75 [0x59371c]`; 0x415440 writes `seg+0x3c` for every 0x54-byte segment. |
| 0x40e150 lane init | no-car branch matches (`+0xc8=0`, `+0xc0=T+0x84`, `+0xcc=lane+0x74` = path(+0x3c)+0x38 length, `+0xc4=0`) |
| runtime.cpp load_ground | init after load, then `register_callback(h, 0x1c)`: fine (lazy creation is the existing INVENTED shift) |

## 2. How vmboot enters the City
Shipped paths into `Valocity` (extends City, extends Track; Valocity.java:41 sets `Integrator.isCity = 1`):
- **Garage `CMD_HITTHESTREET` = 109** "Go driving in the city" (Garage.java:74, 536, 1693-1700):
  needs `player.car` and `player.car.isDriveable() == null` -> `changeActiveSection(new Valocity())`.
- MainMenu `CMD_QUICKRACE` 0xA009 (MainMenu.java:40, 471-497): quick race, makes a VS_DEMO car,
  City.enter then builds a race bot (`createQuickRaceBot`), more scope.
- GameLogic.java:410-421 `quickrace` debug flag (0 in shipped code).

vmboot already has everything the Garage path needs: it buys a used car (isDriveable null is
checked) and is back in the Garage at frame 18300, where it clicks garage cmd 133 (EventList).
**Experiment** (scratch, reverted): sending 109 instead of 133 at that frame, `vmboot --selfcheck`
(ulimit -v 1500000, timeout 300; 1m54 wall):
- `actualState` -> `java.game.Valocity`, its thread parked in `GameType.run` line 157, 0 failed threads.
- the ground's `maps\city\ai\city.trc` loads on first `ground_traffic` lookup and Traffic__init runs:
  eq 53, light 218, roads 484, lanes 1572 (= resman's city.rdb road-kind check 484/53/218),
  `max_road_width` 10.5, `sim_time` 288.76.
- career daytime branch (Valocity.java:76-170): **19 `addTrafficN` calls and 45 `addTrafficCar`
  calls** (Valocity.java:163, speedymen bots' `dummycar`). Both are SERVICE MISSING stubs returning 0.
- the race-only checks after the loop all FAIL (expected: they assume Track/JuniorCurcuit).

So nothing blocks *entering* the City: the only change is the Garage command. What is missing is
a harness mode that stops after the City checks (card 211).

**Correction to notes/27 §0**: `addTrafficCar` *is* reached from shipped scripts: Valocity.java:163
(career, day, every speedyman bot with a dummycar, `pos = getNearestCross(nrData.startPos[idx]) * rnd*20`),
followed by `notifyTrafficCar(id, 1)`. Bot.joinTraffic is still uncalled.

## 3. Per-frame traffic sim: who calls it, what it does
`Game__mainLoop` 0x47d760, once per rendered frame, after the 16 ms control gate (0x47e050 physics,
`Game__runTimers`) and the 8 ms VM gate: `Game__runAnimateCallbacks` **0x47ba10**`(min(frame_dt, *0x5da468))`,
then `Game__flushDeferred` 0x47d070. src `FrameLoop::frame` (src/app/boot.cpp) has no animate walk.

### 0x47ba10 Game__runAnimateCallbacks(dt) (ECX = Game)
```
for node in list game+0xa0 (mode 0x1c, appended by 0x47d670 / 0x47d560):
    if (node+0x2c == 0) { node->vf0(1) /*delete*/; game+0x21c--; continue; }   // dead handle
    inst = node+0x30; if (!inst || inst+0x7c != 1) continue;                     // not resident
    typed = inst.typed(0xa0000000)                                               // 0x4ebdd0/0x4e8860(0x100) save/restore around
    if (typed+0x4c == 0) { if (typed+0x50) callScriptMethod(typed, "animate", 0, 0x4f); }
    else { T = (typed+0x44).typed(0x80000000); if (T+0xc) T+0xc->vf13(typed+0x4c, dt); }
```
### 0x4bac10 type_ground vf13(ground)
```
if (ground+0x8c /*pedestrians*/) { 0x41cc20(); per ped-object loop ...; 0x41ed70(*0x5dadf0, 1); }   // not traced
if (ground+0x88 /*Traffic T*/) {
    0x56a230(3) /*profiler start*/;
    0x41a9d0(T);          // reap: list T+0x98, entry+0xc == 0 -> 0x40f1e0 free, move to free list T+0xac
    0x41b290(T, *0x5dadf0 /*world time*/);
    0x417c90(T, 1, camera count *0x5f1e44 > 0 ? &camera_pos 0x5fe110 : null);
    ground+0x90 += (0x56a260(3) - ground+0x90) * 0.1;   // profiler average, not behaviour
}
```
### 0x41b290 Traffic step(t)
```
g_traffic = T; T+0x84 (sim_time) = t;
if (T+0x3118 player pseudo-car) its +0xe0/+0xe4/+0xe8 = t + 1.0;
while (T+4 /*heap count*/ && *(float*)T[0] /*top time*/ < t) { 0x40fcf0(); 0x41a810(); }  // timed events
walk 4 intrusive active lists, unlink a node when its step returns false:
   T+0x90 cross_eq (0x415c40, next +0x54), T+0x94 cross_light (0x415d60, next +0x60),
   T+0x88 roads (0x40f350, next +0x44),   T+0x8c lanes (0x40f4e0, next +0xa4)    // sentinel = T
drain T+0xc8 car list (next +0x10): if (car+0x138) Game__queueEvent(car+0x130, 0, 1, 0, 0)
```
### 0x417c90 Traffic pending spawns(T, flag, cam_pos)
16-byte entries at T+0x310c, count T+0x3110: score each live entry (0x40e6e0) by
`-|car_pos(0x40dde0) - cam|^2` (0 without camera), sort (0x40c270), try 0x40d4e0 in order, keep
the failed ones compacted, zero the spawned ones; count = kept.

### What can run today
src `traffic::Graph` has no event heap, no active lists, no car pool and no spawn queue: their
producers are the car paths (lane_init car branch 0x417050, addTrafficN 0x4dc9e0 ->
ctLane__spawnCar + `create_native(..., "traffic_car")` 0x523c80, addTrafficCar 0x4dd090), none
ported. With the pool empty, the only observable effect of the per-frame step is
`T+0x84 = world time` every frame. That is enough for a walker card (212) whose check is
`Graph::sim_time == process_world().time` after a City run.

Moving cars needs, in order: addTrafficN (needs `traffic_car` native instances of cars/traffic
rpks, type_car getInfo 0x40/0x45/0x46, ctLane__spawnCar), the event heap 0x40fcf0/0x41a810, the
lane/road/cross steps above, and 0x4107c0 (per-car step, ~600 lines). Not carded; next RE.

## 4. Cards
- 211 vmboot `--city`: Garage cmd 109 -> Valocity, City checks, stop.
- 212 Game__runAnimateCallbacks 0x47ba10 in FrameLoop + ground vf13 traffic time step (depends 211).
