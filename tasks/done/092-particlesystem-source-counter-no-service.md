---
id: 092
status: merged
branch: card/092
depends: []
---
# script: ParticleSystem setDirectSource/setSource/setCounter/getCounter (service-less)

## Goal
Replace the four remaining `java.util.resource.ParticleSystem` stub rows with ports
of the exe's guard chain, in the same no-service style as card 091. src has no
typed-data layer and no particle service, so the particle object `D = typed+0x18`
can never exist here. Every exe read/write below is reached only when `D != 0`.
The correct behaviour in this rebuild is therefore: validate the argument count
(`want()`), then return `void` — except `getCounter`, which returns `int 0`
(the exe's value on every guard failure). The exact guard chain, action type
codes and constants are recorded in the comments so a later particle-service
card can fill in the body.

## Exe evidence (cleaned pseudocode, renamed fields)
Mapping already used by the neighbouring ResourceRef/RenderRef ports (notes/06,
notes/12, notes/13) and by card 091:
`getArgs` 0x4b1af0 = `want()` + `get_*`; `instanceToHandle` 0x4ecc70 =
`game_rt()->handle(receiver)`; `handle+0xc` = `Runtime::Handle::target`
(`Runtime::Resource`); `getTyped` 0x51f4f0(0xa0000001,0,0) + `RI->vf3(1.0f)` =
the typed data `T`; `T+0x18` = the particle object `D`. src has no typed layer,
so `T`/`D` do not exist.

`getArgs` slot order (confirmed in the asm of 0x4e0a10/0x4e0bf0/0x4e0de0): the
exe writes the **receiver** into the first output slot, then the declared
parameters in order (pre-loop `*slot = args[count-1]`, then the signature walk).
So the declared arity is 1 receiver + the Java parameters:
setDirectSource/setSource 10 slots, setCounter 4, getCounter 2.

Helpers: `list_find` 0x4b1060 with ECX = `D`: node = `D+0x24`, walk `node+0x4`
(next), name at `node+0x2c` compared by `0x57c11e` (stricmp); `getF` 0x4ec980
(missing/non-float field → 0); `alloc` 0x567a20. Vector3 field-name strings are
`"x"`/`"y"`/`"z"` at 0x5a53a0/0x5a53c4/0x5a53cc (v1), 0x5a53d8/0x5a53fc/0x5a5454
(v2), 0x5a5abc/0x5a5ac4/0x5a5adc (v1), 0x5a5b10/0x5a5b44/0x5a5b54 (v2),
0x5a5f98/0x5a5fac/0x5a5ffc (setCounter pos) — all read as addresses, verified
with read_bytes.

### 0x4e0de0 ParticleSystem.setDirectSource(String,Vector3,F,F,Vector3,F,F,F,String)V
    getArgs(ctx, &self, &name, &v1, &f1, &f2, &v2, &f3, &f4, &f5, &boneName);
    h = instanceToHandle(self);                                  // 0x4ecc70
    if (!h) return;
    R = h->target; if (!R) return;                               // h+0xc
    if (R+0x7c != 1) R->vf5(1.0f);                               // 1.0f = 0x3f800000
    if (getTyped(0xa0000001,0,0) < 0) return;                    // 0x51f4f0, ECX=R
    T = R->vf3(1.0f); if (!T) return;
    D = T+0x18; if (!D) return;
    x1 = getF(v1,"x"); y1 = getF(v1,"y"); z1 = getF(v1,"z");     // 0x4ec980
    x2 = getF(v2,"x"); y2 = getF(v2,"y"); z2 = getF(v2,"z");
    bone = boneName ? RenderInst__getBoneId(name=boneName) : 0;  // 0x4e77f0, ECX=h
    act = list_find(D, name);                                    // 0x4b1060
    if (!act) {
        node = alloc(0x7c); if (!node) return;                   // 0x567a20
        a1 = Vector3From(&x1, f1, f2);                           // 0x470410
        a2 = Vector3From(&x2, f3, f4);                           // 0x470410
        act = create_direct_source(node, name, f5, a1, a2, bone); // 0x46fa70
        if (act) link act into list at D+0x44 / D+0x4c;
    } else if (act->[0x28] == 0xb) {                             // 11 = direct source
        setA(act, Vector3From(&x1, f1, f2), bone);               // 0x4d5c20
        setB(act, Vector3From(&x2, f3, f4));                     // 0x4d5c50
        act->[0x6c] = f5;                                        // MOVSS
    }
    // other action type: returns unchanged (0x4e0f3d CMP 0xb / JNZ end)

Note: only `v.x` of each input Vector3 reaches the built vectors; `y1/z1/y2/z2`
are read by 0x4ec980 and discarded (dead in 0x4e0de0).

### 0x4e1160 ParticleSystem.setSource(String,Vector3,F,F,Vector3,F,F,F,String)V
Same guard chain and same slot/field layout as 0x4e0de0, with:
    act = list_find(D, name);
    if (!act) {
        node = alloc(0x80); if (!node) return;
        a1 = Vector3From(&x1, f1, f2);                           // 0x470410
        a2 = Vector3From(&x2, f3, f4);
        act = create_source(node, name, f5, a1, a2, bone);       // 0x46ffb0
        if (act) link act into list at D+0x44 / D+0x4c;
    } else if (act->[0x28] == 10) {                              // 0xa = source
        setA(act, Vector3From(&x1, f1, f2), bone);               // 0x4d5c20
        setB(act, Vector3From(&x2, f3, f4));                     // 0x4d5c50
        act->[0x6c] = f5;
    }

### 0x4e0bf0 ParticleSystem.setCounter(String,Vector3,F)V
    getArgs(ctx, &self, &name, &pos, &f);                        // 4 slots
    h = instanceToHandle(self); if (!h) return;
    R = h->target; if (!R) return;
    if (R+0x7c != 1) R->vf5(1.0f);
    if (getTyped(0xa0000001,0,0) < 0) return;
    T = R->vf3(1.0f); if (!T) return;
    D = T+0x18; if (!D) return;
    x = getF(pos,"x"); y = getF(pos,"y"); z = getF(pos,"z");     // y,z unused
    vec = Vector3From(&x, 0.0f, f);                              // 0x470410
    act = list_find(D, name);                                    // 0x4b1060, ECX=D
    if (act) {
        if (act->[0x28] == 0xd) counter_set(act, vec);           // 13 = counter, 0x4d5bf0
    } else {
        node = alloc(0x7c); if (!node) return;
        n = counter_create(node, name, vec, 0, 0);               // 0x46f7e0, ECX=node
        if (n) link n into list at D+0x44 / D+0x4c;
    }

### 0x4e0a10 ParticleSystem.getCounter(String)I
    getArgs(ctx, &self, &name);                                  // 2 slots
    h = instanceToHandle(self); if (!h) return 0;                // 0x4ecc70
    R = h->target; if (!R) return 0;
    if (R+0x7c != 1) R->vf5(1.0f);
    if (getTyped(0xa0000001,0,0) < 0) return 0;
    T = R->vf3(1.0f); if (!T) return 0;
    D = T+0x18; if (!D) return 0;
    act = list_find(D, name);                                    // 0x4b1060, ECX=D
    if (!act || act->[0x28] != 0xd) return 0;
    v = act->[0x70];                                             // int counter
    act->[0x70] = 0;                                             // consumed
    return v;

## UNRESOLVED
- 0x470410 argument roles. Raw lines:
  `MOVSS XMM0,[EBP-0x20]; MOVSS [ESP+4],XMM0; MOVSS XMM0,[EBP-0x24];
   MOVSS [ESP],XMM0; PUSH ECX(LEA &x); ECX=alloc(0x20); CALL 0x00470410`
  and in 0x4e0bf0 `MOVSS XMM0,[EBP-0x14]; MOVSS [ESP+4],XMM0; MOV [ESP],0;
   PUSH ECX(LEA &x); CALL 0x00470410`. The ctor reads only the vector's `.x`
  plus two floats; whether `(x, a, b)` is the literal layout is not traced.
  Reached only when `D != 0`, so it does not affect this no-op card.
- 0x4d5c20 / 0x4d5c50 / 0x4d5bf0 / 0x46fa70 / 0x46ffb0 / 0x46f7e0 bodies are
  not traced. Action type codes 0xb / 10 / 0xd and offsets `+0x28`, `+0x6c`,
  `+0x70`, list head `D+0x24` / link `D+0x44..0x4c` come from the call sites
  only.
- `FUN_0046f7e0`'s first argument: Ghidra renders `FUN_0046f7e0(local_1c,...)`
  but the asm at 0x4e0dad is `MOV ECX,EDI` where EDI is the 0x7c allocation;
  the asm is used above.
- `RenderInst__getBoneId` 0x4e77f0 is called with ECX = the ParticleSystem handle
  `h` (asm 0x4e0f1c `MOV ECX,EBX`), not a RenderRef handle; the requirement that
  the particle instance carry render typed data is engine-side and untraced.
- Whether `act+0x70` (the counter getCounter reads/clears) is written by
  0x4d5bf0 or by the particle engine later is untraced.

## Files
- edit: `src/script/natives.cpp` — four functions `ps_set_direct_source`,
  `ps_set_source`, `ps_set_counter`, `ps_get_counter` next to the `anim_*`
  functions (same place as card 091's `ps_*`), plus the four ParticleSystem
  registry rows (currently `nullptr, true` → function name, `false`):
  `setDirectSource (Ljava.lang.String;Ljava.lang.Vector3;FFLjava.lang.Vector3;FFFLjava.lang.String;)V`,
  `setSource (same)`, `setCounter (Ljava.lang.String;Ljava.lang.Vector3;F)V`,
  `getCounter (Ljava.lang.String;)I`.
- read only: `src/script/natives.h` (Entry), `tasks/091-particlesystem-no-service.md` (style)

Each body follows card 091:
```cpp
bool ps_get_counter(Vm &, const Args &a, Value *out, std::string *err)
{
    if (!want("ParticleSystem.getCounter", a, 2, err)) return false;
    *out = Value::make_int(0);
    return true;
}
```
with `want(..., N, ...)` where N = 1 + Java parameter count: setDirectSource 10,
setSource 10, setCounter 4, getCounter 2. Comment block per function:
`// 0xADDR ParticleSystem.name(desc): <guard chain as above>` and
`// SERVICE MISSING: D = typed+0x18 does not exist without the particle
// service (0x4e8b50, card 091); every exe write is behind D != 0.` plus the
action codes/offsets in one `shortcut:` line. `getArgs` copies slots without
type checks, so `want()` (arity) is the src equivalent; do not add state fields
and do not invent a particle object. Decompiler slots on type mismatch are the
callers'/VM's business here.

## Done when
- the four rows above have a function and the last field `false`; the other five
  ParticleSystem rows stay as they are (card 091)
- each ported function has a `// 0xADDR ParticleSystem.name` comment and a
  `SERVICE MISSING:` note with the offsets/constants above; any deviation is
  `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
