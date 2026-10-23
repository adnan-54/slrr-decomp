---
id: 070
status: todo
branch:
depends: []
---
# render: java.util.resource.Animation player natives

## Goal

Turn the nine `java.util.resource.Animation` natives (stubs at
`src/script/natives.cpp:5470-5478`) into functions that decode their arguments and
take the exe's no-service behaviour. They are instance methods (`<init>` included:
`a[0]` is the new Animation object), so the registered signature is shifted by one
in our VM.

The natives only drive a render-side animation *player*: `FUN_0051b8d0` allocates a
0xc0-byte player under a RenderRef resource instance, and every operation appends an
8-byte `{opcode, value}` command to that player's queue (`+0xb4` buffer, `+0xb8`
count, `+0xbc` capacity). `src/` has **no render instance, no RenderRef handle
creation and no animation player**, so no player can ever exist and the queue is
unreachable. Do not invent a player or a render host. Implement the argument
decoding, keep the exe's defaults/return values for the service-less case, and mark
the missing service.

## Exe evidence (cleaned pseudocode, renamed fields)

Helpers used below:
- `getArgs` = `FUN_004b1af0` (0x4b1af0): first out = receiver, then one out per
  signature char in Java order (`F`/`I` copy the 32-bit slot, `L`/`[` the object
  pointer). Cards 061/063/064.
- `getIntField(inst, DAT_007a2620)` = `FUN_004ecc70` (0x4ecc70): reads the int
  native-handle field named by the shared descriptor `DAT_007a2620`; returns 0 when
  the field/instance is absent (logs `vm_get_int_field ...`). In src this role is
  `Runtime::handle(Object*)` / `value_handle(Value)` (`Runtime::Handle::id`).
- `getInstanceField(inst, name, cls)` = `FUN_004ecae0` (0x4ecae0): reads a script
  field by name/class.
- `Resource__getTyped` = `FUN_0051f4f0` (0x51f4f0): hidden ECX = RI;
  `(mask, float, float)`; `< 0` = no typed data. Mask `0x80000000` here.
- Field name data: `0x5a447c` = `"obj"`, `0x5a4654` = `"obj"`, `0x5a4450` and
  `0x5a4630` = `"java.util.resource.RenderRef"` (read byte-wise this session).
- `0x3f800000` = 1.0f.

Common shape of the six operations (`setSpeed`, `setFade`, `seek`, `play`,
`loopPlay`, `pause`):

```
getArgs(ctx, &self [, &value]);
player = getIntField(self, DAT_007a2620);                  // Animation native handle
obj    = getInstanceField(self, "obj", "java.util.resource.RenderRef");
hObj   = getIntField(obj, DAT_007a2620);                   // RenderRef native handle
if (*(int*)(hObj + 8) != 0)                                // resource id != 0
    <queue-op>(player, value);                             // ECX = player
```

The exe does **not** null-check `hObj` (deref at `[hObj+8]`), and does not check
`player` either. When `obj` has a bound resource but `player` is 0 the queue helper
derefs address 0 — an exe fault. `src/` has no state that can satisfy the guard, so
the port must never reach the helper.

### 0x4d8a40 Animation.<init>(RenderRef, ResourceRef)V

Raw getArgs line: `FUN_004b1af0(param_1,&local_8,&local_c,&local_10,&local_14,&local_18);`
(five out slots: receiver + four argument slots; see UNRESOLVED).

```
self = out1; renderRef = out2; resourceRef = out3; a3 = out4; a4 = out5;
player = 0;
if (renderRef != 0) {
    RI = *(void**)(renderRef + 0xc);                       // RenderRef native handle
    if (RI != 0) {
        if (*(int*)(RI + 0x7c) != 1) RI->vf[+0x14](1.0f);
        if (Resource__getTyped(RI, 0x80000000, 0, 0) >= 0) {
            inst = RI->vf[+0xc](1.0f);
            if (inst != 0 && FUN_0051be40(inst)) {          // ECX = inst
                player = FUN_0051b8d0(inst, resourceRef);   // ECX = inst
                if (player != 0) {
                    FUN_0051c4e0(player, a3, 0.0f);         // player+0x3c/+0x40 = a3
                    FUN_0051c560(player, a4, 0.0f);         // player+0x48/+0x4c = a4
                    FUN_0051c540(player, 0, 0);             // player+0x34 = 0
                }
            }
        }
    }
}
setIntField(self, DAT_007a2620, player);                    // Animation native handle
obj  = getInstanceField(self, "obj", "java.util.resource.RenderRef");
hObj = getIntField(obj, DAT_007a2620);
FUN_0040cac0(hObj /*ECX*/, *(void**)(renderRef + 0xc));     // no null check on renderRef
```

