# Car / part structs and their natives

Scope: every `Chassis`, `Vehicle`, `WheelRef`, `Part`, `DynoData` and `SfxTable` native in
`analysis/natives.tsv` (114 entries) was decompiled, plus the constructors/factories that allocate the
objects. This note records the struct layouts, who allocates them, and the native-by-native offset map.
Vehicle physics/drivetrain semantics live in `09-vehicle.md`; the dyno engine model lives in
`10-engine-dyno.md`; both are referenced, not repeated.

Ground truth for script-visible semantics: the shipped 2.3.1 sources
`bin/the game/parts/scripts/{Part,WheelRef,DynoData}.java` and
`bin/the game/parts/scripts/bodypart/Chassis.java` (not in git). `Chassis extends BodyPart extends Part
extends GameType`; `WheelRef`, `DynoData`, `SfxTable` extend `Native`.

## 1. How a native gets its object (three different payload paths)

1. **Part / Chassis** are resource game objects. The native does
   `handle = Native__instanceToHandle(scriptInstance, DAT_007a2620)`; `handle+0xc` is the resource
   instance `RI`. If `RI+0x7c != 1`, call `RI->vf5(1.0)` (make resident), then
   `Resource__getTyped(0xa0000000,0,0)` and `RI->vf3(1.0)` return the typed data `T`.
   `T+0x4c` is the native payload `P` (the car/part struct). `T+0x44` is the secondary script
   instance; fetching `Resource__getTyped(0x20000000)` on it and calling `vf3` yields a wrapper `S`
   whose `S+0xc` is the **manager/type block `M`**. `T+0x50` is the part's own script instance.
2. **WheelRef / SfxTable / DynoData** are lightweight wrappers. Their native handle payload is the raw
   struct pointer, installed with `FUN_004ed320(scriptInst, DAT_007a2620, ptr)`:
   - `Chassis.getWheel(i)` wraps `*(int*)(car+0x1414) + i*0x2e4` as `java.game.parts.WheelRef`
     (`FUN_00549ee0` allocates the instance);
   - `Chassis.getSfxTable(i)` wraps `e+0xfc / e+0x344 / e+0x58c` as `java.game.parts.SfxTable`;
   - `DynoData.newNative()` wraps its 0x1ec-byte allocation.
3. When no instance is available, the engine calls `FUN_00468b00(typeMask,0,0,0)`; the returned
   wrapper's `+0xc` is the same manager `M` (used e.g. by `getWheelDamage`). This is the resource/type
   registry lookup described in `06-native-methods.md` (type script entry).

`GameRef__getPhysBody(part)` returns the physics body; `body+0xb4` is its body-info record,
`bodyinfo+0x5c` the body definition, `def+0x2c` inverse mass, `def+0x90` the collision geometry
(`geom+0x20` vertex count, `geom+0x24` vertex array, 12 bytes per vertex; `geom+0x40..0x48` shape COM).

## 2. Allocation summary (struct sizes)

| object | size | allocated / constructed at | notes |
|---|---:|---|---|
| car / Chassis part instance | **0x2284** | `FUN_00567a20(0x2284)`, ctor `Vehicle__ctor 0x490100`, init `Part__initInstance 0x4c1f90` | `Vehicle__createCar 0x4992e0` returns it; same base layout as any `Part` instance |
| part instance (non-chassis) | 0x2284 | same allocator/ctor/init | payload of `Part` scripts |
| wheel array | `count*0x2e4` | count = `M+0x1fb4`; element ctor `Wheel__ctor 0x4919c0`, dtor `Wheel__dtor 0x493af0`; pointer field at `car+0x1414` | the pointer stored at `car+0x1414` points at record 0 (no count word); `getWheel` indexes `*(car+0x1414) + index*0x2e4` directly |
| suspension spring | **0x104** | `FUN_00567a20(0x104)` (malloc), ctor `Suspension__ctor 0x4915d0`; pointer at `wheel+0x30` | solver/semantics in `09-vehicle.md` |
| runtime slot array (per part) | `M+0x1160 * 0x68` | ctor `Slot__ctor 0x4c13d0`; pointer at `P+0x78` | one record per slot, `09`-style slot model |
| part element array | `M+0xff8 * 0x10` | ctor `FUN_00491990`; pointer at `P+0x4c` | per-part elements/attachments |
| part float array | `M+0xff8 * 4` | pointer at `P+0x50` | parallel float values |
| engine/chassis block | **0xc34** | `EngineBlock__clone 0x4b0910` from `M+0x2ebc`; pointer at `car+0x2050` | default block in the type manager |
| SfxTable | **0x248** (payload 0x244) | embedded in engine block at `e+0xfc`, `e+0x344`, `e+0x58c`; copy ctor `SfxTable__copyCtor 0x4ad650` | 16 items × 0x24 starting at +0 |
| DynoData / engine definition | **0x1ec** | `Mem__alloc 0x567a20`, ctor `EngineModel__ctor 0x4f44a0`, freed by `EngineModel__freeTables 0x4f44e0` + `Mem__free 0x56a42f` | pointer stored at `e+0xc30` when the engine part is attached |
| bucking record | **0x40** | `Mem__alloc(0x40)` inside `setBuck`; vtable `bucking::vftable` | list anchored at `car+0x1e18`, node fields +0x24..+0x3c |
| installed-part link | **0x34** | ctor `FUN_00491b10(record, body)`; one per manager installed-part record | linked from `car+0x2164` (head `+0x216c`), records `M+0x2c38/0x2c3c` stride 0x28 |
| wheel template | **0xc8** each | `M+0x1fb8 + i*0xc8`, count `M+0x1fb4` | type data, not separately allocated by the natives |

`Mem__alloc 0x567a20(size)` is the engine's size-tagged allocator; `Mem__free 0x56a42f(ptr,size)` its
counterpart. `FUN_00549ee0` creates a script `Instance` of a named JVM class; `FUN_004ed320` attaches
the native payload.

## 3. Car / chassis / part instance (`P`, size 0x2284) — base fields

For a `Part` script, `P = T+0x4c`. For `Chassis`, `P` **is** the car struct (the same object also holds
the vehicle state documented in `09-vehicle.md` at `+0x70 .. +0x2258`).

| offset | type | meaning | readers / writers |
|---:|---|---|---|
| +0x4c | ptr | array of 0x10-byte elements, count `M+0xff8` | `Part__initInstance` |
| +0x50 | ptr | float array, count `M+0xff8` | `Part__initInstance` |
| +0x54 | int | 0 after init | |
| +0x58 | int | 7th cfg field (`"%f,%f,%f,%f,%f,%f,%d"`), part slot/config value | `Part__initInstance` |
| +0x70/+0x74 | int | 0 | `Part__initInstance` |
| +0x78 | ptr | runtime slot array, `M+0x1160` × 0x68 | all slot natives; init copies type-slot pos/matrix |
| +0x84 | int | texture resource id | `getTexture` / `setTexture` |
| +0x94 | int | mesh resource id | `getMesh` / `setMesh` |
| +0xa4 | int | render type | `getRenderType` / `setRenderType` |
| +0xac | int | part/resource id (`*(resource+8)`) | set by init; unused by these natives |
| +0xb0 | int | 0 after init | |
| +0xb4 | float | tear (init **1.0**) | `getTear` / `setTear` |
| +0xb8 | float | stored wear (`getWear` returns `1.0 − [+0xb8]`, init 0) | `getWear` / `setWear` |
| +0xbc | float | `1/maxWear` (init `1/M+0x121c`) | `setMaxWear(v)` writes `1/v` |
| +0xc0 | obj | embedded per-part game/event object (`FUN_0047c640(P+0xc0,7,0)`) | flap / install code |
| +0xc8 | int | car id (`getCar`) | also written by init |
| +0xcc | obj | reference to the car/chassis **Part** object; typed 0xa0000000 → `+0x50` script instance | `getCarRef` |
| +0xd0 | int | wheel id (0 = not a wheel part) | `getWheelID` |
| +0x120 | int | event id, copied from `M+0x1220` (`M[0x488]`) | init / flap |
| +0x124 | ptr | physics-bone handle list head | init |
| +0x12c/+0x130/+0x134 | float×3 | part position from cfg (x, y, z) | `Part__initInstance` (cfg parse) |
| +0x138/+0x13c/+0x140 | float×3 | cfg rotation applied to `(0,1,0)` — part up/Y axis (middle column of the Euler matrix) | `Part__initInstance` (cfg parse) |
| +0x144/+0x148 | float | Sfx loop params, init **1.0** | `setSfxLoopParams` |
| +0x1b0c..+0x1b18 | — | **not here**: these are manager `M` OSD fields, see §5 | `getOSD`/`setOSD` |
| +0x1e00 | int | current horn record index (0..3) | `getHorn`; AI int id 8 |
| +0x1e18 | list | bucking list anchor (sentinel at +0x1e38, tail +0x1e40) | `setBuck` |
| +0x1e5c/+0x1e60 | int | gear index / copy | AI int ids 6/7 |
| +0x1e80 / +0x1e7c | float | smoothed rpm rad/s / raw engine ω | `getRPM` = ·9.549296; `setRPM` |
| +0x1ec0 | float | engine temperature °C | `getEngineTemp` |
| +0x1f5c | float | nitro left | `getNitro` |
| +0x2050 | ptr | engine/chassis block `e` (0xc34) | init/refresh |
| +0x2250 | float | mileage | `getMileage` / `setMileage` |
| +0x2258 | float | crime value, default −1.0 | `hasCrime` |
| +0x1ddc..+0x1df8 | floats | steering, throttle, brake, handbrake, clutch, nitro inputs | AI float params |
| +0x13ec | ptr | root physics body | getAngvel/getCM/getMin/getMax/getWheelPos |

