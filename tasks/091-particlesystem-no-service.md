---
id: 091
status: implemented
branch: card/091
depends: []
---
# script: ParticleSystem init/stop/setFreq/modePermanent/delAction (service-less)

## Goal
Replace five `java.util.resource.ParticleSystem` stub rows with ports of the exe's
guard chain. src has no typed-data layer and no particle service, so the particle
object `P` (typed data `T+0x18`) can never exist here; every exe write below is
reached only when `P != 0`, so the correct behaviour in this rebuild is: validate
the argument count, then return `void` without touching state. This follows the
no-service pattern of card 085; the exact guard chain and constants are recorded in
the comments so a later particle-service card can fill in the body.

## Exe evidence (cleaned pseudocode; helpers abbreviated)
Mapping to src (established in notes/06, notes/12, notes/13 and used by the
neighbouring ResourceRef/RenderRef ports): `getArgs` 0x4b1af0 = `want()` + `get_*`;
`instanceToHandle` 0x4ecc70 = `game_rt()->handle(receiver)`;
`handle+0xc` = our `Runtime::Handle::target` (`Runtime::Resource`); make-resident
`RI->vf5(1.0f)` when `RI+0x7c != 1` and `Resource__getTyped(0xa0000001,...)` 0x51f4f0
followed by `RI->vf3(1.0f)` yield the typed data `T` (notes/12 §"typed data"); src
has no typed layer, so `T`/`P` do not exist.

### 0x4e0ad0 ParticleSystem.init(ResourceRef res, RenderRef render, String name)V
    getArgs(ctx, &self, &res, &render, &name);            // 0x4b1af0
    h = instanceToHandle(self);                            // 0x4ecc70
    if (h != 0 && res != 0 && render != 0)
        ParticleSystemEngine__init(res, render, 0, 0, name, 0.0f);   // 0x4e8b50

    0x4e8b50 (helper, not ported) with ECX = h:
      if (!res || !render) return 0;
      R = render->target; if (!R) return 0;                // render+0xc
      if (R+0x7c != 1) R->vf5(1.0f);                       // 1.0f = 0x3f800000
      if (getTyped(R, 0xa0000001, 0, 1.0f) < 0) return 0;  // 0x51f4f0
      T = R->vf3(1.0f); if (!T) return 0;
      typeData = T+0x28; if (!typeData) return 0;
      u = 0x524840(res, render, name, 0); 0x40cac0(u); 0x459290(u+0xb0);
      psT = getTyped(ECX, 0xa0000001, ...); if (!psT) return 0;   // see UNRESOLVED
      P = 0x51bae0(typeData, 0, name);
      psT+0x16c |= 0x8000;
      psT+0x18 = P;

### 0x4e1440 ParticleSystem.stop()V
    getArgs(ctx, &self);
    h = instanceToHandle(self);
    if (h != 0) ParticleSystem__stop(h);        // 0x4ec720, ECX = h

    0x4ec720 (helper) with ECX = h:
      R = h->target; if (!R) return;              // h+0xc
      if (R+0x7c != 1) R->vf5(1.0f);
      if (getTyped(R, 0xa0000001, 0, 0) < 0) return;
      T = R->vf3(1.0f); if (!T) return;
      P = T+0x18; if (!P) return;
      P+0x9c |= 0x40000000;

### 0x4e10c0 ParticleSystem.setFreq(F freq)V
    getArgs(ctx, &self, &freq);
    h = instanceToHandle(self);
    if (!h) return; R = h->target; if (!R) return;
    make resident (R+0x7c != 1 -> R->vf5(1.0f));
    if (getTyped(R, 0xa0000001, 0, 0) < 0) return;
    T = R->vf3(1.0f); if (!T) return;
    P = T+0x18; if (!P) return;
    P+0xc = freq;                                 // raw float bits, movss

### 0x4e0b30 ParticleSystem.modePermanent(I mode)V
    getArgs(ctx, &self, &mode);
    h = instanceToHandle(self);
    if (!h) return; R = h->target; if (!R) return;
    make resident; if (getTyped(R, 0xa0000001, 0, 0) < 0) return;
    T = R->vf3(1.0f); if (!T) return;
    P = T+0x18; if (!P) return;
    if (mode == 0)      P+0x9c &= 0xdfffffffu;    // clear 0x20000000
    else if (mode == 1) P+0x9c |= 0x20000000u;
    // other values: unchanged (asm: sub ecx,0 / je; sub ecx,1 / jne ret)

