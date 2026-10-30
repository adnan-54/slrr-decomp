---
id: 234
status: todo
branch:
review:
depends: [235]   # both edit struct Car in traffic.h; 235 changes the next-ref fields
---
# traffic: per-car pushInstance + advance (lane motion)

## Goal
Port `TrafficCar__pushInstance` 0x4118f0 and `TrafficCar__advance` 0x4102d0 with the
three `CarPathRef` accessors they call (posOffset, dir, length), and call
`car_push_instance` from both `car_live_step` shortcut sites. `advance` gives a live
lane car its world position from its path position, its heading (+0xa0/+0xa4) and its
front point (+0x98/+0x9c) once `TrafficCar__step` 0x4107c0 has moved it. 0x4107c0,
the instance pose push and the first-instance getInfo init stay documented shortcuts
(services missing, see below); 0x4118f0/0x4102d0 return void, so dropping them is
the exe's own missing-data path.

## Exe evidence (cleaned pseudocode, renamed fields)
`T` = `g_traffic` 0x5e652c (src `Graph`). `CarPathRef` at car+0xc4 =
{+0 lane (src `lane` index), +4 path (src `path`), +8 dir (src `dir`)}; src field
names from src/game/traffic.h. `car->world` = +0x8c..+0x94, `front_x` +0x98,
`front_z` +0x9c, `pos` +0x88, `lateral` +0xa8, `speed` +0xb0.

### 0x40dde0 CarPathRef__posOffset(out, d, off)  (ECX = ref)
    out[0]=out[1]=out[2]=0;
    if (ref->path) {
        if (ref->dir >= 0) out = ctSegment__posOffset(ref->path, d, off);  // 0x413660
        else               out = ctPath__posOffsetAt(ref->path, d, off);   // 0x4133f0
    }
    // all 12 output bytes copied (x,y,z)

### 0x40dd60 CarPathRef__dir(out, d)  (ECX = ref)
    out[0]=out[1]=out[2]=0;
    if (ref->path) {
        if (ref->dir >= 0) out = ctSegment__dir(ref->path, d);  // 0x412f60
        else               out = ctPath__dirAt(ref->path, d);   // 0x412ed0
    }

### 0x40e810 CarPathRef__length() -> float  (ECX = ref, no stack args)
    if (!ref->path) return 0.0f;
    return ref->dir >= 0 ? *(float *)(ref->path + 0x30)    // Segment::len
                         : *(float *)(ref->path + 0x38);   // Path::length

All three in src: `dir >= 0` (a car on a cross) is unrepresentable — card 233's
`car_live_step` already says so and `dir` is only ever -1 — so the segment branches
are a `shortcut:`; the lane branches call the existing src helpers.

### 0x4102d0 TrafficCar__advance  (ECX = car, void; only caller 0x411a5d)
    if (car->lane == 0) return;                      // car+0xc  (src: !car->path)
    t0 = car->e4; t = T->sim_time;                   // car+0xe4, T+0x84
    if (!(t0 < t)) return;                           // JBE
    car->e4 = t;
    dt = t - t0;                                     // local_8
    TrafficCar__step(car);                           // 0x4107c0  shortcut (see below)
    oldx = car->world[0]; oldz = car->world[2];
    p = CarPathRef__posOffset(ref, car->pos, car->lateral);   // 0x40dde0
    car->world[0..2] = p[0..2];                      // +0x8c/+0x90/+0x94
    dx = car->world[0] - oldx; dz = car->world[2] - oldz;
    dist = sqrt(dx*dx + dz*dz);
    if (dist == 0.0f) return;
    L = car->k18;                                    // +0x18
    if (L / (dt + 1.0f) <= dist) {                   // snapped: take the front from the path
        dir = CarPathRef__dir(ref, car->pos);        // 0x40dd60
        car->a0 = -dir[0]; car->a4 = -dir[2];        // +0xa0/+0xa4 (dir[1] unused)
        d  = car->pos + L;
        dl = CarPathRef__length(ref);                // 0x40e810
        d2 = d - dl;
        if (d2 <= 0.0f) d2 = d;
        q = CarPathRef__posOffset(ref, d2, car->lateral);
        tx = q[0]; tz = q[2];
    } else {
        if (0.01f < dist) { car->a0 = dx/dist; car->a4 = dz/dist; }
        fx = car->front_x - oldx; fz = car->front_z - oldz;   // +0x98/+0x9c
        h = sqrt(fx*fx + fz*fz);
        if (1e-05f < h) { fx = fx/h; fz = fz/h; }             // decompiler prints 1e-05
        dot  = fx*dx + fz*dz;
        disc = (dot*dot*dist*dist + L*L) - dist*dist;         // keep this order
        root = disc > 0.0f ? sqrt(disc) : 0.0f;
        f = dot*dist + root;
        if (T->front_mode == 1) f = f + (L - f) / (dist + 1.0f);  // global 0x5e6518
        else if (T->front_mode == 2) f = L;
        tx = fx*f + oldx; tz = fz*f + oldz;
    }
    nx = tx - car->world[0]; nz = tz - car->world[2];
    n = sqrt(nx*nx + nz*nz);
    if (1e-05f < n) { nx = nx/n; nz = nz/n; }
    car->front_x = car->world[0] + nx*L;             // +0x98
    car->front_z = car->world[2] + nz*L;             // +0x9c
    // only x/z of the front point; its y is never stored