Part-instance init (`Part__initInstance 0x4c1f90`, ECX = `M`, args = part resource + cfg string + car):
links the physics body, sets `+0xac` from `resource+8`, `+0x144/+0x148 = 1.0`; allocates the two
`M+0xff8` arrays (each allocation has a 4-byte count header, so the stored pointer is `alloc+4`);
parses `"%f,%f,%f,%f,%f,%f,%d"`: 3 position floats → `+0x12c/+0x130/+0x134`, 3 Ypr floats →
`Math__eulerToMat`, whose up column (`R·(0,1,0)`) is stored at `+0x138/+0x13c/+0x140`, 7th int →
`+0x58`. The full 3×4 rotation matrix is used transiently for bone attachment and is not kept on `P`.
It then creates physics bones from manager records (count `M+0xfa8`, record base `M+0x128`, stride
0x74; record `+0x3c` kind 1/2, `+0x40` body/shape pointer, `+0x58` flags — see §4.1); attaches bones
named `"bbone"`, `"steer"`, `"cbone"`, `"click"` via `FUN_004f78d0`; allocates the runtime slot array
(`count*0x68 + 4`, stored pointer after the count header) and copies type-slot pos/matrix; sets
`+0xb4=1.0, +0xb8=0, +0xbc=1/M+0x121c, +0xd0=0`; queues `"add_mslot"` events.

## 4. Vehicle manager / type block `M`

`M` is reached from a car or part through the `0x20000000` typed wrapper (`S+0xc`), or through
`FUN_00468b00(0x20000000,...)`. It is the shared per-type descriptor (templates, slot table, OSD
calibration, part list). Fields confirmed by these natives:

| offset | type | meaning |
|---:|---|---|
| +0x128 | — | bone/body records, 0x74 each (§4.1); `+0x13c` is the matrix start (`base+0x14`) read by `Part__initInstance` |
| +0x168 | ptr | body pointer duplicated per record (`base+0x40`), same 0x74 stride |
| +0xfa8 | int | bone/body record count (max 0x20, indices 0..0x1f) |
| +0xff0 | int | first body index (set to the current count if negative) |
| +0xff8 | int | count of `P+0x4c` (0x10) / `P+0x50` (4) arrays |
| +0xffc | int | index of the last body created by the cfg handler |
| +0x1120 | int | manufacturer/logo id (`getLogo`) |
| +0x1160 | int | slot count = `getSlots()` (max slot index+1) |
| +0x116c | ptr | type-slot linked-list head; node `+0x4` next, `+0x24` pos Vector3, `+0x30..0x5c` 3×4 matrix, `+0x60` slot ID, `+0x64` part type id (low 16 bits), `+0x16c` per-slot condition/field |
| +0x121c | float | max wear source (`P+0xbc = 1/M+0x121c`) |
| +0x1220 | int | part event id (`P+0x120`) |
| +0x1b0c,+0x1b10,+0x1b14,+0x1b18 | float | OSD gauge base/gain slots 5..8 |
| +0x1b54,+0x1b58,+0x1b5c,+0x1b60 | float | OSD gauge base/gain slots 0..3 |
| +0x1f1c/+0x1f20/+0x1f24 | Vector3 | vehicle origin/COM offset used by getCM/getMin/getMax/getWheelPos |
| +0x1f34 | float | wheel spring weight/gravity source (createCar) |
| +0x1fb4 | int | wheel count (`getWheels`) |
| +0x1fb8 | — | wheel templates, 0xc8 each (§6) |
| +0x2c38 / +0x2c3c | int / ptr | installed-part record count / array, stride 0x28 |
| +0x2ebc | ptr | default engine/chassis block (0xc34), cloned per car |
| +0x2ec4 | float[17] | default Pacejka coefficients (0x44 bytes), copied to each wheel on create/refresh |
| +0x1f5c / +0x1f7c | int | event ids (tyre flat / suspension damage, used by `Vehicle::update`) |

Open: who allocates/populates `M` (the type resource loader), and the meaning of most wheel-template
fields beyond the ones listed in §6.

### 4.1 Bone/body records and the cfg body handler

Record `R = M + 0x128 + i*0x74`, count `M+0xfa8` (max 0x20). `M+0x168 + i*0x74` is `R+0x40`
(same body pointer), so `Part__initInstance`'s `ptr = M+0x13c, stride 0x74` is the matrix start
`R+0x14`, not the record base.

| offset | type | meaning |
|---:|---|---|
| +0x00/+0x04/+0x08 | float | body position (cfg x, y, z) |
| +0x0c..+0x38 | float[12] | 3×4 rotation matrix from the cfg Euler triple |
| +0x3c | int | record kind: 1 = physics bone/body; 2 = extra bone flags + event 0x60 |
| +0x40 | ptr | body/shape object (numeric id → `Physics__createBody`; primitive → `Physics__createBox`) |
| +0x44 | ptr | intrusive body-list link written by the numeric-id path |
| +0x58 | int | flags: `0x100` → bone flag 0x20, `0x200` → bone flag 0x4000 |

Cfg body line handler `FUN_004bf4d0(line, M)` parses
`"%f %f %f %f %f %f %f %s %f %f %f"` = position(3), rotation(3), mass, shape token, size(3).
The first token (presumably the body name) is NUL-skipped before parsing and is not stored. Shape token:

- numeric resource id: `"%d"` parse accepts `0x`hex or decimal with optional trailing `r`; resolves
  `FUN_0051e860(id)`, request-loads `FUN_0051fa40(res,9,0)`, builds the body with
  `Physics__createBody(mass,&def)`; the size floats are ignored.
- `ball` / `sphere` → primitive kind 1 (radius = size x, y/z ignored).
- `box` → kind 5 with all three sizes.
- `rodx` / `rody` / `rodz` → kinds 2/3/4 (third size zeroed).
- any other name → kind 6 with the name token and unit sizes.

In the primitive branches the shape is created with `Physics__createBox(mass,kind,sx,sy,sz,name)`.
On success (both paths) the handler sets
`R+0x3c = 1`, stores position at `R+0..8`, builds the Euler matrix at `R+0x0c..`, clears body flag
bit 0 and sets bit 2 when inverse mass (`body def +0x2c`) is 0 (static), and sets
`M+0xffc = count−1`. On failure it decrements `M+0xfa8` so the record is reused.

## 5. OSD fields

`getOSD(id)` / `setOSD(id,value)` operate on **manager** `M`, not the car:

