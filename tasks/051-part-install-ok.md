---
id: 051
status: draft
branch:
depends: []
---
# Part: install_OK (install-slot search) native

## Goal
Replace the `java.game.parts.Part.install_OK` engine stub (registry row
`src/script/natives.cpp:4383`) with the exe's entry gates and its failure return.
The native searches a target part's typed type-slot list for an install slot and
returns `int[2]`. That search needs services this rebuild does not have (typed
type manager `M`, its `+0x116c` type-slot list, per-slot physics transforms,
`GetInfo` on a `GameRef`); with any of them missing the exe itself returns
**Java null** (`[I` null, `return 0` at 0x4cd3a0). So the port returns null on
every path, exactly like the exe's failure path, and must say so in a
`shortcut:` comment. On success the exe also writes the found part into the
out-argument `GameRef` (arg index 3); the port leaves it untouched.

Consequence to record in the code: `InventoryItem_Part.installToCar`
(`bin/the game/sl/Scripts/game/InventoryItem_Part.java:115`) treats a null result
as `error = ""` (success) and does **not** send the `install` command. Installing
a part from inventory therefore does not happen until the slot search is ported.

## Exe evidence (cleaned pseudocode, renamed fields)
Native prologue: `Native__getArgs 0x4b1af0` writes the receiver, then the
declared args in order (`instance+8` each) to consecutive stack slots. With the
declared signature `(GameRef, I, GameRef, I, Vector3)[I` the 0x4cd3a0 locals are:

| local | meaning |
|---|---|
| `local_1c` | receiver (self script object) |
| `local_18` | arg0 `dest` GameRef → its 0x10-byte ref node (`node+8` id, `node+0xc` RI) |
| `local_2c` | arg1 `slot` (int) — **overwritten** by the search result before use |
| `local_20` | arg2 `part` GameRef (out-argument; only null-tested in this function) |
| `local_28` | arg3 `slot2` (int) — in/out to the search |
| `local_14` | arg4 `pos` Vector3 or null |

`Native__instanceToHandle 0x4ecc70(self, DAT_007a2620)` + `Resource__getTyped
0x51f4f0` are the same chain as `part_of()` (card 050, src/script/natives.cpp:2338).
`FUN_0040cac0(node, resource)` is the intrusive ref-node bind/unbind already
implemented as `Runtime::bind` (src/game/runtime.h:105); its `this` (ECX) is
elided by the decompiler in this function.

### 0x4cd3a0 Part.install_OK
```
getArgs(self, dest, slot, part, slot2, pos)
if (dest == 0) return null;                      // arg0 ref node null
handle = instanceToHandle(self);                 // 0x4b1af0/0x4ecc70
RI = handle ? handle->0xc : 0;
if (RI) {
    if (RI->0x7c != 1) RI->vf5(1.0f);            // make resident
    if (Resource__getTyped(RI, 0xa0000000, 0, 0) >= 0) {
        T = RI->vf3(1.0f);                       // typed part object (= our PartData)
        if (T) {
            uVar4 = T->0x4c;                     // P, part data (our PartData*)
            if (FUN_00468b00(0x20000000, 1.0f, 0, 0) != 0) {   // secondary typed obj (see below)
                FUN_0040cac0(&targetNode, *(dest + 0xc));      // targetNode = dest resource
                targetNode = {0,0,0,0};
                posPtr = pos ? {getF(pos,"x"), getF(pos,"y"), getF(pos,"z")} : null; // 0x4ec980
                slot2_out = slot2;
                result = FUN_004c96c0(uVar4 /*P*/, &targetNode, &slot2_out, posPtr);
                // on success the found part is linked into the `part` out-argument;
                // the only such-looking line is `if (part != 0) FUN_0040cac0(local_3c)`
                if (result != 0) {
                    arr = new Instance(class DAT_0059f0a0, len 2);   // 0x567a20/0x55add0/0x54cab0
                    arr[0] = result;                                 // 0x4ed300
                    arr[1] = slot2_out;
                    return arr;
                }
            }
        }
    }
}
return null;                                     // every failure path in the exe
```

### 0x4c96c0 Part__installSearch(P, targetNode, &slot2, pos)
```
bind node to P+0x6c's resource (0x40cac0; ECX elided)
local slot = slot2; best = 100000000.0f; result = 0;
if (targetNode.id == 0 || FUN_004c9af0(P, targetNode, &slot, 0) == 0) {
    kind = FUN_0047c640(&node, 7, 0);            // GetInfo type code
    if (kind == 9 || kind == 5) {
        r = FUN_004c9af0(P, &node, &slot, pos);
        if (r) { if (!pos) { slot2 = slot; result = r; }
                 else if (typed secondary resident) {
                     if (FUN_004c6e50(P->0x4c, slot, &world) &&
                         dist2(world,pos) < 100000000.0f) { best=dist2; slot2=slot; result=r; } } }
        if (kind == 5) {                         // iterate the slot list: GetInfo 0x31
            while (FUN_0047c640(&node, 0x31, &iter)) {
                slot = 0; r = FUN_004c9af0(P, &iter, &slot, pos);
                if (r) { ... same position test, keep nearest ... }
            }
        }
    }
} else { slot2 = slot; result = r; }             // direct hit, no search
return result;                                   // 0 = no match -> native returns null
```
Note `P->0x78` (our `PartData::slots`) and the type-slot list `M+0x116c`
(node `+0x60` slot id) are walked in `FUN_004c9af0`.