`FUN_0051b8d0(inst, resourceRef)` (0x51b8d0, ECX = inst) allocates 0xc0 bytes,
calls `FUN_0051af10` (player ctor), binds the node at `inst+0xe4` to
`*(resourceRef+0xc)` through `FUN_0040cac0`, links the player as
`{+4 prev, +4 self, +8 next}` into the list head `inst+0xec`, sets
`*(u32*)(inst+0x16c) |= 0x4000`, and returns the player (0 on alloc failure).

`FUN_0051be40` (0x51be40, ECX = inst): returns 0 unless `*(int*)(inst+0x11c) == 3`;
then requires `*(int**)(inst+0x114)` and typed data `Resource__getTyped(0x80000000)`,
`typed = ri->vf[+0xc](1.0f)`, `*(int*)(typed+0x38) != 0`; on success it lazily
creates/initialises a stream object at `inst+0xc` and returns 1. Identity of the
`+0x38` payload and the stream ctor are UNRESOLVED (not needed without the service).

`FUN_0051c4e0` (0x51c4e0, `(player, value, 0.0f)`): `player+0x3c = value`,
`player+0x40 = value`, `player+0x38 = 0`. `FUN_0051c560` (0x51c560): the same shape
at `+0x48/+0x4c/+0x44`. `FUN_0051c540` (0x51c540): `player+0x34 = 0`. Names of the
three parameters are UNRESOLVED (likely initial playback settings, cf. card 047's
"template default" naming); the offsets are observed.

### 0x4d8980 Animation.finalize()V

```
getArgs(ctx, &self);
player = getIntField(self, DAT_007a2620);
obj    = getInstanceField(self, "obj", "java.util.resource.RenderRef");
hObj   = getIntField(obj, DAT_007a2620);
if (*(int*)(hObj + 8) != 0 && player != 0)                 // no null check on hObj
    player->vf[0](1);                                      // release/destruct
```

### 0x4d8d70 setSpeed(F)V
`value` = arg. Guard as above, then `FUN_004cf350(player, value)` (0x4cf350): queue
opcode **5**, value. Assembly tail confirms ECX = player (`mov esi,eax` after the
first `getIntField`, then `mov ecx,esi`).

### 0x4d8d00 setFade(F)V
Same guard, then `FUN_004cf2e0(player, value)` (0x4cf2e0): queue opcode **6**, value.

### 0x4d8c90 seek(F)V
Same guard, then `FUN_004cf270(player, value)` (0x4cf270): queue opcode **4**, value.

### 0x4d8c30 play()V
Same guard, then `FUN_004d8840(player)` (0x4d8840): queue opcode **0**, value 0.

### 0x4d8b70 loopPlay()V
Same guard, then `FUN_004d8780(player, 0)` (0x4d8780): queue opcode **1**, value 0.

### 0x4d8bd0 pause()V
Same guard, then `FUN_004d87e0(player)` (0x4d87e0): queue opcode **3**, value 0.

Queue push (identical body in 0x4cf270/0x4cf2e0/0x4cf350/0x4d8780/0x4d87e0/0x4d8840,
ECX = player):

```
if (*(int*)(player+0xb8) == *(int*)(player+0xbc)) {        // count == capacity
    *(int*)(player+0xbc) += 0x20;
    *(void**)(player+0xb4) = realloc(*(void**)(player+0xb4),
                                     *(int*)(player+0xbc) * 8);
}
i = *(int*)(player+0xb8); *(int*)(player+0xb8) = i + 1;
*(int*)(buf + i*8)     = opcode;
*(int*)(buf + i*8 + 4) = value;
```

### 0x4d89e0 getPos()F

```
getArgs(ctx, &self);                                       // assembly 0x4d89e0
player = getIntField(self, DAT_007a2620);
obj    = getInstanceField(self, "obj", "java.util.resource.RenderRef");
hObj   = getIntField(obj, DAT_007a2620);
if (*(int*)(hObj + 8) != 0)
    return FUN_0051dde0(player);                           // ECX = player
return 0.0f;                          // raw: 0x4d8a26 je +0x0c -> 0x4d8a34 fldz
```