| id | expression |
|---:|---|
| 0 | `M+0x1b54` |
| 1 | `M+0x1b58` (at address `+7000` decimal) |
| 2 | `M+0x1b5c` |
| 3 | `M+0x1b60` |
| 4 | `M+0x1b54 + |body velocity| · scale · M+0x1b58` (scale constant at `0x5fe378`; exact factor not resolved) |
| 5..8 | `M+0x1b0c / +0x1b10 / +0x1b14 / +0x1b18` |
| 9 | `M+0x1b0c + |rpm| · M+0x1b10` (rpm from `car+0x1e80·9.549296`) |

`setOSD` accepts ids 0–3 and 5–8 only (4 and 9 are computed). This pins down the "global HUD/OSD
table `+0x1b0c..+0x1b60`" mentioned in `09-vehicle.md`: those accesses are `M+...`, not `car+...`.

## 6. Wheel template record (`M+0x1fb8 + i*0xc8`, size 0xc8)

| offset | meaning |
|---:|---|
| +0x00 | mount position Vector3 (x, y, z) |
| +0x0c | mount Ypr (yaw, pitch, roll) |
| +0x2c | wheel-damage visual offset base (used by `setWheelDamage`: `+0xf0 = damage·200 + template[0x2c]`) |
| +0x38 | base length added to template y in `setPos/getPos` |
| +0x3c | suspension travel added to template y in `setPos/getPos`; `W+0x50 = +0x38 + +0x3c` at create |
| rest | not resolved (likely suspension/Pacejka defaults) |

`setPos` computes the new hub from `template.pos + (arg pos or 0)`, with
`y += template+0x38 + template+0x3c`. `getPos` returns the inverse:
`(template.x, W+0x7c − template.y − template+0x38 − template+0x3c, template.z)`.

## 7. Wheel record (`W = *(int*)(car+0x1414) + i*0x2e4`, size 0x2e4)

`WheelRef` payload is exactly `W`; `car = *(W+0x22c)`, `i = (W − *(int*)(car+0x1414))/0x2e4`.
This table extends/corrects the wheel table in `09-vehicle.md` (notably: `setMaxLen` writes `+0x54`,
`setMinLen` writes `+0x58`; `Suspension__configure` maps `+0x54 → S.max`, `+0x58 → S.bump stop`).

| offset | type | meaning | native(s) |
|---:|---|---|---|
| +0x00..+0x2f | — | zeroed by ctor | `Wheel__ctor` |
| +0x30 | ptr | suspension object `S` (0x104) | createCar; spring config |
| +0x38/+0x3c/+0x40 | float | template base length / travel / damping ratio used to derive spring defaults | createCar |
| +0x44 | float | spring k | `setForce`, `setDamping`, `setRadius`, `setRestLen`, `setMinLen`, `setMaxLen` |
| +0x48 | float | bump damping | `setDamping` |
| +0x4c | float | rebound damping (default `0.7·bump`) | `setDamping` |
| +0x50 | float | rest length | `setRestLen` |
| +0x54 | float | **max length** (spring ray limit); default `+0x38+0.1` | `setMaxLen` |
| +0x58 | float | **min length** (bump-stop); default −1 | `setMinLen` |
| +0x5c | float | rest length + radius (current free length) | recomputed by all length setters |
| +0x60 | float | radius; `+0x5c = +0x50 + r` | `getRadius`, `setRadius` |
| +0x64/+0x68 | float | base tyre friction (both set together) | `setFriction` |
| +0x6c/+0x70 | float | effective friction = base · `max(1 − w²·0.6, 0.1)`, `w = +0x15c` tyre wear | `setFriction` |
| +0x74 | float | rolling resistance | `setRollRes` |
| +0x78/+0x7c/+0x80 | Vector3 | hub position in car frame | `getPos`, `setPos`, `setHub`, damage get/set |
| +0x84/+0x88/+0x8c | Vector3 | template vector (create) | |
| +0x90..+0xbc | float[12] | wheel orientation 3×4 matrix (Euler→matrix) | `setYpr`, damage get/set |
| +0xc0 | float | wheel spin ω (rad/s, +forward) | `Vehicle::update` |
| +0xc4 | float | drive ratio / sign | template/part scripts |
| +0xc8 | float | steer factor | `getSteer`, `setSteer` |
| +0xcc | float | drive base | `getDrive`, `setDrive` |
| +0xd0 | float | brake base | `getBrake`, `setBrake` |
| +0xd4 | float | handbrake base | `getHBrake`, `setHBrake` |
| +0xd8 | float | effective brake = base · `max(1 − w²·0.6, 0.1)`, `w = +0x164` brake wear | `setBrake` |
| +0xdc | float | effective handbrake, same `w` | `setHBrake` |
| +0xe0 | float | effective drive share (`= +0xcc` on `setDrive`) | `setDrive` |
| +0xe4 | float | 0 at create | |
| +0xe8 | float | wheel spin angle (rad) | `Vehicle::update` |
| +0xec | float | inverse effective inertia ×0.1 | `Vehicle::update` |
| +0xf0 | float | wheel-damage visual/offset value = `damage·200 + template[0x2c]` | damage get/set |
| +0xf4 | float | wobble amplitude | `setWheelDamage`/`getWheelDamage` |
| +0xf8 | float | wobble phase/damage value | damage get/set |
| +0xfc | float | extra brake drag | `Vehicle::update` |
| +0x100 | int | flags: bit `0x20` = pose/damage override active | damage get/set |
| +0x110..+0x13c | float[12] | pose matrix copy of `+0x90..0xbc`, anchor used by `FUN_004f4380(pos, W+0x110)` | `setYpr`, `setPos`, damage set |
| +0x140..+0x148 | float[3] | contact slip velocity (body frame) | `Vehicle::update` |
| +0x14c/+0x150/+0x154/+0x158 | float | slip κ / α / σ / aligning torque Mz | `Vehicle::update` |
| +0x15c/+0x160 | float | tyre wear / rate | damage; `setFriction` reads wear |
| +0x164/+0x168 | float | brake wear / rate | `setBrake`, `setHBrake` |
| +0x16c/+0x170 | float | suspension damage / rate | `Vehicle::update` |
| +0x174 | float | tyre heat capacity, default **30000.0** | createCar |
| +0x178/+0x17c | float | tyre temperature / ambient, default **20.0** | createCar |
| +0x180 | float | tyre cooling, default **1/600 = 0.0016666667** | createCar |
| +0x184 | float | brake heat capacity, default **50000.0** | createCar |
| +0x188/+0x18c | float | brake temperature / ambient, default **20.0** | createCar |
| +0x190 | float | brake cooling (0.001) | createCar |
| +0x194..+0x1c4 | — | two skid/sound context handles (owners `+0x19c`/`+0x1bc`, indices `+0x1b0`/`+0x1d0`) | ctor zeroes `+0x194..+0x1a3` |
| +0x1a8 | vtable | embedded `WheelParticle_Context` (word 0x6a) | `Wheel__ctor` |
| +0x1c8 | vtable | embedded `WheelSkidmark_Context` (word 0x72) | `Wheel__ctor` |
| +0x1d4 | float | half width (`setWidth(v)` stores `v·0.5`; default 0.1) | `setWidth` |
| +0x1d8..+0x1e4 | — | render-part link; `+0x1e0` = has render object | `Vehicle::update` |
| +0x1e8..+0x228 | float[17] | Pacejka coefficients p0..p16 (map in `09-vehicle.md`); `setFriction` writes p2 (`+0x1f0`) | `setPacejka`, `setFriction` and aliases |
| +0x22c | ptr | car payload (back-pointer) | set at create |
| +0x230 | ptr | wheel template? (create) | |
| +0x234 | float | arm length | `setArm` |
| +0x238..+0x240 | Vector3 | arm axis (normalised) | `setArm` |
| +0x244..+0x24c | Vector3 | arm pivot = hub + p − (0, `+0x38`+`+0x3c`, 0) | `setArm` |
| +0x250 | ptr | opposite wheel (`setOppWheel(i)` = `*(int*)(car+0x1414) + i*0x2e4`, index<0 → null) | `setOppWheel` |
| +0x254 | float | hub length | `setHub` |
| +0x258..+0x260 | Vector3 | hub p1 | `setHub` |
| +0x264..+0x26c | Vector3 | hub p2/axis | `setHub` |
| +0x270..+0x278 | Vector3 | hub point pc = hub + pc (y adjusted by `−(0x38+0x3c)`); if len==0, p2 normalised and p1 subtracted | `setHub` |
| +0x27c..+0x284 | Vector3 | instant centre H = hub + H | `setInstantCenter`, `setPos` shifts |
| +0x288..+0x290 | Vector3 | instant centre L = hub + L | `setInstantCenter`, `setPos` shifts |
| +0x294 | vtable | `DList<animated_RI>` (word 0xa5) | `Wheel__ctor` |
| +0x2e0 | int | 0 at create | |