### 0x4c9af0 slot-match helper (called with ECX = type manager M)
```
if (FUN_0047c640(targetNode, 0x27, 0) < 1) return 0;
if (targetNode.id == P.id) return 0;
resolve targetNode's typed part Tt; require 0xa0000000 and its secondary 0x468b00
if (*slot == 0) {
    for each type-slot node of M+0x116c, matching against P+0x78 runtime slots:
        slot+8 == 0 (empty) or flags&1, and !(flags&0x10), and
        FUN_004c89f0(Tt+0x4c, M+4, typeSlot+0x60, pos) != 0
        -> if pos: keep nearest by FUN_004c6e50 world pos
        -> result = typeSlot+0x60 (slot id); *slotOut = FUN_004c89f0 return
} else {
    node = FUN_004c9fe0(P->0x4c, *slot);
    walk M+0x116c and test FUN_004c9d90(M+4, typeSlot+0x60, node, *slot)
}
```
`FUN_004c89f0`, `FUN_004c9d90`, `FUN_004c9fe0` are untraced (UNRESOLVED).

### 0x47c640 GetInfo wrapper / 0x468b00 secondary typed object
`FUN_0047c640(refNode, infoId, out)` makes the node's RI resident, requires
`Resource__getTyped(RI, 0xa0000000, 0, 0)`, then calls
`FUN_0047e450(typed, infoId, out)` ("GetInfo"); info ids used here: 7, 0x27, 0x31,
the return codes 9 and 5 select the car/part branches. `FUN_00468b00(typeId,
1.0f, 0, 0)` (ECX = a ref node) does resident → `getTyped(typeId,0,0)` →
`vf3(1.0f)`; the caller uses it as a presence test for the secondary
`0x20000000` typed object (card 050 reports it at `T+0x44`).

### 0x40cac0 ref-node bind (already in src)
Intrusively links/unlinks a 16-byte node `{prev,next,id,RI}` (`RI+0x78` list).
Implemented as `Runtime::bind` — do not re-implement.

## Changes
1. `src/script/natives.cpp`, next to `pt_get_car`/`pt_set_render_type`:
   ```cpp
   // 0x4cd3a0 java.game.parts.Part.install_OK
   bool pt_install_ok(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Part.install_OK", a, 6, err)) return false;
       // shortcut: the 0x4c96c0 slot search needs the typed type manager
       // (GetInfo 0x47c640, type-slot list M+0x116c) and the per-slot physics
       // transform 0x4c6e50, none of which exist here; the exe returns null
       // for every path that cannot complete the search. The out GameRef
       // (a[3]) stays untouched, as on the exe's failure path.
       *out = Value::make_null();
       return true;
   }
   ```
2. `src/script/natives.cpp:4383`: replace the row with
   `{"java.game.parts.Part", "install_OK", "(Ljava.util.resource.GameRef;ILjava.util.resource.GameRef;ILjava.lang.Vector3;)[I", pt_install_ok, false},`

## UNRESOLVED (raw lines / open points)
- `FUN_004c89f0(Tt+0x4c, M+4, typeSlot+0x60, pos)`, `FUN_004c9d90(...)`,
  `FUN_004c9fe0(P->0x4c, *slot)`: candidate-match semantics untraced.
- `FUN_0047e450` GetInfo: meaning of info ids 7/0x27/0x31 and of the return
  codes 9 and 5 untraced.
- ECX (this) of `FUN_00468b00(0x20000000,…)` and of the `FUN_0040cac0` calls in
  0x4cd3a0/0x4c96c0 is elided by the decompiler; the `T+0x44` secondary-node
  assignment comes from card 050, not from an assembly read here.
- Where the out-argument `part` GameRef is written on success (only candidate:
  `if (local_20 != 0) { FUN_0040cac0(local_3c); }`).
- `DAT_0059f088/8c/90` (Vector3 field names; notes/13 §12.3 says x/y/z) and
  `DAT_0059f0a0` (class of the returned 2-element array) were not byte-read.
- The embedded ref node at `P+0x60`/`P+0x6c` and the meaning of the two returned
  ints (self slot id `typeSlot+0x60` vs `FUN_004c89f0` return) are inferred from
  the pseudocode only.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
