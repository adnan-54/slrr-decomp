---
id: 085
status: implemented
branch: card/085
depends: []
---
# script: GroundRef service-less natives (traffic, routes, pedestrians, water)

## Goal
Replace the 29 remaining `java.util.resource.GroundRef` stub rows (all except `setFog`,
card 072) with ports of the exe's **no-service path**: src has no native ground object
(`type_ground`, notes/19 §1), so the exe gate `Game__getInfo(self, 0x39 / 0x3d)` is 0 and
each native returns the value below. Water needs the physics world, which src does not
own process-wide; those three are no-ops. Later cards (087+) replace individual natives
once the traffic graph exists. Full trace: `notes/19-ground-traffic.md` §5.
Only edit the GroundRef rows and add new functions; other cards (SFX 081-084, round-7
Text/RenderRef/System) edit `natives.cpp` concurrently.

Card-size note: 29 natives exceed the usual ≤5 exe functions, but every one is the same
shape (getArgs, gate, constant result); the only logic is listed per native below.

## Exe evidence (Claude-traced; decompile of every native in notes/19 §5, asm checked
for 0x4ddb50, 0x4dd880, 0x4ddc30)
Mapping to src: `Native__getArgs` (0x4b1af0) = `want()` + `get_*`; `Native__instanceToHandle`
(0x4ecc70) = `value_handle(a[0])`; `Instance__setFloatField` (0x4ed080) = `write_f` /
`vec_write`; result Vector3 objects = `new_point(vm, "java.lang.Vector3", v, false, err)`.

### The gate — 0x47c640 Game__getInfo(handle, id, out) (ECX = 0x5fde90)
    if (handle->id == 0) return 0;
    RI = handle->RI; if (RI->typeof == 1) { typed = getTyped(RI, 0xa0000000); return typed ? GameType__getInfo(typed, id, out) : 0; }
    ...
    // GameType__getInfo 0x47e450: native handler (typed+0x4c) -> type_ground vf15 0x4bb570:
    //   0x39 -> ground+0x88 (Traffic*), 0x3d -> ground+0x8c (Pedestrians*)
src has no typed layer and no native ground object, so the gate is always 0 here.