Pacejka slots written by named setters: `setSliction → p0` (+0x1e8), `setStiffness → p4`
(+0x1f8), `setMaxLoad → p11` (+0x214), `setFrictn_x → p12` (+0x218), `setLoadSmooth → p14`
(+0x220). `setPacejka(i,v)` writes `+0x1e8+4i` for `0 ≤ i < 17` (0x11). `setFriction` also writes
p2 (`W+0x1f0`) in addition to the base/effective friction fields; the tyre solver overwrites p2 each
tick (see `09`).

Wheel ctor (`Wheel__ctor 0x4919c0`): zeroes words 0..0xd (bytes 0x00..0x37) and words 0x65..0x68
(bytes 0x194..0x1a3), installs the three embedded vtables (0x1a8, 0x1c8, 0x294), calls `FUN_0041f880`.
`createCar` then fills hub/instant centres/matrix, spring (`S` at +0x30), spring constants
(`+0x44` k from gravity/weight and template lengths, `+0x48` bump, `+0x4c = 0.7·bump`), arm/hub fields,
opposite wheel, and the heat defaults above.

Wheel native setter details not obvious from the table:

- `setForce(f)`: `+0x44 = f`, then configure spring with `k=f`.
- `setDamping(f)`: `+0x48 = f`, `+0x4c = 0.7f`, configure.
- `setDamping(bump,rebound)`: `+0x48 = bump`, `+0x4c = rebound`, configure.
- `setRadius(r)`: `+0x60 = r`, `+0x5c = +0x50 + r`, configure `(k, bump, +0x5c, +0x54+r, +0x58+r, rebound, r)`.
- `setRestLen(v)`: `+0x50 = v`, `+0x5c = v + r`, configure.
- `setMaxLen(v)`: `+0x54 = v`, configure with `+0x54+r` as max.
- `setMinLen(v)`: `+0x58 = v`, configure with `+0x58+r` as bump stop.
- `setCPatch(halfwidth, angle, offset)`: on `S = W+0x30`: `S+0x9c = |halfwidth|`,
  `S+0xa0 = |halfwidth·sin(angle)|` (sine call in the native), `S+0xa4 = offset`.
- `setBearing(v)` → `+0xf0`.
- `setWidth(v)` → `+0x1d4 = 0.5v`.
- `setPos(pos)`: see §6; additionally shifts both instant centres by (new hub − old hub), writes
  `+0x78..0x80`, and re-anchors the spring at `W+0x110` after adding the car body's parent translation
  (two levels: body-info `+0x1c` and its parent `+0x20` → `+0x1c`, fields +4/+8/+0xc).
- `setYpr(ypr)`: null → template Ypr at `M+0x1fc4 + i*0xc8`; else fields `y,p,r`
  (`Instance__getFloatField` names at 0x59be7c/84/a8). `Math__eulerToMat` writes `W+0x90` (hidden ECX
  destination), then `+0x90..0xbc` is copied to `+0x110..0x13c`.
- `setHub/len...`: see the table; `setInstantCenter` is a straight hub-relative add.

## 8. Runtime slot record (`P+0x78 + slotIndex*0x68`) and type-slot node

The manager owns the immutable type-slot list (`M+0x116c`, `M+0x1160` entries); each part instance
owns a parallel runtime array (`P+0x78`, stride 0x68). Slot **ID** is a game/config id (e.g. 0x65 for
wheel 0), slot **index** is the 0-based position in the list. `Part.java` confirms the distinction
(`getSlotID(index)`, `getSlotIndex(id)`, `getSlots()` = max index+1).

Runtime slot record (0x68):

| offset | meaning |
|---:|---|
| +0x04/+0x08 | next link / attached Part game object (`0` = empty) |
| +0x0c | attached Part script instance object |
| +0x10 | mating/other slot ID (`slotIDOnSlot`) |
| +0x14 | flags: `0x1` occupied/present, `0x10` disabled, `0x20` pose/damage override, `0x40` hard-disabled |
| +0x28 | attached bone/trigger physics object (flap uses `+0x88` flag bits) |
| +0x2c/+0x30/+0x34 | slot position Vector3 |
| +0x38..+0x64 | slot orientation 3×4 matrix |
| +0x68 | end |

Type-slot list node (`M+0x116c`): `+0x04` next, `+0x24` pos Vector3, `+0x30..+0x5c` 3×4 matrix,
`+0x60` slot ID, `+0x64` part type id (low 16 bits), `+0x16c` per-slot condition/field. Lookup helpers:
`PartType__findSlotByID 0x4c6f90` (ID → node), `PartType__findRuntimeSlot 0x4c6fd0` (ID → parallel
runtime slot), `PartType__slotByIndex 0x4c7020` (index → node).

`disableSlot(id,status)`: `flags &= ~0x50`; status 1 → `|0x10`, status 2 → `|0x40|0x10`; repeats on the
mate slot (`slot+0x10`) of the installed part. `isSlotDisabled(id)`: `flags&0x50 ? 2 :
(flags>>4)&1`. `getSlotDamage(index)` returns `""` unless the slot is occupied (`flags&0x20`), else
`"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f"` = pos xyz + matrix→Ypr. `setSlotDamage(index,string)` parses 6
floats, writes pos `+0x2c..0x34`, Euler→matrix into the slot matrix (`+0x38`), sets `0x20`; an
empty/short string clears `0x20` and restores the type-slot pose. `setSlotPos(id,pos,ypr)` copies the
type slot pose (`type+0x24 → slot+0x2c`, `type+0x30..0x5c → slot+0x38..0x64`), applies the position
and Ypr deltas (fields `x/y/z`, `y/p/r`), sets `0x20`, and notifies the car physics via
`FUN_004cb8d0`/`FUN_004cb6c0`.

## 9. Engine/chassis block `e = *(car+0x2050)` (0xc34)

Layout is in `09-vehicle.md`; additions/corrections from this pass:

| offset | meaning |
|---:|---|
| +0xfc / +0x344 / +0x58c | three embedded `SfxTable`s (ids 0/1/2, size 0x248 each) |
| +0x7d4 | exhaust minimum volume (`setSfxExhaustMinVol`) |
| +0x840 + id*0x10 | four horn records (0x10 bytes each) |
| +0x878 + id*4 | horn pitch per id (0..3) (`setHornSFX`) |
| +0xa68 / +0xa6c | count (word 0x29a) / array (word 0x29b), stride 0x1c |
| +0xc2c | material index (`getMaterialIndex`; word 0x30b) |
| +0xc30 | `DynoData` native pointer (engine definition), zero in a fresh clone |

`EngineBlock__clone 0x4b0910` copies from `M+0x2ebc` and re-creates the embedded SfxTables
(copy ctor `0x4ad650`); it copies horn record field+4 and the pitch array from the template.

## 10. SfxTable (0x248; payload 0x244)

| offset | meaning |
|---:|---|
| +0x00 + i*0x24 | item i (16 max): `+0x0c` linked resource/ref handle, `+0x10` volume (`1/arg`), `+0x14/+0x18/+0x1c/+0x20` four params; `+0x00..+0x08` unused/playback state |
| +0x240 | item count |
| +0x244 | extra field copied by the copy ctor |

`addItem(resource, f, a,b,c,d)` (0x4b5980): if count<16, count++, item+0x10 = `1/f`,
item+0x14..+0x20 = a..d, addref of the resource ref (`FUN_0040cac0(*(resource+0xc))`).
`getItems` (0x4b5a90) = count; `clear` (0x4b5a60) = `FUN_00498280` reset.
`SfxTable__copyCtor 0x4ad650(dest ECX, src)`: copies count and +0x244, addrefs each item's
`+0xc` and copies `+0x10..+0x20`.

