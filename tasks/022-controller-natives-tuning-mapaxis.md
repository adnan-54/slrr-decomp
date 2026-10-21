---
id: 022
status: todo
branch:
depends: [021]
---
# input: Controller tuning natives + Input.mapAxis / Input.setAxisSmooth

## Goal
Make `Controller.user_SetAxisSmooth`, `user_SetAxisSpeed`, `user_SetAxisForce`
and the static `Input.mapAxis`, `Input.setAxisSmooth` non-stubs. All use the
`controller_of(Runtime::Resource*)` helper from card 021 and the already ported
`input::Controller` setters; glue only.

## Exe evidence (cleaned pseudocode, renamed fields)
Controller natives resolve `C` exactly as in card 021 (receiver -> handle -> ri ->
typed -> `*(typed+0x4c)+0x1c`); unresolved -> return DEFAULT.
AxisRecord `A = C + 4 + vaxis*0x30` (our `input::AxisRecord`).

### 0x4d4080 Controller.user_SetAxisSmooth(int vaxis, float a, float b, float c, float d, float e) -> int
    C->set_axis_smooth(vaxis, a, b, c, d, e);      // 0x5660f0
    return 1;                                       // always 1 (also when unresolved)
0x5660f0 writes: A+0x0c center_range=a, A+0x10 center_factor=b, A+0x14 opposite=c,
A+0x18 same=d, A+0x20 power=e  (= `Controller::set_axis_smooth` argument order).

### 0x4d4180 Controller.user_SetAxisSpeed(int vaxis, float s) -> void
    C->set_axis_speed(vaxis, s);                    // 0x566140: A+0x1c speed

### 0x4d3fb0 Controller.user_SetAxisForce(int vaxis, float c, float f) -> void
    C->set_axis_force(vaxis, c, f);                 // 0x5660c0: A+0x24 force_c, A+0x28 force_f
    (native returns 1 in eax; Java sig is V)

### 0x4dea20 Input.mapAxis(GameRef inst, int l_axis, int device, int i_axis, float i_from, float i_to, float l_from, float l_to) -> void   (static)
    Native__getArgs(frame, 0, &inst, &l_axis, &device, &i_axis, &i_from, &i_to, &l_from, &l_to);
    C = Game(0x5fde90).getInfo(inst, 0x1b, 0);      // 0x47c640: instance -> typed getInfo(0x1b) = its controller block
    C->add(l_axis, device, i_axis, i_from, i_to, l_from, l_to);   // 0x565aa0, result discarded
No null check in the exe (null C would crash).

### 0x4deab0 Input.setAxisSmooth(GameRef inst, int l_axis, float a, float b, float c, float d) -> void   (static)
    C = Game.getInfo(inst, 0x1b, 0);
    C->set_axis_smooth(l_axis, a, b, c, d, 1.0f);   // 0x5660f0 with power fixed to 1.0

## Mapping to our model
- getInfo(inst, 0x1b) on a Controller instance == `controller_of(value_resource(a[0]))`
  (same block the Controller natives use).
- INVENTED: when `inst` does not resolve (null GameRef / no runtime) mapAxis and
  setAxisSmooth do nothing (the exe would dereference null).

## Files
- edit: src/script/natives.cpp — five natives with `// 0xADDR Class.name` comments; switch rows
  `user_SetAxisSmooth (IFFFFF)I`, `user_SetAxisSpeed (IF)V`, `user_SetAxisForce (IFF)V`,
  `Input.mapAxis (Ljava.util.resource.GameRef;IIIFFFF)V`,
  `Input.setAxisSmooth (Ljava.util.resource.GameRef;IFFFF)V` to `<fn>, false`.
  Use `get_i`/`get_f` for args. Note `Controller::set_*` already reject vaxis outside 0..75
  (keep that guard; it is the existing INVENTED bounds check, the exe has none).
- read only: src/input/controller.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
