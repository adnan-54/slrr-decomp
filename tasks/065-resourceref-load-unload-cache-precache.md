---
id: 065
status: merged
branch: card/065
depends: []
---
# resource: ResourceRef.load / unload / cache / precache

## Goal
Replace the four `java.util.resource.ResourceRef` stubs (registry rows 4698-4699 and
4710-4711 in `src/script/natives.cpp`) with natives that take the exe's missing-handle
branch, log the exe's error text, and reproduce the exe's result on every path `src/`
can reach. All four exe natives are `void` (they `RET` without setting `EAX`), so the
VM result is `null` on every path.

The real work of all four is engine-side: `load` sets a config-gated flag on the
resource instance and then makes the instance resident / fetches its typed data;
`unload` either queues the ref node on a global deferred-unload list or calls a
release vfunc; `cache`/`precache` run a typed-object walk whose float result is
immediately discarded (`FSTP ST0`). `src/` has none of those services (no streaming /
residency, no typed-object registry, no game ConfigData), so with a present handle the
port is a documented no-op. This is faithful: with no config data the exe's
`GetConfigData` itself returns 0, so the only state `src/` could carry (the
`0x80000` flag) is never written in that situation either.

## Exe evidence (cleaned pseudocode, renamed fields)

Helpers:
- `getArgs` = FUN_004b1af0 (0x4b1af0): out0 = receiver (script instance). A `()V`
  native has exactly that one out. In `src/` the receiver is `a[0]`.
- `instanceToHandle` = FUN_004ecc70 (0x4ecc70): reads the instance's native handle
  (hidden int field keyed by `DAT_007a2620`); 0 when absent. Notes/13 §1. The ref
  node is the 0x10-byte record allocated by 0x4e4080: `+0x0` prev, `+0x4` next,
  `+0x8` id, `+0xc` RI. In `src/`: `value_handle(a[0])` → `h->id` (+0x8),
  `h->target` (+0xc).