## 11. DynoData (0x1ec) — engine definition

Full engine model and field semantics: `10-engine-dyno.md`. Offset map repeated here for the struct
index:

| offset | meaning | offset | meaning |
|---:|---|---:|---|
| +0x00 | stroke/2 | +0x04 | bore/2 |
| +0x10 | Vmin | +0x14 | cylinders |
| +0x1c | time_burn | +0x34 | 293.15·intercooling |
| +0x3c | mixture_H | +0x44/+0x48/+0x4c/+0x50 | in_min/out_min/in_max/out_max |
| +0x58 | RPM_limit | +0x64 | Displacement (result) |
| +0x74 | Compression (result) | +0x8c | maxRPM (internal) |
| +0x94/+0x98/+0x9c/+0xa0 | valve open/close times | +0xa4/+0xa8/+0xb8/+0xbc | derived valve durations |
| +0xcc/+0xd0/+0xd4/+0xd8 | spark min/inc/RPM0/RPM1 | +0xdc/+0xe0/+0xe8 | turbo mul/opt/range |
| +0xec/+0xf0 | boost max/waste | +0xf4 | mixture_ratio |
| +0xf8/+0xfc | max fuel/air consumption | +0x1ac/+0x1b0 | max torque / max HP |
| +0x1c0/+0x1c4 | RPM at max torque / max HP | +0x1d0 | table step (int) |
| +0x1d4 | table count (int) | +0x1d8 | nitro flag (int) |
| +0x1dc | torque table ptr | +0x1e0 | torque2 (nitro) table ptr |
| +0x1e4 | HP table ptr | +0x1e8 | second HP table ptr (freed by dtor, not read by getHP) |

Ctor `0x4f44a0` zeroes `+0x120`, `+0x1d8`, `+0x1dc`, `+0x1e0`, `+0x1e4`, `+0x1e8`;
dtor/free `0x4f44e0` frees the four table pointers in pairs.

`calcDyno(steps)` 0x4cba80: reuse an attached native struct or allocate a temp; copy script fields
(bore, stroke, Vmin, cylinders, valve timing, time_burn, RPM_limit, spark, turbo, mixture,
consumption, intercooling) into the struct; run `EngineModel__setDefaults`, `updateMixture`,
`updateGeometry`, `buildTables(step)`; write back `maxRPM`, `table_stepsize`, `Displacement`,
`Compression`, `maxTorque`, `RPM_maxTorque`, `maxHP`, `RPM_maxHP`, `torque = 1.0`, and (temp mode only)
the `torquetable/HPtable/torquetable2` float arrays. `getTorque(rpm,nitro)` / `getHP(rpm,nitro)` lerp
the `+0x1dc` / `+0x1e4` table with step `+0x1d0` and count `+0x1d4`; the nitro argument is ignored;
outside `[0, (count−1)·step]` they return 0.

## 12. Native summaries

All addresses are virtual addresses in `StreetLegal_Redline.exe` (SLRR 2.3.1, build 940).
"chain" means the part/chassis payload path of §1; "payload W/M/..." means the direct pointer.

### 12.1 Chassis (`java.game.parts.bodypart.Chassis`) and Vehicle

All operate on `P = T+0x4c`, `M = S+0xc`, `e = *(P+0x2050)`, except where noted.

| address | native | behavior |
|---:|---|---|
| 0x4b24f0 | getAngvel | returns Vector3 angular velocity from root body `car+0x13ec` (follow `+0x20` while `+0x58&4`; active state `body+idx*0xd4`, angvel at `+0xd4`) |
| 0x4b2cb0 | getMaterialIndex | `e+0xc2c` (int), default −1 |
| 0x4b3110 | getMaxSteer | `e+0xa20` |
| 0x4b5030 | setMaxSteer | `e+0xa20 = f` |
| 0x4b39b0 | getShifting | `e+0x10` (int) |
| 0x4b3680 | getNitroing | `car+0x70 & 0x800` as int |
| 0x4b29d0 | getEngineTemp | `car+0x1ec0` |
| 0x4b2220 | getAIparam_float | 0→+0x1de4 throttle, 1→+0x1de8 brake, 2→+0x1df4 handbrake, 3→+0x1ddc steer, 4→+0x1df0 clutch, 5→+0x1df8 nitro; else returns the raw arg |
| 0x4b23c0 | getAIparam_int | 6→+0x1e5c gear, 7→+0x1e60 gear copy, 8→+0x1e00 horn id; else raw arg |
| 0x4b4b70 | setAIparam_float | same mapping, writes the float fields |
| 0x4b4d00 | setAIparam_int | same mapping, writes the int fields |
| 0x4b3780 | getRPM | `car+0x1e80 · 9.549296` (rad/s → rpm) |
| 0x4b51f0 | setRPM | `car+0x1e7c = f` (raw rad/s) |
| 0x4b3ab0 | getTorque | `torqueCurve(rpm)·car+0x1e04·(1−nitro) + nitroCurve(rpm)·car+0x1e08·nitro` (curves in `e+0x0/+0x8`) |
| 0x4b52e0 | setTorque | `e+0xd4 = e+0xd8 = f`; refresh health factors |
| 0x4b2b90 | getMass | `1 / (def+0x2c)` via `GameRef__getPhysBody` on `P` |
| 0x4b2680 | getCM | centre of mass: shape COM `def+0x90` (`+0x40..+0x48`) transformed by body state, offset by `M+0x1f1c..0x1f24`; returns Vector3 |
| 0x4b3220 | getMin | AABB min over collision-geometry vertices (`geom+0x20/+0x24`), transformed through parent-body chain, `+ M+0x1f1c..24`, **+0.1** padding |
| 0x4b2dc0 | getMax | same AABB max, **−0.1** padding |
| 0x4b2ae0 | getHorn (Vehicle) | `e+0x840 + (car+0x1e00)*0x10 != 0` (bool as int) |
| 0x4b4f40 | setHornSFX | args (resource, pitch, id); if id<4 addref the resource ref and `e+0x878 + id*4 = pitch` |
| 0x4b5130 | setNitroSFX | args (resource); addrefs the resource ref only (sound resource is managed by the asset system; no numeric field) |
| 0x4b4e30 | setCooling | `e+0xa5c/+0xa60/+0xa64 = a/b/c` (defaults 10/50/0.01) |
| 0x4b5890 | forceUpdate | `Vehicle__refreshParameters 0x4aebf0` |
| 0x4b1f30 | getMileage | `car+0x2250` |
| 0x4b4670 | setMileage | `car+0x2250 = f` |
| 0x4b4410 | setBuck | args (id, handle, a, b, c, f); if handle<1 search installed parts (`car+0xdc`) for record+0x2c==id, allocate 0x40-byte node, fields +0x24 id, +0x28 a, +0x2c b, +0x30 c, +0x34 f, +0x38 state=1, +0x3c world; insert at `car+0x1e38/+0x1e40`. handle≥1 → existing node: update when a,b>0 else destroy; returns node pointer |
| 0x4b4710 | setSfxExhaustMinVol | `e+0x7d4 = f` |
| 0x4b4ad0 | setSteerWheelRadius | `e+0xa28 = f` |
| 0x4b4a20 | setSteerWheel | `e+0xa28 = a`, `e+0xa2c = b` |
| 0x4b47b0 | setAckermann | `e+0xa30 = f` |
| 0x4b4370 | hasCrime (Vehicle) | `car+0x2258` (default −1.0) |
| 0x4b3890 | getSfxTable | id 0/1/2 → wrap `e+0xfc/+0x344/+0x58c` as `java.game.parts.SfxTable` |
| 0x4b4280 | getWheels | `M+0x1fb4` |
| 0x4b4150 | getWheel | index<count → wrap `*(int*)(car+0x1414) + index*0x2e4` as `java.game.parts.WheelRef`; else null |
| 0x4b3e40 | getWheelPos | car world position + `W+0x78..0x80`: transforms `M+0x1f1c..24` through the body parent chain, adds wheel hub. Default `(0,0,0)` |
| 0x4b3c50 | getWheelDamage | index<count and `W+0x100&0x20` → 10-float string `"%.3f"×10`: hub xyz, matrix→Ypr (3), `W+0xf4`, 1, 1, `W+0xf8`; else "" |
| 0x4b5480 | setWheelDamage | index<count; 10-float CSV: hub xyz, ypr (Euler→`W+0x90`), `p6 → W+0xf4`, `p9 → W+0xf8`, `W+0xf0 = p9·200 + template[+0x2c]`; sets `W+0x100|=0x20`; copies matrix `+0x90→+0x110`, re-poses spring with body parent translation. Empty/short string: clears `0x20`, restores template Ypr/hub, `W+0xd8/+0xdc = +0xd0/+0xd4`, `W+0xe0=+0xcc`, `W+0xf4=+0xf8=0`, `W+0xf0=template[+0x2c]` |
| 0x4b4850 | setOSD | id 0–3/5–8 → manager fields of §5 (ids 4/9 not settable) |
| 0x4b1fd0 | getOSD | id 0–9 → §5; 4 speed gauge, 9 rpm gauge |
| 0x4b3570 | getNitro | `car+0x1f5c` |
| 0x4e6960 | getSpeedSquare (Vehicle) | `FUN_0047c640(handle,3,&v)` fills velocity; returns `v·v`; logs "vm_vehicle_getspeedsquare: scripted resource ... not found" if the handle is null |