Constants read from the image: 0.01f = 0x3c23d70a @0x5935d0, 1.0f @0x593598, 0.5 @0x593590.
0x5e6518 is 0 in the BSS (whole 0x5e6510..0x5e652f read back zero); its setter is not
traced, src keeps it as a per-graph field initialised 0.

### 0x4118f0 TrafficCar__pushInstance  (ECX = car, void)
    if (car->lane == 0 || car->live == 0) return;    // car+0xc, +0xdc  (src: !car->path)
    t = T->sim_time;                                 // T+0x84
    if (!(car->e8 < t)) return;                      // car+0xe8
    dt = t - car->e8; car->e8 = t;
    if (car->k140 == 0 && car->inst_id != 0) {       // +0x140, +0x138
        car->k140 = 1;
        ok  = getInfo(car+0x130, 0x40, &car->k18);   // 0x47c640
        ok &= getInfo(car+0x130, 0x46, &car->k1c);
        ok &= getInfo(car+0x130, 0x41, box /* ebp-0x84, 6 floats */);
        if (ok) {
            half = (1.0f - car->k1c) * car->k18;
            car->k28 = max(-box[0], box[3]);         // max(-min.x, max.x)
            car->k14 = -box[2] - half;               // -min.z - half
            car->k20 = half + box[5];                //  half + max.z
            car->k120 = car->k20;                    // +0x120, not in src Car
            // the CMP car->cc == -1 at 0x4119d0 is dead: the next SUBSS overwrites
            // the flags, and the JNZ at 0x411a3f skips the lane+0xc8 += delta store
            // unless delta == 0, where it adds 0.  Do not port that store.
        }
    }
    TrafficCar__advance(car);                        // 0x4102d0
    if (car->inst_id == 0) return;                   // 0x411c49 JZ
    // ---- tail 0x411c4f..0x411ef6, not ported (see Shortcuts) ----
    // object = next ref idx>=0 ? cross path +0x5c : 0x40d970(ref) or car->k128
    // y = world[1]; 0x40da20(ref, front_x, front_z, &y), retry with ref car+0xd0
    // F = normalize(front_x - world.x, y - world.y, front_z - world.z)
    // A = (F.z/h, 0, -F.x/h), B = (-F.x*F.y/h, h, -F.y*F.z/h), h = |F.xz|
    // Mat34 0x567250 columns: 0=A, 1=B, 2=F; euler = 0x569970(matrix)
    // center = world + F*(k18*0.5)
    // s = 0x56a000("fffffffffffffffi" @0x5935e8, center, euler, -speed*F,
    //             0,0,0, 2*angdiff, -(speed*dt), k158, object)
    // angdiff = wrap(atan2(-a0, -a4) - euler.yaw) into (-pi, pi]
    // 0x4eb3f0(car+0x130, s) sends it to the typed object's vf7

Callers: 0x40f350 liveStep at 0x40f47c (the `!live` head) and 0x40f4ca (the tail)
= src `car_live_step`, two sites; a third exe caller at 0x4aebe0 is outside the
traffic walk (procedure not identified here).

## Shortcuts / missing services (state them in the code)
1. `TrafficCar__step` 0x4107c0 is not ported (notes/37 §4): keep the call comment;
   with no step the car does not travel, `dist == 0`, so `advance` only stamps e4.
2. First-instance init block (0x411975..0x411a53): needs `Game__getInfo(inst, 0x41)`
   (type_car box). src has no Game__getInfo on a car instance. Exe behaviour when a
   getInfo fails: skip the block (`JZ 0x411a5b`); the +0x18/+0x1c stores of earlier
   successful calls remain. src takes exactly that path (nothing available), so the
   init body is not ported. UNRESOLVED: type_car vf15 0x41 output layout/semantics;
   raw line: `iVar3 = FUN_0047c640(iVar3,0x41,local_88);` (0x4119c3).
   0x40/0x46 map to CarData wheel mounts as in `gr_add_traffic_n`
   (src/script/natives.cpp ~7444) if a later card ports the block.
3. Instance tail 0x411c4f..0x411ef6: `SERVICE MISSING` — src has no traffic_car
   typed object / vf7, and 0x40da20 (path y at front xz, callees 0x4136e0/0x413b00)
   and 0x40d970 are untraced. The exe drops the push only when +0x138 == 0; there is
   no return value, so skipping the pose send changes nothing observable in src.
   It also keeps k158 (+0x158) unsmoothed and leaves `k120` unneeded.
4. `dir >= 0` (cross path) branches of the three accessors: no car is ever on a cross
   in src. Only the segment forms differ — 0x412f60 `ctSegment__dir` is already
   src `segment_dir` and the +0x30 read is `Segment::len`, but 0x413660
   `ctSegment__posOffset` has no src equivalent; keep the lane branch only, with a
   `shortcut:` naming those addresses.

## Files
- edit: src/game/traffic.cpp — add `car_ref_pos_offset`, `car_ref_dir`,
  `car_ref_length`, `car_advance`, `car_push_instance` (static, above
  `car_live_step`); replace the two `shortcut: 0x4118f0 -> 0x4102d0 ...` comments in
  `car_live_step` (~lines 1100/1116) with `car_push_instance(g, car);`.
- edit: src/game/traffic.h — `Car`: `float heading_x = 0, heading_z = 0;  // +0xa0, +0xa4`
  before `lateral`; `Graph`: `int32_t front_mode = 0;  // global 0x5e6518, INVENTED:
  per graph, initial 0 (BSS); setter not traced`.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
