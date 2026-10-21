---
id: 008
status: implemented
branch: card/008
depends: [005, 004]
---
# natives: DynoData.newNative/deleteNative/getTorque/getHP + payload binding

## Goal
Bind an `EngineModel` (card 005, `src/vehicle/dyno.h`) to a `java.game.parts.DynoData`
script object the way WheelRef wrappers are bound in `src/game/car.*`, and replace
4 of the 5 DynoData stubs. `calcDyno` is card 009. These are **instance** natives:
`a[0]` is the receiver object, Java arguments start at `a[1]`.
(Depends on 004 only because both edit `src/script/natives.cpp`.)

## Exe evidence (cleaned pseudocode, renamed fields)
`Native__instanceToHandle 0x4ecc70(inst, DAT_007a2620)` reads the native pointer;
`FUN_004ed320(inst, DAT_007a2620, ptr)` stores it. Here: `game::dyno_data(obj)` /
`game::attach_dyno(obj)` / `game::detach_dyno(obj)`.

### 0x4cc3b0 DynoData.newNative()
```c
if (!instanceToHandle(self)) {
    EngineModel *e = new(Mem__alloc(0x1ec)) EngineModel;   // ctor 0x4f44a0 only, no setDefaults
    setHandle(self, e);
}
```
### 0x4cc150 DynoData.deleteNative()
```c
EngineModel *e = instanceToHandle(self);
if (e) { e->freeTables(); Mem__free(e, 0x1ec); }   // 0x4f44e0, 0x56a42f
setHandle(self, NULL);
```
### 0x4cc2b0 DynoData.getTorque(float rpm, float nitro) / 0x4cc1b0 getHP(rpm, nitro)
Identical except the table: getTorque reads `torque_table[0]` (+0x1dc), getHP
reads `power_table[0]` (+0x1e4, watts). `nitro` is read and ignored.
```c
float result = 0.0f;
if (!find_class("java.lang.Native")) return 0.0f;      // FUN_00549d60
EngineModel *e = instanceToHandle(self);
if (!e || !(0.0f <= rpm)) return 0.0f;                 // NaN -> 0
int i = 1;
if (1 < e->count) {
    int x = e->step;
    while ((float)x <= rpm) {
        i++; x += e->step;
        if (e->count <= i) return result;              // 0: rpm >= (count-1)*step
    }
    const float *t = table;                            // torque_table[0] / power_table[0]
    result = ((rpm - (float)((i - 1) * e->step)) * (t[i] - t[i - 1])) / (float)e->step + t[i - 1];
}
return result;
```
`// INVENTED: also return 0 when the table vector is shorter than count` (the exe
would read whatever the pointer holds; after calcDyno sizes always match).

## Files
- edit: `src/game/car.h` — forward-declare `struct EngineModel;` (global namespace,
  before `namespace game`), and in `namespace game` next to `bind_wheel/wheel_data`:
  ```cpp
  // DynoData native handle (Native__instanceToHandle 0x4ecc70 / FUN_004ed320).
  EngineModel *dyno_data(Object *wrapper);
  EngineModel *attach_dyno(Object *wrapper);  // creates if missing, returns it
  void detach_dyno(Object *wrapper);          // frees it
  ```
- edit: `src/game/car.cpp` — `#include "vehicle/dyno.h"`; add
  `std::map<Object *, std::unique_ptr<EngineModel>> g_dynos;` next to `g_wheels`;
  implement the three functions; add `g_dynos.clear();` to `reset()`.
- edit: `src/script/natives.cpp` — `#include "vehicle/dyno.h"`; add
  `dd_new_native`, `dd_delete_native`, `dd_get_torque`, `dd_get_hp` in a
  `// ---- java.game.parts.DynoData ----` section near the WheelRef natives
  (`grep -n 'bool wr_set_opp_wheel' src/script/natives.cpp` and add after that function).
  Use `receiver_object`, `want`, `get_f`; `vm.find_class("java.lang.Native")` for the
  class check; share one helper `dyno_lerp(const EngineModel&, const std::vector<float>&, float rpm)`.
  void natives set `*out = Value::make_null()`, getTorque/getHP `Value::make_float`.
  Flip the 4 registry rows (`grep -n 'java.game.parts.DynoData' src/script/natives.cpp`)
  to `<fn>, false`. Leave `calcDyno` as a stub.

## Done when
- each native has a `// 0xADDR DynoData.name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