### 12.2 WheelRef (`java.game.parts.WheelRef`)

Payload `W`; `car = *(W+0x22c)`; `i = (W − *(int*)(car+0x1414))/0x2e4`; template `T = M+0x1fb8 + i*0xc8`.
Getters return float unless noted.

| address | native | behavior |
|---:|---|---|
| 0x4b5b90 | getPos | Vector3 `(template.x, W+0x7c − template.y − template[+0x38] − template[+0x3c], template.z)` |
| 0x4b5e40 | getYpr | `FUN_00569970(W+0x90)` → new Ypr |
| 0x4b5b10 | getDrive | `W+0xcc` |
| 0x4b5e00 | getSteer | `W+0xc8` |
| 0x4b5dc0 | getRadius | `W+0x60` |
| 0x4b5ad0 | getBrake | `W+0xd0` |
| 0x4b5b50 | getHBrake | `W+0xd4` |
| 0x4b6af0 | setPos | §7/§6: template pos + arg, shifts instant centres, writes hub, adds body parent translation, `FUN_004f4380(pos, W+0x110)` |
| 0x4b70e0 | setYpr | template Ypr when arg null; else `y/p/r` fields; Euler→`W+0x90`; copy to `+0x110` |
| 0x4b6330 | setDrive | `W+0xcc = W+0xe0 = v` |
| 0x4b7010 | setSteer | `W+0xc8 = v` |
| 0x4b6e50 | setRadius | `W+0x60 = r`, recompute `+0x5c`, configure spring |
| 0x4b7090 | setWidth | `W+0x1d4 = 0.5v` |
| 0x4b6150 | setCPatch | `S+0x9c = |halfwidth|`, `S+0xa0 = |halfwidth·sin(angle)|`, `S+0xa4 = offset` |
| 0x4b6420 | setFriction | `W+0x64 = W+0x68 = v`; `W+0x6c = W+0x70 = effective` with tyre wear `W+0x15c`; also Pacejka p2 `W+0x1f0 = v` |
| 0x4b6500 | setFrictn_x | Pacejka p12 (`W+0x218`) |
| 0x4b6fd0 | setSliction | Pacejka p0 (`W+0x1e8`) |
| 0x4b7050 | setStiffness | Pacejka p4 (`W+0x1f8`) |
| 0x4b6f90 | setRollRes | `W+0x74 = v` |
| 0x4b6080 | setBearing | `W+0xf0 = v` |
| 0x4b6950 | setMaxLoad | Pacejka p11 (`W+0x214`) |
| 0x4b6870 | setLoadSmooth | Pacejka p14 (`W+0x220`) |
| 0x4b6a90 | setPacejka | `0 ≤ i < 17` → `W+0x1e8 + 4i = v` |
| 0x4b6380 | setForce | `W+0x44 = v`, configure spring |
| 0x4b6280 | setDamping(f) | `W+0x48 = f`, `W+0x4c = 0.7f`, configure |
| 0x4b61d0 | setDamping(bump,rebound) | `W+0x48/+0x4c = bump/rebound`, configure |
| 0x4b6ef0 | setRestLen | `W+0x50 = v`, `W+0x5c = v + r`, configure |
| 0x4b6990 | setMinLen | `W+0x58 = v`, configure bump stop `v + r` |
| 0x4b68b0 | setMaxLen | `W+0x54 = v`, configure max `v + r` |
| 0x4b60c0 | setBrake | `W+0xd0 = v`; effective `W+0xd8 = v·max(1−w²·0.6, 0.1)`, `w = W+0x164` |
| 0x4b6540 | setHBrake | `W+0xd4 = v`; effective `W+0xdc` with same `w` |
| 0x4b5f50 | setArm | `W+0x234 = len`; `W+0x238..240 = normalize(axis)`; `W+0x244..24c = hub + pivot − (0, 0x38+0x3c, 0)` |
| 0x4b65d0 | setHub | `W+0x254 = len`; p1 → `+0x258..260`; p2 → `+0x264..26c`; pc → `+0x270..278` (= hub + pc, y −(0x38+0x3c)); if len==0, normalize p2 and subtract p1 from pc |
| 0x4b6790 | setInstantCenter | `W+0x27c..284 = hub + H`; `W+0x288..290 = hub + L` |
| 0x4b6a30 | setOppWheel | i≥0 → `*(int*)(car+0x1414) + i*0x2e4`, else null; `W+0x250` |

### 12.3 Part (`java.game.parts.Part`)

Payload `P = T+0x4c`; `M` as above. `getCarRef` returns the car/chassis Part script object; for a
Chassis instance `P` is the car struct.