`FUN_0051dde0` (0x51dde0, ECX = player):

```
pos = 0.0f;
ri = *(int**)(player+0x30);
if (ri) {
    if (ri[0x1f] != 1) ri->vf[+0x14](1.0f);                // ri+0x7c != 1
    if (Resource__getTyped(ri, 0x80000000, 0, 10.0f /*0x41200000*/) < 0) T = 0;
    else T = ri->vf[+0xc](1.0f);
    if (T != 0 && *(int*)(T + 0xc) != 0)
        pos = *(float*)(player+0x34) / (float)FUN_00514680();  // duration/clip len, UNRESOLVED
}
return pos;
```

### Service-missing values (src has no render/animation player)
No RenderRef script object with a bound handle can exist in `src/` (the whole
`java.util.resource.RenderRef` block is still stubs and no render resource is
created), so `getIntField(obj, ...)` is 0 / the guard fails:
- `<init>`, `finalize`, `setSpeed`, `setFade`, `seek`, `play`, `loopPlay`,
  `pause` → void, do nothing;
- `getPos` → **0.0f** (the exe's own fallback, `fldz` at 0x4d8a34; also the value
  `FUN_0051dde0` returns with no player data).

## Files
- edit: `src/script/natives.cpp` (nine functions before `}  // namespace` at line
  5185; nine registry rows 5470-5478)

## Changes
1. Add before line 5185 (`}  // namespace`):
   ```cpp
   // ---- java.util.resource.Animation (notes/12 §9) -------------------------
   // All nine natives drive a render-side animation player: FUN_0051b8d0 creates
   // the 0xc0-byte player under a RenderRef resource instance and the operations
   // append {opcode, value} records to it (+0xb4 buf, +0xb8 count, +0xbc cap).
   // SERVICE MISSING: src/ has no render instance, no RenderRef handle creation
   // and no player, so no guard can pass and nothing is queued.

   // 0x4d8a40 Animation.<init>(RenderRef,ResourceRef)V
   // exe: builds the player when the RenderRef RI has typed data (0x4d8a40:
   // Resource__getTyped(RI,0x80000000) -> RI->vf[+0xc] -> FUN_0051be40 ->
   // FUN_0051b8d0), then stores it in the Animation's native-handle field and
   // binds the "obj" RenderRef handle to the argument's RI (FUN_0040cac0).
   // shortcut: no player can be created (native-handle field stays 0). The
   // "obj" bind is unconditional in the exe and IS ported. The exe derefs a null
   // RenderRef argument at 0x4d8b5b and a null obj handle in 0x40cac0; the port
   // skips those (INVENTED safety).
   bool anim_ctor(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("Animation.<init>", err)) return false;
       Object *o;
       if (!receiver_object("Animation.<init>", a, &o, err) ||
           !want("Animation.<init>", a, 3, err))
           return false;
       Value v;
       std::string e;
       Runtime::Handle *h = vm.get_instance_field(o, "obj", &v, &e) ? value_handle(v) : nullptr;
       Runtime::Handle *src = value_handle(a[1]);       // renderRef native handle
       if (h && src) game_rt()->bind(h, src->target);   // 0x40cac0(hObj, renderRef+0xc)
       *out = Value::make_null();
       return true;
   }

   // 0x4d8980 Animation.finalize()V
   // exe: if the "obj" RenderRef handle's resource id (+8) != 0 and the stored
   // player != 0, player->vf[0](1). No player exists here.
   bool anim_finalize(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Animation.finalize", a, 1, err)) return false;
       *out = Value::make_null();
       return true;
   }

   // 0x4d8d70 Animation.setSpeed(F)V: queue opcode 5 (FUN_004cf350) when the
   // "obj" RenderRef handle has a nonzero resource id.
   bool anim_set_speed(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Animation.setSpeed", a, 2, err)) return false;
       float v;
       if (!get_f("Animation.setSpeed", a[1], &v, err)) return false;
       (void)v;   // 0x4cf350 unreachable: no animation player in src
       *out = Value::make_null();
       return true;
   }

   // 0x4d8d00 Animation.setFade(F)V: queue opcode 6 (FUN_004cf2e0).
   bool anim_set_fade(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Animation.setFade", a, 2, err)) return false;
       float v;
       if (!get_f("Animation.setFade", a[1], &v, err)) return false;
       (void)v;
       *out = Value::make_null();
       return true;
   }

   // 0x4d89e0 Animation.getPos()F: 0.0f when the "obj" guard fails (0x4d8a26
   // je -> 0x4d8a34 fldz), otherwise player+0x34 / clip length via FUN_0051dde0.
   // SERVICE MISSING: no player, so the exe's no-data value 0.0f.
   bool anim_get_pos(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Animation.getPos", a, 1, err)) return false;
       *out = Value::make_float(0.0f);
       return true;
   }

   // 0x4d8c90 Animation.seek(F)V: queue opcode 4 (FUN_004cf270).
   bool anim_seek(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Animation.seek", a, 2, err)) return false;
       float v;
       if (!get_f("Animation.seek", a[1], &v, err)) return false;
       (void)v;
       *out = Value::make_null();
       return true;
   }

   // 0x4d8c30 Animation.play()V: queue opcode 0 (FUN_004d8840).
   bool anim_play(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Animation.play", a, 1, err)) return false;
       *out = Value::make_null();
       return true;
   }

   // 0x4d8b70 Animation.loopPlay()V: queue opcode 1, value 0 (FUN_004d8780).
   bool anim_loop_play(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Animation.loopPlay", a, 1, err)) return false;
       *out = Value::make_null();
       return true;
   }

   // 0x4d8bd0 Animation.pause()V: queue opcode 3, value 0 (FUN_004d87e0).
   bool anim_pause(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Animation.pause", a, 1, err)) return false;
       *out = Value::make_null();
       return true;
   }
   ```
