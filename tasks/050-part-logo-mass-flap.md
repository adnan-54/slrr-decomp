---
id: 050
status: implemented
branch: card/050
depends: []
---
# Part: getLogo / getMass / flap natives

## Goal
Replace the three `java.game.parts.Part` engine stubs with the exe behaviour:
`getLogo()` reads the part type manager (`M+0x1120`), `getMass()` returns
`1 / inverse mass` of the part's physics body (`P+0x1c` → body → def `+0x2c`), and
`flap(mode)` walks the runtime slots and toggles state bits on the attached part's
bone/render object (`slot+0x28`, flags `+0x88`).

Our rebuild does not model the typed type manager or per-slot bone/render objects, so
`getLogo` and `flap` take the exe's own failure return (0); `getMass` gets its inverse
mass from the cfg `body` line (the same mass the exe passes to `Physics__createBody`,
whose inverse mass is `1/m`). Do not touch the `bodypart.Chassis` `getMass` row
(a different native, 0x4b2b90).

## Exe evidence (cleaned pseudocode)

Shared native prologue of all three: `Native__getArgs 0x4b1af0` unpacks the receiver,
then `Native__instanceToHandle 0x4ecc70(self, DAT_007a2620)` gives a handle whose
`+0xc` is the resource instance `RI`. In src this is already `part_of(a)`
(`src/script/natives.cpp:2338`): do not re-implement the chain. `RI->vf5` = vtable
`+0x14`, `RI->vf3` = vtable `+0xc`; `Resource__getTyped` = 0x51f4f0.

### 0x4cc6e0 Part.getLogo()I
```
handle = instanceToHandle(self);            // 0x4b1af0 + 0x4ecc70; null -> 0
RI = handle->0xc;                           // null -> 0
if (RI->0x7c != 1) RI->vf5(1.0f);           // make resident
if (Resource__getTyped(RI, 0xa0000000, 0, 0) < 0) return 0;
T = RI->vf3(1.0f);                          // null -> 0
S = T->0x44;                                // secondary resource instance; null -> 0
if (S->0x7c != 1) S->vf5(1.0f);
if (Resource__getTyped(S, 0x20000000, 0, 0) < 0) return 0;
W = S->vf3(1.0f);                           // null -> 0
M = W->0xc;                                 // type manager
return M->0x1120;                           // manufacturer/logo id (int)
```
Every failure path returns 0. Note `bin/the game/parts/scripts/Part.java:730` implements
`getLogo()` as a Java body (`return manufacturer`, default `MF_STOCK = 0`), so this
native is unreachable from script calls in 2.3.1; the registration still exists.

### 0x4cc7d0 Part.getMass()F
```
handle = instanceToHandle(self);            // null -> 0
RI = handle->0xc;                           // null -> 0
if (RI->0x7c != 1) RI->vf5(1.0f);
if (Resource__getTyped(RI, 0xa0000000, 0, 0) < 0) return 0;
T = RI->vf3(1.0f);                          // null -> 0
body = GameRef__getPhysBody(T->0x4c + 0x1c);   // 0x4eabc0; null -> 0
def = body->0xb4->0x5c;
inv_mass = def->0x2c;
if (!(inv_mass > 0.0f)) return 0;           // COMISS/JBE at 0x4cc878
return 1.0f / inv_mass;                     // DIVSS const at 0x593598 (1.0f, notes/13 §12.3)
```

#### helper 0x4eabc0 GameRef__getPhysBody, this = P+0x1c (16-byte ref node: id +0x8, RI +0xc)
```
if (this->0x8 == 0) return 0;
RI = this->0xc;                             // null -> 0
if (RI->0x7c != 1) RI->vf5(0);              // arg 0 here, not 1.0f
if (Resource__getTyped(RI, 0xa0000000, 0, 0) < 0) return 0;
body = RI->vf3(1.0f);                       // null -> 0
if (body->0xb4 != 0) return body;           // +0xb4 = body-info record
FUN_0047b970(this);                          // only when +8 and +0xc are both set
return 0;
```

### 0x4cc440 Part.flap(I)I
```
handle = instanceToHandle(self);            // null -> -1
RI = handle->0xc;                           // null -> -1
if (RI->0x7c != 1) RI->vf5(1.0f);
if (Resource__getTyped(RI, 0xa0000000, 0, 0) < 0) return -1;
T = RI->vf3(1.0f);                          // null -> -1
S   = T->0x44;                              // null -> -1
P   = T->0x4c;
if (S->0x7c != 1) S->vf5(1.0f);
if (Resource__getTyped(S, 0x20000000, 0, 0) < 0) return -1;
W = S->vf3(1.0f);                           // null -> -1
M = W->0xc;                                 // ECX for the helper
return Part__flap(P, mode);                 // 0x4c5ed0, thiscall this = M
```