| address | native | behavior |
|---:|---|---|
| 0x4cced0 | getSlots | `M+0x1160` (max slot index + 1) |
| 0x4ccaa0 | getSlotID | index<0 → ID of first runtime slot with `flags&1`; else type node `PartType__slotByIndex(M,index)+0x60` |
| 0x4ccbf0 | getSlotIndex | walk type list (`M+0x116c`, count `M+0x1160`), return index where node+0x60 == id, else 0 |
| 0x4cd600 | partOnSlot | id==−1 → `T+0x50` (this part's script instance); else runtime slot (`PartType__findRuntimeSlot`), if slot+8 → the attached Part's script instance (+0x50), else null |
| 0x4ce770 | slotIDOnSlot | runtime slot `+0x10` |
| 0x4ccd10 | getSlotDamage | if occupied and `flags&0x20`: `"%.3f,%.3f,%.3f,%.3f,%.3f,%.3f"` = slot pos `+0x2c/+0x30/+0x34` + matrix→Ypr; else "" |
| 0x4cdc30 | setSlotDamage | parse 6 floats (pos + Ypr); slot pos, Euler→slot matrix, `flags |= 0x20`; short/empty string clears `0x20` and restores type pose |
| 0x4cddc0 | setSlotPos | copy type pose, apply pos/Ypr deltas, set `0x20`, notify car physics (`FUN_004cb8d0`, `FUN_004cb6c0`) |
| 0x4cc550 | getCarRef | `P+0xcc` → typed 0xa0000000 → `+0x50` script instance |
| 0x4cd1d0 | getWear | `1.0 − P+0xb8` |
| 0x4ce390 | setWear | `P+0xb8 = 1 − v`, returns v |
| 0x4cd740 | setMaxWear | `P+0xbc = 1/v` |
| 0x4ccfc0 | getTear | `P+0xb4` |
| 0x4ce0e0 | setTear | `P+0xb4 = v`, returns v |
| 0x4ce4a0 | disableSlot | see §8 |
| 0x4ce660 | isSlotDisabled | see §8 |
| 0x4cd0d0 | getTexture | `P+0x84` |
| 0x4ce1f0 | setTexture | `FUN_004c75a0(P,id)` + request-load `FUN_0051fa40(id,7,0)`; returns id |
| 0x4cc8a0 | getMesh | `P+0x94` |
| 0x4cd840 | setMesh | `FUN_004c72d0(P,id)` + `FUN_0051fa40(id,5,0)`; returns id |
| 0x4cc9a0 | getRenderType | `P+0xa4` |
| 0x4cd9e0 | setRenderType | `FUN_004c7470(P,id)` + `FUN_0051fa40(id,0xe,0)`; returns id |
| 0x4cc6e0 | getLogo | `M+0x1120` |
| 0x4cc640 | getCar | `P+0xc8` (int id) |
| 0x4cd2e0 | getWheelID | `v = P+0xd0`; `100 < v < 401` → `(v−101)%10`, else −1 (0x65..0x190 = wheel part ids) |
| 0x4cc7d0 | getMass | `1 / (def+0x2c)` via `GameRef__getPhysBody(P)`; 0 if no body/invalid mass |
| 0x4cdb80 | setSfxLoopParams | `P+0x144 = a`, `P+0x148 = b`; returns 1 |
| 0x4cc440 | flap | `Part__flap 0x4c5ed0(P, mode)`: walks runtime slots with `flags&1`, resolves the attached part's render object (`slot+0x28`, else mate slot+0x28), manipulates flags at object `+0x88` (bits 1/2/4) per mode; returns state mask. Java: `getFlap()=flap(0)`, `toggleFlap()=flap(1)` |
| 0x4cd3a0 | install_OK | args (dest GameRef, slot, part GameRef, slot2, pos Vector3) → `int[2]` (class `[I`, len 2). Calls `Part__installSearch 0x4c96c0(P,&target,&slot2,pos)`, which searches install candidates (`FUN_004c9af0`, iterating with `FUN_0047c640` kinds 7/0x31, position field names x/y/z) and picks the nearest; result array = `{status, resolved slot/id}` |

`Part.getWheel()` in Java is `((Chassis)getCarRef()).getWheel(getWheelID())`, i.e. it goes through the
Chassis — `getWheelID` alone returns only `(id−101)%10` for ids in 0x65..0x190.

### 12.4 DynoData (`java.game.parts.DynoData`)

Payload = 0x1ec-byte struct (direct pointer via `FUN_004ed320`).

| address | native | behavior |
|---:|---|---|
| 0x4cc3b0 | newNative | if no attached native: `Mem__alloc(0x1ec)`, `EngineModel__ctor`, attach to instance; else no-op |
| 0x4cc150 | deleteNative | detach, `EngineModel__freeTables`, `Mem__free(ptr,0x1ec)` |
| 0x4cba80 | calcDyno(|steps|) | see §11 and `10-engine-dyno.md`; returns 0 |
| 0x4cc2b0 | getTorque(rpm,nitro) | lerp torque table `+0x1dc`, step `+0x1d0`, count `+0x1d4`; nitro ignored; 0 outside range |
| 0x4cc1b0 | getHP(rpm,nitro) | same on HP table `+0x1e4` |

### 12.5 SfxTable (`java.game.parts.SfxTable`)

| address | native | behavior |
|---:|---|---|
| 0x4b5980 | addItem | append resource + `1/f` + 4 params to the 16-item table (§10) |
| 0x4b5a60 | clear | `FUN_00498280(this)` resets count/items |
| 0x4b5a90 | getItems | `+0x240` count |

## 13. Java-side confirmations (2.3.1 sources)

- `Chassis` declares `max_steer = 0.7`, `wheels = 4`, `gears`, `ratio[8]`, `rearend_ratio = 3.3`,
  `ClutchF = 500`, `C_drag = 0.32`, `drag_center`, `drive_type`, `diff_lock`, `tank_nitro`,
  `consumption_nitro`, SFX fields, `rpm_trans_fwd/rev = 1400`, `sfx_starter_rpm = 200`,
  `sfx_horn_pitch = 1.0`, `suspend_update`, `AI_steerhelp`, `game_version`; comments document AI ids
  (0 throttle, 1 brake, 2 handbrake, 3 steer, 4 clutch, 5 nitro; 6 gear, 7 gear_rest, 8 horn) — exactly
  the native mapping found above.
- `Part.install_OK(GameRef dest, int slot, GameRef part, int slot2, Vector3 pos)` returns `int[]`;
  `getFlap() { return flap(0); }`, `toggleFlap() { flap(1); }`; `getSlots()` is max index + 1;
  `getSlotID(int slotIndex)` index→ID (−1 = parent slot ID); `partOnSlot(int slotID)`,
  `slotIDOnSlot(int slotID)`, `getCarRef()` returns `Part`, `getCar()` returns int,
  `getWheelID()` returns int.
- `WheelRef` setters match the native signatures: `setCPatch(halfwidth,angle,offset)`,
  `setArm(len,px,py,pz,nx,ny,nz)`, `setHub(len,p1,p2,pc)`, `setInstantCenter(Hx,Hy,Hz,Lx,Ly,Lz)`,
  `setPacejka(i,val)`.
- `DynoData`'s field list (`bore, stroke, Vmin, cylinders, in/out_min/max, time_in/out_open/close,
  time_burn, RPM_limit, time_spark_*, rpm_turbo_*, P_turbo_max/waste, mixture_H/ratio,
  max_air/fuel_consumption, intercooling, nitro_H, nitro_cooling`) matches `calcDyno`'s copy list.

## 14. Open questions

- `FUN_00468b00` is used as the static typed-data/type-entry lookup; its exact contract
  (`0x20000000/0xa0000000,0,0,0`) and the wrapper object at `+0xc` could be named more precisely.
- `Part__initInstance` keeps only the cfg rotation's up column at `+0x138..+0x140`; whether the full
  part orientation is stored elsewhere (another init path or loader) is unresolved.
- `install_OK`'s use of `dest`/`slot`/`part` during the search, and the exact meaning of the two
  returned ints (status vs resolved slot) are partly inferred from `FUN_004c96c0`.
- `flap(mode)` mode→flag transitions (modes 1..4) are only summarized; the render object `+0x88`
  flag semantics and the returned mask bits need a focused pass.
- `getCM`/`getMin`/`getMax` use the manager offset `M+0x1f1c..24` and the body parent chain; the exact
  transform and whether the resulting CM is local or world is not fully verified.
- The manager wheel-template fields beyond pos/Ypr/+0x2c/+0x38/+0x3c, and who writes them (type
  loader vs. cfg), are unknown.
- `setSlotDamage`/`getSlotDamage` assume the slot matrix at `+0x38`; this follows `setSlotPos`'s copy
  but the Euler destination is a hidden ECX argument in the decompiler.
- `M+0x1b0c..0x1b60` OSD fields are manager fields; who initializes them per car type is untraced.
- The slot-ID → render-handle table built in `Vehicle__createCar` (ids 0x65..0x190 mapped with
  constant offsets) is still unresolved; see `09-vehicle.md` open questions.

## 15. Part install: `"install ..."` command (2026-10-24, card 150 RE)

**Who installs what (scripts).** `Vehicle(parent, id, ...)` (sl/Scripts/game/Vehicle.java:107-117)
creates the car: `chassis = create(parent, GameRef(id), "0,0,0,0,0,0", "Vehicle")` returns the
car type's script instance (the Chassis, e.g. Axis_200S; the car record has `native car
_main_200S.cfg`, no `native part`). Then `chassis.addStockParts(desc)` (Axis_200S.java:52 fills
`stock_parts_list_*`, Chassis.java:776 -> Part.addPart per id). Garage purchase, freeride player
car and MapDebug all go through this ctor (Vehicle.java:91/117, MapDebug.java:361). Sub-parts:
each installed part's `addStockParts(desc)` calls `addPart` again (e.g. Duhen_D18V_block.java:99-133
crankshaft, heads, transmission ...). Load from save: Part.java:195 `attachedPart.command("install
0 <me> <otherSlotID> <me> <slotID>")` (explicit slots).
`Part.addPart(id, name, desc)` (Part.java:818): `carID = getCar()` (P+0xc8; 0 -> `this.id()`),
`rootID = GameRef(carID).getParentID()`; `part = xa.create(GameRef(rootID), GameRef(id), "", name)`
(new part in the car's parent, i.e. the world); `xa.command("install 0 " + carID + " 0 " +
this.id() + " 0")`; success = `xa.getParentID() != rootID` (install reparents), then
`part.addStockParts(desc)`, `setTexture`; failure -> `xa.destroy()`. Hungarian comment at
Part.java:194 names the 5 ints: cursor, dest ("kire"), my slot, target part ("pontosan kire"),
target slot.

**Native side, all in the exe** (`type_part__vf14` 0x4c4400, ECX = part type manager M, param_1 = P
the part instance, code 0x10):
```
"install": n = Str__scan(cmd, "%*s %d %d %d %d %d %f %f %f", &cursor, &dest, &mySlot, &target, &tSlot, &x, &y, &z)
  P+0x60 ref = dest (0x51fa40 on ECX = P+0x60)     // ResourceRef: +0 prev +4 next +8 id +0xc RI
  tref = target
  mySlot' = Part__installSearch 0x4c96c0(P, &tref, &tSlot, n >= 8 ? &pos : null)   // mySlot arg ignored
  if (!mySlot') return 0                           // nothing changes, script destroys the part
  Part__attach 0x4cb1d0(P, mySlot', &tref, tSlot)
```
`Part__installSearch 0x4c96c0(P, tref, &tSlot, pos)`: if `tref` resolves, `r = 0x4c9af0(P, tref,
&tSlot, null)`; if found return it. Else fall back to the dest object (`P+0x6c`): `getInfo(dest, 7)`
must be 9 or 5 (5 = vehicle): try `0x4c9af0(P, dest, ...)`, and for 5 also every part of the car
(`getInfo(dest, 0x31, &it)` iterator); with `pos`, pick the candidate whose target slot world
position (`0x4c6e50`) is nearest (squared distance < 1e8), without `pos` the first.

`Part__findMateSlot 0x4c9af0(P, tref, &tSlot, pos)` (ECX = M of P):
```
if (getInfo(tref, 0x27) < 1 || tref.id == P.id) return 0
T' = type manager of tref's instance (TI+0x4c); TI = tref.RI vf3(1.0)
if (tSlot == 0)                                 // auto: walk MY type slots in list order
  for each my type slot s (M+0x116c, i < M+0x1160), runtime r = P+0x78 + i*0x68:
    if ((r+8 == 0 || r.flags & 1) && !(r.flags & 0x10)):
      c = 0x4c89f0(T' ECX, TI, &M+4 /*my type ref*/, s.id, pos)
      if (c) { if (!pos) { tSlot = c; return s.id }  nearest-by-pos otherwise }
else                                            // explicit target slot
  if (0x4c9fe0(T', TI, tSlot))                  // target slot tSlot exists, free (+8 == 0), not 0x10, target not locked (0x4cb0b0)
    for each my type slot s with r+8 == 0 and !(r.flags & 0x10):
      if (0x4c9d90(&T'+4, tSlot, &M+4, s.id)) return s.id
return 0 (or the nearest)
```
`0x4c89f0(T' ECX, TI, myTypeRef, mySlot, pos)`: `if (0x4cb0b0(TI)) return 0` (target locked: some
occupied slot has flags&0x50 == 0x50, recursive up the attached chain); for each target type slot
t (list order) whose runtime slot is free (`+8 == 0`) and not disabled (`!(flags & 0x10)`): if
`0x4c9d90(&T'+4, t.id, myTypeRef, mySlot)` -> return t.id (no pos) / nearest (pos).

**Slot compatibility `0x4c9d90(refA, slotA, refB, slotB)`** (refs = the two TYPE resources, `+8` =
type id `pkg<<16|local`; sA/sB = `PartType__findSlotByID 0x4c6f90` type-slot nodes; node `+0xc4` =
`attach` list, `+0x110` = `compatible` list, entries `+0x24` type id, `+0x28` slot id):
```
if (A.id == B.id && slotA == slotB) return 0
if (sA.attach  has (B.id, slotB)) return 1
if (sB.attach  has (A.id, slotA)) return 1
if (any sA.attach entry == any sB.compatible entry (id and slot)) return 1
if (any sB.attach entry == any sA.compatible entry) return 1
return 0
```
Axis example: `_main_200S.cfg:387` slot 401 `attach 0x000A009B 99` (+5 other engines);
Duhen_D18V_block.cfg slots 46, 98, 99, 14, 47, 1, 40 ... -> engine slots 46 and 98 match nothing,
slot 99 matches chassis 401 (rule 1 from the chassis side). `compatible 0x00000006 39` (exhaust
slot 39) is the group form (rule 3/4).

**`Part__attach 0x4cb1d0(P, mySlot, tref, tSlot)`** (ECX = M):
1. `if (P+0x68) 0x4ec210(P+0x60)`: reparent P under dest (GameRef set-parent: no-op when the parent id
   already matches; moves the RI across worlds via 0x51f880 when `+0xf8` differ). Same for the
   `M+0xff8` element objects. **This reparent is what Part.addPart tests (`getParentID() != root`).**
2. `P+0x5c = 1`; `bolts = 0x4c1a10(P, mySlot, tref, tSlot, &pos, &mat)`: computes P's pose = target
   slot world pose (target body pose `+0x20c*0xd4` block ∘ target runtime slot pos/matrix) minus my
   slot offset, and returns `tTypeSlot+0x6c + myTypeSlot+0x6c` (per-slot count, very likely bolts;
   which cfg keyword fills `+0x6c` UNRESOLVED). `0x4eac50(pos, mat)` places P's body.
3. My runtime slot (mySlot): `flags |= 4` (pending), slot ref (+0..+0xc) = target RI (`+8` = target
   id), `+0x10 = tSlot`. Target runtime slot (`PartType__findRuntimeSlot 0x4c6fd0`(TI, tSlot)):
   `flags |= 4`, ref = P, `+0x10 = mySlot`.
4. `if (bolts == 0) Part__commitInstall 0x4c7d20(P)`; else the install stays pending (flag 4) until
   bolted (slot_bolt objects; not traced). `partOnSlot` (0x4cd600) reads `slot+8` only, so a
   pending part is already visible to scripts.

**`Part__commitInstall 0x4c7d20(P)`** (ECX = M): for every my runtime slot with `+8 && flags & 4`:
`flags = (flags & ~4) | 1`, `P+0x74++`; on the attached part's runtime slot `+0x10`: `flags = (flags &
~4) | 2`, `T+0x74++`, `0x4c7060(T, 1)`; `P+0xb0` = target `+0xb0` with the low/high 16 bits of the
target type slot `+0x68` overriding unless 0xffff (`0x4c9530(P)` on change); first attached part ->
local ref `tgt`. Physics: join P's body under the target body (`0x501d00`, `0x4f7b20(Pbody,
Tbody)`, compound mass `0x5032d0(Tbody)`, wake/sleep handling). `0x4cb6c0(P)`: recursively re-point
P's own installed children (flag 2 slots) to P's dest (`0x4ec210`), copy `+0x5c`. Then
`if (P+0xc8) 0x4ca980(P,0)`; **`getInfo(tgt, 0x2b, P+0xc0)` = P's car ref := the target's car**
(P+0xc8 = car id, what `Part.getCar` returns); if now set: `P+0xd0 = 0x4c8b10(P,0)` (wheel id),
`0x4ca390(P,0)`. Script callback `GameObject__callScriptMethod 0x47dd50(P inst, "install", 0x4f,
0x4f)` = `Part.install()` (Part.java:707, empty in the base). Finally clear the `P+0x60` dest ref.

Also in vf14: `"remove %d %d"` -> 0x4cb510(P) (uninstall, not traced), `"repair"`, `"texture"`,
`"add_part"`/`"rem_part"` (0x4c17b0/0x4c6990), `"filter"`, `"paint"`, `"decal"`, `"bigdecal"`,
`"colorscheme"`, `"flap_toggle"`. `install_OK` (0x4cd3a0) wraps the same `0x4c96c0` search for the
garage UI; `Part.addPart` no longer calls it (commented out in Part.java:840).
