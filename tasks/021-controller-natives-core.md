---
id: 021
status: merged
branch: card/021
depends: [020]
---
# input: Controller natives user_Add / user_Del / user_Reset / user_GetAxisVal

## Goal
`java.io.Controller` is a GameType whose native instance (created by
`createNativeInstance(parent, 0x32 "controller", ...)`) owns one controller block.
Add the per-instance controller lookup and make `user_Add`, `user_Del`,
`user_Reset`, `user_GetAxisVal` non-stubs. The controller logic is already ported
in `input::Controller` (`add/remove/reset/get_axis_val`, verified against
0x565aa0/0x565f60/0x566030/0x565b60); this card is glue only.

## Exe evidence (cleaned pseudocode, renamed fields)
Every Controller native starts with the same resolution (shown once):

    Native__getArgs(frame, &this, &args...);
    h = Native__instanceToHandle(this, GameRefClass);   // 0x4ecc70 -> our value_resource(a[0])
    if (!h) return DEFAULT;
    ri = h->ri;                     // +0xc
    if (!ri) return DEFAULT;
    if (ri->state != 1) ri->load(1.0f);          // vtbl+0x14
    if (Resource__getTyped(ri, 0xa0000000) < 0) return DEFAULT;   // 0x51f4f0
    typed = ri->typed(1.0f);                      // vtbl+0xc
    if (!typed) return DEFAULT;
    C = *(typed + 0x4c) + 0x1c;   // controller block: +0 count, +4 AxisRecord[76], +0xe44 Mapping[152]
    ... call the controller method with ECX = C

### 0x4d3cb0 Controller.user_Add(int vaxis, int device, int paxis, float a, float b, float c, float d) -> int
    DEFAULT = 0
    return C->add(vaxis, device, paxis, a, b, c, d);     // 0x565aa0 = Controller::add

### 0x4d3db0 Controller.user_Del(int vaxis, int device, int paxis) -> int
    DEFAULT = 0
    C->remove(vaxis, device, paxis);                    // 0x565f60
    return 0;                                           // always 0, even when resolved

### 0x4d3f10 Controller.user_Reset() -> void
    C->reset();                                         // 0x566030 (native returns 1 in eax; Java sig is V)

### 0x4d3e60 Controller.user_GetAxisVal(int vaxis) -> float
    DEFAULT = 0.0f
    return C->get_axis_val(vaxis);                      // 0x565b60

## Mapping to our model
- Our `Runtime::Resource` (the receiver's handle target, `value_resource(a[0])`)
  stands for `ri`/`typed`. Keep one `input::Controller` per Resource:
  ```cpp
  // INVENTED: the exe allocates the block with the typed "controller" object; we
  // create it on first use with reset defaults (AxisRecord/Mapping initializers
  // equal 0x566030's values).
  input::Controller *controller_of(Runtime::Resource *r);   // null r -> null
  ```
  implemented with a file-static `std::map<Runtime::Resource *, std::unique_ptr<input::Controller>>`,
  each constructed as `input::Controller(input::devices())`.
  shortcut-comment: entries are never freed (no instance-destroy hook yet).
- `controller_of` will also be used by cards 022 and 023 (Input.mapAxis /
  setAxisSmooth / checkHotkeys resolve the same block through the GameRef they get),
  so put it near the other input natives and keep it non-static-free (anonymous namespace is fine).

## Files
- edit: src/script/natives.cpp — add `#include "input/controller.h"`, `controller_of`,
  four natives with `// 0xADDR Controller.name` comments, and switch rows
  `user_Add (IIIFFFF)I`, `user_Del (III)I`, `user_Reset ()V`, `user_GetAxisVal (I)F`
  from `nullptr, true` to `<fn>, false`. Return `make_int`/`make_float`/`make_null`
  per the evidence above (unresolved receiver -> DEFAULT, not an error).
- read only: src/input/controller.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