#### helper 0x4c5ed0 Part__flap(P, mode), this = M
```
slots = P->0x78;                            // runtime slot array, stride 0x68
// iVar8 walks M->0x116c (type-slot list) but is never read in the body.
n = M->0x1160;                              // slot count (getSlots)
for (i = 0; i < n; i++, slots += 0x68) {
    if (!(slots->0x14 & 1)) continue;       // slot present
    if (slots->0x8 == 0) continue;          // attached game object
    RIa = slots->0xc; if (!RIa) continue;   // attached Part script instance
    if (RIa->0x7c != 1) RIa->vf5(1.0f);
    if (Resource__getTyped(RIa, 0xa0000000, 0, 0) < 0) continue;
    Ta = RIa->vf3(1.0f); if (!Ta) continue;
    Sa = Ta->0x44; Pa = Ta->0x4c;
    if (!Sa) continue;
    if (Sa->0x7c != 1) Sa->vf5(1.0f);
    if (Resource__getTyped(Sa, 0xa0000000, 0, 0) < 0) continue;   // 0xa0000000, not 0x20000000
    Wa = Sa->vf3(1.0f); if (!Wa) continue;  // only a readiness check; Wa is unused

    obj = slots->0x28;                      // attached bone/trigger physics object
    if (!obj) {
        mate = PartType__findRuntimeSlot(Pa, slots->0x10);   // 0x4c6fd0
        if (mate) obj = mate->0x28;
    }
    if (!obj) continue;

    f     = obj->0x88;
    bit0  = f & 1;
    result = (bit0 ? 1 : 2) | ((f & 2) ? 4 : 0) | ((f & 4) ? 8 : 0);
    switch (mode) {
    case 1: if (bit0) { FUN_004b1030(); obj->0x88 |= 2; }
            else       obj->0x88 = (f & ~2) | 4;
            return result;
    case 2: if (bit0) FUN_004b1030();
            obj->0x88 = (obj->0x88 & ~4) | 2;   // write is unconditional
            return result;
    case 3: if (bit0 == 0) {
                // the "if (slot flags & 1 == 0) P = Pa" branch is dead: the
                // outer condition already requires slot 0x14 bit 0.
                FUN_004cb8d0(P); FUN_004b10c0(0); FUN_004b1770();
            }
            return result;
    case 4: if (bit0 == 0) obj->0x88 = (f & ~2) | 4;
            return result;
    default: return result;                 // mode 0 and any other value
    }
}
return 0;                                   // no qualifying slot
```

## Files
- edit: `src/script/natives.cpp` — add `pt_get_logo`, `pt_get_mass`, `pt_flap` near
  `pt_get_car`/`pt_set_render_type`; replace the three registry rows at lines 4377,
  4380, 4382 from `nullptr, true` to these functions with `false`.
- edit: `src/game/car.h` — add to `struct PartData`:
  ```cpp
  float inv_mass = 0.0f;  // P+0x1c body -> body+0xb4 -> def+0x5c -> +0x2c (inverse mass)
  ```
- edit: `src/game/car.cpp` — in `fill_part`, after the texture/mesh/render block:
  ```cpp
  // INVENTED: which cfg body line P+0x1c binds to is UNRESOLVED; use the first.
  // 0x4bf4d0 passes this mass to Physics__createBody, whose inverse mass is 1/m.
  if (!cfg.bodies.empty() && cfg.bodies[0].mass > 0.0f)
      p.inv_mass = 1.0f / cfg.bodies[0].mass;
  ```

Implementation shape:
```cpp
// 0x4cc6e0 Part.getLogo
bool pt_get_logo(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getLogo", a, 1, err)) return false;
    // shortcut: no typed type manager (M+0x1120) in this rebuild; every 0x4cc6e0
    // failure path returns 0. Java Part.getLogo() (Part.java:730) shadows it.
    *out = Value::make_int(0);
    return true;
}

// 0x4cc7d0 Part.getMass
bool pt_get_mass(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("Part.getMass", a, 1, err)) return false;
    game::PartData *p = part_of(a);
    float inv = p ? p->inv_mass : 0.0f;
    *out = Value::make_float(inv > 0.0f ? 1.0f / inv : 0.0f);  // JBE 0x4cc878
    return true;
}

// 0x4cc440 Part.flap
bool pt_flap(Vm &, const Args &a, Value *out, std::string *err)
{
    int32_t mode;
    if (!want("Part.flap", a, 2, err) || !get_i("Part.flap", a[1], &mode, err))
        return false;
    (void)mode;
    // shortcut: no per-slot bone/render object (slot+0x28, flags +0x88) nor the
    // 0x4b1030/0x4cb8d0/0x4b10c0/0x4b1770 trigger calls; the exe falls through the
    // 0x4c5ed0 slot loop and returns 0. Upgrade when part physics/render bones exist.
    *out = Value::make_int(0);
    return true;
}
```

## UNRESOLVED (kept from the trace)
- Which cfg `body` line the `P+0x1c` ref node binds to (first line is an INVENTED choice).
- `FUN_004b1030`, `FUN_004cb8d0`, `FUN_004b10c0`, `FUN_004b1770` side effects not
  traced (raw lines above); unreachable in this port because no slot has a `+0x28` object.
- Creation/initialisation of the `+0x28` bone/trigger object and the initial value of
  its `+0x88` flag word.
- Who loads the typed type manager and writes `M+0x1120` (the part type data loader).
- The `1.0f` at `0x593598` is taken from notes/13 §12.3 (DIVSS operand; byte read not
  available in this session).

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