- RI+0x7c = `Runtime::Resource::type` (src comment at `rr_type`: "0x4e43e0: RI ?
  RI+0x7c : 0").
- RI+0x84 = `Runtime::Resource::flags` (src comment at `gr_get_flags`: "0x4daa40:
  RI+0x84").
- RI+0x9c = `Runtime::Resource::detail` (src comment at `gr_get_detail`: "0x4e3330:
  detail float at RI+0x9c").
- `GetConfigData` = 0x47c110: `(name)` → int from the game config; returns 0 when
  the config object/typed data/field is absent (and logs `GetConfigData: Config data
  not present` when the typed object exists but `+0x50` is null).
- `Resource__getTyped` = 0x51f4f0: hidden ECX = RI, cdecl `(mask, float, float)`.
  Returns 0, or 0x80000001 (= -2147483647 signed) — callers test `>= 0`, i.e. only
  the 0 result passes. The 0/negative choice is `(*(byte*)(RI + 0x94) & 1) != 0`;
  the mask's bit 0 is not tested, so 0x80000001 behaves like 0x80000000.
- `Resource__releaseChain` = 0x4ec800: hidden ECX = ref node `h`.
- `UnloadQueue__add` = 0x47e340: first arg = `h`, hidden ECX = global 0x5fde90
  (`0x4ec815 PUSH EAX; 0x4ec816 MOV ECX,0x5fde90; CALL 0x0047e340`, EAX = h).
- `Resource__cacheApply` = 0x4e9830: hidden ECX = `h`, cdecl `(int32, float)`.

Constants: `0x80000` = 524288 (flags bit); `0x3f800000` = 1.0f; `0x80000001` =
2147483649 (negative as int32); `0x3f800000` passed as the int first argument of
precache is the same bit pattern.

### 0x4e3de0 ResourceRef.load()V
```
self = args[0];                                     // 0x4b1af0
h = instanceToHandle(self);                         // 0x4ecc70
if (h == 0) {                                       // 0x4e3e08
    log("vm_resourceref_load: scripted resource " + hex(h) +
        " not found in file \"" + callinfo->file /*+0x74*/ +
        "\" at line " + callinfo->line /*+0x70*/);
    return;
}
if (GetConfigData("ground_precache") /*0x47c110*/ > 0 && *(void**)(h + 0xc) != 0)
    *(uint32_t*)(*(uint32_t*)(h + 0xc) + 0x84) |= 0x80000;   // RI flags |= 0x80000
RI = *(void**)(h + 0xc);
if (RI == 0) return;                                // 0x4e3e33
if (*(int*)(RI + 0x7c) != 1)
    RI->vtbl[+0x14](1.0f);                          // 0x4e3e4b: make resident
if ((int32_t)Resource__getTyped(/*ECX=*/RI, 0x80000001, 0.0f, 0.0f) >= 0)
    RI->vtbl[+0xc](1.0f);                           // 0x4e3e80
```

### 0x4e4500 ResourceRef.unload()V
```
self = args[0];                                     // 0x4b1af0
h = instanceToHandle(self);                         // 0x4ecc70
if (h == 0) {
    log("vm_resourceref_unload: scripted resource " + hex(h) +
        " not found in file \"" + callinfo->file + "\" at line " + callinfo->line);
    return;
}
Resource__releaseChain(h);                          // 0x4ec800, ECX = h
```

### 0x4ec800 Resource__releaseChain(h)   (hidden ECX = h)
```
if (*(uint32_t*)(h + 0x8) != 0 && (RI = *(void**)(h + 0xc)) != 0) {
    if (*(int*)(RI + 0x7c) == 1)
        UnloadQueue__add(/*ECX=*/l_005fde90, h);    // 0x47e340
    else
        RI->vtbl[+0x1c](1);                         // 0x4ec825
}
```
Raw 0x4ec815-0x4ec81b: `PUSH EAX; MOV ECX,0x5fde90; CALL 0x0047e340` (EAX = h).

### 0x47e340 UnloadQueue__add(q = l_005fde90, h)
```
node = Mem__alloc(0x34);                            // 0x567a20
node = node ? FUN_0047b090(h) : 0;
tail = *(void**)(q + 0x114);
*(void**)(tail + 0x4) = node;
*(void**)(node + 0x4)  = q + 0x10c;                 // list sentinel/head
*(void**)(node + 0x8)  = tail;
*(void**)(q + 0x114)   = node;
*(uint32_t*)(q + 0x220) += 1;
```

### 0x4e3100 ResourceRef.cache()V
```
self = args[0];                                     // 0x4b1af0
h = instanceToHandle(self);                         // 0x4ecc70
if (h == 0) {
    log("vm_resourceref_cache: scripted resource " + hex(h) +
        " not found in file \"" + callinfo->file + "\" at line " + callinfo->line);
    return;
}
(void)Resource__cacheApply(/*ECX=*/h, 0, 1.0f);     // 0x4e9830; FSTP ST0
```
Raw args at the call: `0x4e312e [ESP+4]=0x3f800000`, `0x4e3136 [ESP]=0`.

### 0x4e40f0 ResourceRef.precache()V
```
self = args[0];                                     // 0x4b1af0
h = instanceToHandle(self);                         // 0x4ecc70
if (h == 0) {
    log("vm_resourceref_precache: scripted resource " + hex(h) +
        " not found in file \"" + callinfo->file + "\" at line " + callinfo->line);
    return;
}
(void)Resource__cacheApply(/*ECX=*/h, 0x3f800000 /* = 1.0f as int */, 1.0f);  // 0x4e9830
```
Both args are `0x3f800000` (`0x4e411e`/`0x4e4126`).

### 0x4e9830 Resource__cacheApply(h, a1, a2)   (hidden ECX = h; float result discarded)
```
if (*(uint32_t*)(h + 0x8) == 0) return 0.0f;
RI = *(void**)(h + 0xc);
if (RI == 0) return 0.0f;
v = *(float*)(RI + 0x9c);                           // detail
if (*(int*)(RI + 0x7c) != 0xe) {
    TypeRegistry__get(0xa0000001, a2, 0, a1);       // 0x468b00
    return v;
}
T = TypeRegistry__get(0xa0000001, 1.0f, 0, 0);       // 0x468b00
if (T == 0) return v;
TypeRegistry__get(0xa0000000, a2, 0, a1);
v *= (*(void**)(T + 0x14) && *(void**)(T + 0x18))
         ? *(float*)(*(uint32_t*)(T + 0x18) + 0x9c) : 0.0f;
for (i = 0; i < *(int*)(T + 0x20); i++) {
    base = *(uint32_t*)(T + 0x24);
    node = *(void**)(base + 0xc + i*0x10);
    if (node) {
        if (*(int*)(node + 0x7c) != 1) node->vtbl[+0x14](a2);
        if ((int32_t)Resource__getTyped(/*ECX=*/node, 0xa0000000, 0, a1) >= 0)
            node->vtbl[+0xc](1.0f);
    }
    float f = (*(uint32_t*)(base + 0x8 + i*0x10) == 0 || node == 0)
                  ? 0.0f : *(float*)(node + 0x9c);
    v *= f;
}
return v;
```

## Files
- edit: `src/script/natives.cpp` only — add four functions after `rr_get_next_child`
  (ends line 1181), before the `// ---- java.util.resource.GameRef` block (line 1183);
  change registry rows 4698-4699 (`load`, `unload`) and 4710-4711 (`cache`,
  `precache`) from `nullptr, true` to the new functions with `false`.
- read only: `src/game/runtime.h` (`Runtime::Handle`, `Runtime::Resource`).

## Changes
1. Four natives, same shape (only the log prefix differs):
   ```cpp
   // 0x4e3de0 ResourceRef.load()V
   bool rr_load(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("ResourceRef.load", a, 1, err)) return false;
       *out = Value::make_null();
       if (!value_handle(a[0]))
           std::fprintf(stderr, "vm_resourceref_load: scripted resource not found\n");
       // shortcut: no ConfigData/streaming service in src. The exe ORs 0x80000 into
       // RI+0x84 only when GetConfigData("ground_precache") > 0 (0x4e3e0c), then
       // makes the instance resident (vtbl +0x14) and fetches typed data
       // (0x51f4f0 mask 0x80000001; on success vtbl +0xc). GetConfigData returns 0
       // when config data is absent (0x47c110), so the flag branch is not taken.
       return true;
   }

   // 0x4e4500 ResourceRef.unload()V
   bool rr_unload(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("ResourceRef.unload", a, 1, err)) return false;
       *out = Value::make_null();
       if (!value_handle(a[0]))
           std::fprintf(stderr, "vm_resourceref_unload: scripted resource not found\n");
       // shortcut: no release service in src. 0x4ec800: id != 0 && RI != 0 ->
       // type == 1 ? deferred-unload queue 0x47e340 (global 0x5fde90) : vtbl +0x1c.
       return true;
   }

   // 0x4e3100 ResourceRef.cache()V
   bool rr_cache(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("ResourceRef.cache", a, 1, err)) return false;
       *out = Value::make_null();
       if (!value_handle(a[0]))
           std::fprintf(stderr, "vm_resourceref_cache: scripted resource not found\n");
       // shortcut: 0x4e9830(h, 0, 1.0f) walks engine typed objects (0x468b00) and
       // its float result is discarded (FSTP ST0); no such service in src.
       return true;
   }

   // 0x4e40f0 ResourceRef.precache()V
   bool rr_precache(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("ResourceRef.precache", a, 1, err)) return false;
       *out = Value::make_null();
       if (!value_handle(a[0]))
           std::fprintf(stderr, "vm_resourceref_precache: scripted resource not found\n");
       // shortcut: 0x4e9830(h, 0x3f800000 /*1.0f as int*/, 1.0f), result discarded.
       return true;
   }
   ```
   The missing-handle check must come before anything else (exe order).
2. Registry:
   ```cpp
   {"java.util.resource.ResourceRef", "load", "()V", rr_load, false},
   {"java.util.resource.ResourceRef", "unload", "()V", rr_unload, false},
   {"java.util.resource.ResourceRef", "cache", "()V", rr_cache, false},
   {"java.util.resource.ResourceRef", "precache", "()V", rr_precache, false},
   ```

## Done when
- each function has its `// 0xADDR ResourceRef.<name>` comment; every unported side
  effect is covered by a `shortcut:` comment naming the exe call and why it cannot run
- missing handle: the four prefixes `vm_resourceref_load/unload/cache/precache:
  scripted resource not found` go to stderr (src convention, e.g. natives.cpp:1655)
- present handle: returns `null` without touching anything, for all four
- registry rows 4698-4699 and 4710-4711 point at the new functions
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- **Deferred unload queue**: 0x47e340 stores a 0x34-byte node built by
  `FUN_0047b090(h)` into global 0x5fde90: tail at `+0x114`, sentinel/head at
  `+0x10c`, count at `+0x220`; the node payload/layout and the consumer are not
  traced. Raw lines: `*(int *)(iVar2 + 8) = iVar1;` and
  `*(int *)(in_ECX + 0x220) = *(int *)(in_ECX + 0x220) + 1;`.
- **Typed-object walk 0x4e9830**: `TypeRegistry__get` = 0x468b00 whose hidden ECX
  is not shown; fields `T+0x14/+0x18/+0x20/+0x24`, type `0xe`, masks
  `0xa0000001`/`0xa0000000` (0xa0000000 = physics/vehicle data per notes/09 line 13)
  are unidentified. Both callers discard the returned float, so nothing depends on
  it here.
- **0x51f4f0 status flag**: returns 0 only when byte `RI+0x94` bit 0 is set, else
  0x80000001; `RI+0x94` is not mapped to a `src` field. It only gates the
  `vtbl +0xc` call.
- **Instance vtables**: `RI->vtbl[+0x14]` (notes/13 §1: make resident),
  `RI->vtbl[+0xc]` (returns typed data), `RI->vtbl[+0x1c]` (argument 1, purpose
  untraced) have no `src` counterpart.
- **No ConfigData service in `src/`**: `ground_precache` cannot be read, so the
  `RI+0x84 |= 0x80000` write is never reached. Exe return for the absent-service
  case is 0 (0x47c110 tail), which keeps it on the no-write path. All four natives
  are `void`; the exe has no error return, so the port always returns `null`.

## Gate (Claude)
Approved as the no-service port (all four natives are void; missing-handle logs kept).
