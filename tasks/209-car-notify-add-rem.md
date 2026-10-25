---
id: 209
status: implemented
branch: card/209
depends: []
---
# Car: `notify` type keyword + `car_add` / `car_rem` command messages to the parent

## Goal
Port the exe-side half of the traffic/police tracking messages (notes/25 §2). A car type whose cfg
has the keyword `notify` (shipped: only `cars/misc/Police_V3/scripts/_main_.cfg`) makes every
created car instance send `"car_add <id> 0"` to its parent instance, and its destruction send
`"car_rem <id> 0"`, both as EVENT_COMMAND (0x10). City.java:3082-3180 consumes them. No traffic
manager work (§2.4 stays unported).

## Exe evidence (cleaned pseudocode; M = car type record, T = typed car instance)
### 0x48fb10 cfg keyword `notify` (keyword table 0x5da880 -> string 0x59d5f8 "notify")
```
void notify_kw(ctx, M) {
    if (M+0x120 == 0)          // src treats M+0x120 as 0 (see fill_car `controller` comment)
        M+0x2ec0 |= 2;         // the "notify" flag; type_car vf1 0x49ab20 sets M+0x2ec0 = 0 first
}
```
### 0x4992e0 Vehicle__createCar, tail (after the wheel/part setup, just before return)
```
if ((M+0x2ec0 & 2) && (T.id & 0xffff0000) != 0xffff0000 && T.parent != 0) {
    sprintf(buf, "car_add %d %d", T.id, 0);
    Game__queueEvent(T.parent, sender 0, 0x10, buf);     // 0x47d550, synchronous in src
}
```
### 0x49beb0 type_car vf4 (instance dtor), first statement
```
if ((M+0x2ec0 & 2) && (T.id >> 16) != 0xffff && T.parent != 0) {
    sprintf(buf, "car_rem %d %d", T.id, T+0x2054);      // T+0x2054 = traffic car ptr; src has none -> 0
    Game__queueEvent(T.parent, 0, 0x10, buf);
}
... (rest of the dtor = existing src instance_dtor body)
```
`%d` of the id: the exe prints the uint32 id as signed int (ids 0x8000xxxx come out negative);
use `%d` with `(int32_t)id` like the exe.

## Files
- edit: src/game/car.h (CarData: `bool notify = false;  // M+0x2ec0 bit 2`)
- edit: src/game/car.cpp
  - `fill_car`: `if (ln.key == "notify") { c.notify = true; continue; }` with `// 0x48fb10` comment.
  - `create_car_bodies`: at the end of the `native car` path (after fill_car ran and the bodies
    exist, ~line 1514 before the final sleep is fine) send car_add via
    `rt.queue_event(inst->parent, nullptr, 0x10, Value::make_str(buf))` when `car->notify`,
    `(inst->id >> 16) != 0xffff` and `inst->parent`.
  - `instance_dtor`: same test + `car_rem` at the top, BEFORE the bodies are removed; replace the
    wrong comment near line 1663 ("id>>16 == 0xffff ... not ported") with `// 0x49beb0 head`.
- read only: src/game/runtime.h (`queue_event`, `Resource::id/parent`).

## Done when
- each ported piece has its `// 0xADDR` comment; T+0x2054 = 0 is commented
  `// 0x49beb0: T+0x2054 traffic car, none in src (traffic cars not ported)`.
- the src `create_car_bodies` early return for cfgs without `body` lines skips car_add; mark that
  `INVENTED:` (exe createCar always reaches the tail; every shipped car type has body lines).
- `make -C src clean check` green; no new unit tests or hand-made expected values.
- card frontmatter: status implemented, branch name.