2. Registry rows 5470-5478: point each `nullptr, true` at the matching function and
   set the last field to `false`:
   `<init>`→`anim_ctor`, `finalize`→`anim_finalize`, `setSpeed`→`anim_set_speed`,
   `setFade`→`anim_set_fade`, `getPos`→`anim_get_pos`, `seek`→`anim_seek`,
   `play`→`anim_play`, `loopPlay`→`anim_loop_play`, `pause`→`anim_pause`.
   Do not touch the neighbouring Text/ParticleSystem rows.

## Done when
- each function has its `// 0xADDR Animation.<name>` comment; the missing service
  and the observed-but-unreachable queue opcodes are marked `SERVICE MISSING:` /
  `shortcut:`; the null-object difference is `INVENTED:`/`shortcut:`
- `getPos` returns float 0.0f; the other eight return null
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- **Constructor argument count.** `0x4d8a40` passes five out pointers to
  `getArgs` (receiver + four args) and reads the last two as floats into
  `FUN_0051c4e0/560`, but the registered signature is
  `(Ljava.util.resource.RenderRef;Ljava.util.resource.ResourceRef;)`
  (`analysis/natives.tsv:277`, string `0x5a5160`) — only two parameters. Either the
  loaded Animation class declares more parameters than the registration, or the
  exe reads uninitialised stack for `a3`/`a4`. The port decodes only the two
  declared arguments; it does not model a3/a4.
- **`FUN_0051be40` / `FUN_0051dde0` tail.** `typed+0x38` (stream/asset payload),
  `FUN_00514680` (clip length/denominator) and the lazy stream object at
  `inst+0xc` are not re-traced; they are unreachable without the player service.
- **Player parameter names.** `0x51c4e0` writes `+0x3c/+0x40/+0x38`,
  `0x51c560` writes `+0x48/+0x4c/+0x44`, `0x51c540` writes `+0x34`; which is
  speed/fade/loop is not established (only the opcodes 5/6 are named by the
  natives).
- **Ops after the bind (gate note, Claude).** Once `obj` is bound, the exe's guard
  `hObj+8 != 0` passes and the queue helpers / FUN_0051dde0 deref the 0 player (exe
  fault). The port keeps them no-ops / 0.0f: mark that `INVENTED:` in each op.
- **Null-argument fault.** `0x4d8b5b` derefs `renderRef+0xc` unconditionally and
  the other natives deref `hObj+8` without a null check; the port accepts null
  instead of faulting (no crash in the rebuild, marked in the code).