### Per native (exe pseudocode, service-less outcome in **bold**)
    0x4dc400 setWater(FFF)V          getArgs(self,&point.y,&density,&viscosity); h=instanceToHandle(self);
                                     if (h->RI) { if (density>0 && viscosity>0) { PhysicsWorld__setWater(...); world+0x80=1; } else world+0x80=0; }
                                     **no-op** (no physics world service)
    0x4dc2b0 setWater(VVFF)V         same with point/normal read from the two Vector3 (x,y,z)      **no-op**
    0x4da500 addWaterLimit(VV)V      if (h->RI) PhysicsWorld__addWaterLimit(&point,&normal)        **no-op**
    0x4dd090 addTrafficCar(GameRef,V)I
                                     getArgs(self,&inst,&pos);
                                     if (inst == 0) { log "vm_ground_addtrafficcar: Traffic car instance is null in \"<file>\" at line <n>"; return 0; }
                                     if (!getInfo(h,0x39)) return 0;  ...                          **inst null: log, 0; else 0**
    0x4de520 remTrafficCar(I)V       getArgs(self,&car); if (car) { ... TrafficCar__remove(car) }  **no-op** (ids are raw car pointers; none exist)
    0x4de5f0 notifyTrafficCar(II)I   if (car == 0) return 0; ...                                    **0**
    0x4dc9e0 addTrafficN(GameRef,IFFF)I   if (n < 1 || !getInfo(h,0x39)) return 0; ...              **0**
    0x4dcdc0 addTrafficP(GameRef,V,IFFF)I if (n < 1 || !getInfo || pos == 0) return 0; ...          **0**
    0x4dd840 delTraffic()V           if (getInfo(h,0x39)) Traffic__clear();                         **no-op**
    0x4e6280 setTrafficCarBehaviour(II)V  if (car && car+0x138) car+0x160 = mode;                   **no-op**
    0x4de360 haltTrafficCross(V,F)V  if (getInfo) { nearestCross(pos); cross->halt(time, 1) }       **no-op**
    0x4dd700 haltTrafficPath(VV)V    if (p1 && p2 && getInfo) Traffic__haltPath(p1, p2)             **no-op**
    0x4de270 haltTrafficRoad(VV)V    if (p1 && p2 && getInfo) Traffic__haltRoad(p1, p2)             **no-op**
    0x4dd6c0 clearHalts()V           if (getInfo) Traffic__clearHalts()                             **no-op**
    0x4dd7f0 setTrafficCrossStopOffset(F)V  if (t = getInfo) t+0x3140 = f                           **no-op**
    0x4dd880 getNearestCross(V,F)V3  if (pos == 0) return 0; if (h == 0) return 0; if (!getInfo) return 0; ...  **null**
    0x4ddfb0 getStartDirection(VV)V3 if (!getInfo) return 0; ...                                    **null**
    0x4ddc50 getRouteLength(VV)F and findRoute(VV)F (one function, two registrations)
                                     r = -1.0f; if (p1 && p2 && getInfo) { ...findRoute... }; return r;   **-1.0f**
    0x4ddc30 getRouteLength()F       bytes 833d50e45f0000 7503 d9ee c3 d90554e45f00 c3:
                                     return route(0x5fe450) ? routeLen(0x5fe454) : 0.0f;            **0.0f** (route global never set)
    0x4dde50 getRoutePos(F)V3        if (route == 0) return 0; ...                                  **null**
    0x4ddb50 getRouteDist(V)F        r = -1.0f; if (route == 0 || !(0 < routeLen)) return 0.0f (FLDZ at 0x4ddc22);
                                     if (pos == 0) return r; ...                                    **0.0f**
    0x4dd470 alignToRoad(V)[V3       if (pos == 0) return 0; if (h == 0) return 0; if (!getInfo) return 0; ...  **null**
    0x4de580 setPedestrianDensityN(F)V   if (getInfo(h,0x3d)) Pedestrians__setDensity(d, d*1.1)     **no-op**
    0x4dc940 addPedestrianType(GameRef)V if (getInfo(h,0x3d)) { make type resident; Pedestrians__addType(type) }  **no-op**
    0x4de4d0 removePedestrianType(GameRef)V  if (getInfo(h,0x3d)) Pedestrians__removeType(type)     **no-op**
    0x4de420 pedestrianDistance(V,I)F    if (!getInfo(h,0x3d)) return -1.0f; ...                    **-1.0f**
    0x4de1c0 getTrafficCarPos(I,V)V      if (car) { pos = carPos(car); out.x/y/z = pos }            **no-op**
    0x4dda10 getPedestrianPos(V,V,I)V    getArgs(self,&pos,&out,&type);
                                         if (!getInfo(h,0x3d)) { out.x = 0; out.y = 0; out.z = 0; return; }  **out = (0,0,0)**
Field names of the out writes in 0x4dda10: 0x5a0fe8 "x", 0x5a100c "y", 0x5a1028 "z" (bytes checked).

## Changes
1. `src/script/natives.cpp`, next to `gr_set_fog`: add one function per native (names
   `gr_set_water`, `gr_set_water_v`, `gr_add_water_limit`, `gr_add_traffic_car`,
   `gr_rem_traffic_car`, `gr_notify_traffic_car`, `gr_add_traffic_n`, `gr_add_traffic_p`,
   `gr_del_traffic`, `gr_set_traffic_car_behaviour`, `gr_halt_traffic_cross`,
   `gr_halt_traffic_path`, `gr_halt_traffic_road`, `gr_clear_halts`,
   `gr_set_traffic_cross_stop_offset`, `gr_get_nearest_cross`, `gr_get_start_direction`,
   `gr_find_route` (used by both `getRouteLength(VV)` and `findRoute` rows),
   `gr_get_route_length`, `gr_get_route_pos`, `gr_get_route_dist`, `gr_align_to_road`,
   `gr_set_pedestrian_density_n`, `gr_add_pedestrian_type`, `gr_remove_pedestrian_type`,
   `gr_pedestrian_distance`, `gr_get_traffic_car_pos`, `gr_get_pedestrian_pos`).
   Each: `want("GroundRef.<name>", a, N, err)` with N = 1 + parameter count, then the
   **bold** result above (`Value::make_null()` for void/object, `make_int(0)`,
   `make_float(-1.0f)` / `make_float(0.0f)`). Comment block per function:
   `// 0xADDR GroundRef.name(desc): <one-line exe gate>` and
   `// SERVICE MISSING: no native ground object, Game__getInfo(self, 0x39|0x3d) is 0 (notes/19).`
   Water: `// SERVICE MISSING: no process-wide physics world (PhysicsWorld__setWater 0x501e10).`
   - `gr_add_traffic_car`: if `a[1]` is Null (or a null object) ->
     `std::fprintf(stderr, "vm_ground_addtrafficcar: Traffic car instance is null\n")`
     (INVENTED: the exe appends `in "<file>" at line <n>`; a NativeFn has no frame, same
     as `object_run_method`), result int 0; otherwise int 0.
   - `gr_get_pedestrian_pos`: `a[2]` Null -> INVENTED skip (the exe would write through a
     null object); else `get_o` + `write_f(vm, o, "x", 0.0f)`, `"y"`, `"z"` in that order;
     result null.
   - `gr_get_route_length` / `gr_get_route_pos` / `gr_get_route_dist`: the exe reads the
     process-wide route (0x5fe450..0x5fe474) that only `findRoute` writes; comment
     `shortcut: no route state until findRoute (0x417e50) is ported; the route is never set.`
2. Registry rows: point each GroundRef row at its function and set the last field to
   `false`. Both `getRouteLength(Ljava.lang.Vector3;Ljava.lang.Vector3;)F` and `findRoute`
   use `gr_find_route`.

## Files
- edit: `src/script/natives.cpp` (29 functions next to `gr_set_fog`, the GroundRef rows)
- read only: `notes/19-ground-traffic.md` §5, `gr_set_fog` / `anim_*` for the comment and
  arity style

## Done when
- every GroundRef row has a function and `false`; the native stub count drops by 29
- each function carries its `// 0xADDR` comment; the missing service is `SERVICE MISSING:`,
  the log suffix and null-out skip are `INVENTED:`, the route state is `shortcut:`
- `make -C src clean check` green (natives-check vs `analysis/natives.tsv` still passes);
  no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