### 0x4e0970 ParticleSystem.delAction(String name)V
    getArgs(ctx, &self, &name);
    h = instanceToHandle(self);
    if (!h) return; R = h->target; if (!R) return;
    make resident; if (getTyped(R, 0xa0000001, 0, 0) < 0) return;
    T = R->vf3(1.0f); if (!T) return;
    P = T+0x18; if (!P) return;
    node = FUN_004b1060(P, name);                 // ECX = P
    if (node) node->vf0(1);                       // (**(code**)*node)(1)

    0x4b1060 (helper) with ECX = P:
      node = P+0x24;                              // list head
      while (node) { if (stricmp_0x57c11e(node+0x2c, name) == 0) return node;
                     node = node->next (node+4); }
      return 0;

## UNRESOLVED
- 0x4e8b50, second typed fetch: ECX is the caller's `0x4ecc70` payload
  (`8b 4d fc` / `68 01 00 00 a0` / `e8 ... call 0x51f4f0`), but notes/12 documents
  0x51f4f0 as taking the RI (`R+0x7c`/`R+0x94`), while the other natives load
  `payload+0xc` first. Does not affect this no-op card.
- 0x524840 / 0x468b00 / 0x51bae0 operate on particle-engine data not identified
  here; `typeData = T+0x28` and `P = T+0x18` meanings are engine-side only.
- 0x4b1060 list head `P+0x24` and node name `+0x2c` are read from decompiled code
  only, not from a traced node ctor (no action ctor is reachable without the service).

## Changes
1. `src/script/natives.cpp`, next to the `anim_*` functions: add five functions
   `ps_init`, `ps_stop`, `ps_set_freq`, `ps_mode_permanent`, `ps_del_action`.
   Each body:
   ```cpp
   bool ps_stop(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("ParticleSystem.stop", a, 1, err)) return false;
       *out = Value::make_null();
       return true;
   }
   ```
   with `want(..., N, ...)` where N = 1 + parameter count: init 4, stop 1,
   setFreq 2, modePermanent 2, delAction 2.
   Comment block per function: `// 0xADDR ParticleSystem.name(desc): <guard chain>`
   plus one of:
   - init: `// SERVICE MISSING: 0x4e8b50 creates the particle system through
     0x524840 / 0x40cac0 / 0x459290 / 0x468b00 / 0x51bae0 and stores it at typed
     +0x18 (typed+0x16c |= 0x8000); none of these services exist in src.`
   - stop: `// SERVICE MISSING: writes P+0x9c |= 0x40000000; P = typed+0x18 does not
     exist without the 0x4e8b50 service.`
   - setFreq: `// SERVICE MISSING: writes P+0xc = freq (P+0x18 typed slot absent).`
   - modePermanent: `// SERVICE MISSING: P+0x9c bit 0x20000000 (0/1 only; other
     values unchanged). P absent.`
   - delAction: `// SERVICE MISSING: 0x4b1060 walks P+0x24 and destroys a name match
     (0x57c11e stricmp); P absent here.`
   Note: `getArgs` copies slots without type checks, so `want()` (arity) alone is
   the src equivalent for these no-service bodies; do not add new state fields and
   do not invent a particle object.
2. Registry rows (currently `nullptr, true`): point each row at its function and
   set the last field to `false`.
   Exact rows: `init (Ljava.util.resource.ResourceRef;Ljava.util.resource.RenderRef;Ljava.lang.String;)V`,
   `stop ()V`, `setFreq (F)V`, `delAction (Ljava.lang.String;)V`,
   `modePermanent (I)V`.

## Files
- edit: `src/script/natives.cpp` (five functions near `anim_*`; the five ParticleSystem rows)
- read only: `src/script/natives.h` (Entry), `tasks/085-groundref-no-service.md` (style)

## Done when
- each of the five `ParticleSystem` rows has a function and the last field `false`;
  the other four ParticleSystem rows (`setDirectSource`, `setSource`, `setCounter`,
  `getCounter`) stay `nullptr, true`
- each function carries its `// 0xADDR` comment; the missing services are
  `SERVICE MISSING:` with the offsets/constants above; any deviation is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
