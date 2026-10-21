---
id: 055
status: draft
branch:
depends: []
---
# GameRef.getInfo / setActiveCollision, GameLogic.kismajomCheck

## Goal
Replace three registry stubs in `src/script/natives.cpp` with traced implementations:
`GameRef.getInfo` (both overloads, one function), `GameRef.setActiveCollision`, and
`GameLogic.kismajomCheck`. `getInfo` and `setActiveCollision` only need the existing
handle/resource layer; the game-type native dispatch they call does not exist in src,
so both must return the exe's failure value (`getInfo` → 0) until that layer is added.
`kismajomCheck` is implemented in full against the shipped cheat table.

## Exe evidence (cleaned pseudocode, renamed fields)
Helpers: `getArgs` = FUN_004b1af0 (0x4b1af0, notes/06 §conventions); `unwrap` =
FUN_004ecc70 (script instance → native handle `H`, `H+0x8` = id, `H+0xc` = RI
per notes/13 §1); `arrayElem` = FUN_004ecac0; `intField` = FUN_004ece80.
In src this is `value_handle(a[0])` → `Handle{id, target}` (`target` = RI).

### 0x4daa80 java.util.resource.GameRef.getInfo(II)I / getInfo(ILjava/lang/String;)I
    getArgs(ctx, &self, &arg1, &arg2);
    H = unwrap(self, GameRef-class);
    if (H == 0) { log("vm_gameref_getinfo: scripted resource ... not found in file ..."); return 0; }
    if (H->id (+0x8) == 0) return 0;
    RI = H->target (+0xc);
    if (RI == 0) return 0;
    if (RI->+0x7c != 1) RI->vf5(+0x14)(1.0);            // make resident
    if (RI->getTyped(0x80000000,0,0) (FUN_0051f4f0) < 0) return 0;  // 0 = typed data present
    T = RI->vf3(+0xc)(1.0);                             // typed data
    if (T == 0 || T->payload (+0x4c) == 0) return 0;
    e = FUN_00468b00(0x80000000, 1.0, 0, 0);            // type registry entry
    M = (e == 0) ? 0 : e->+0xc;                         // manager/type block
    if (M == 0) return 0;
    return M->vtbl[+0x3c](T->payload, arg1, arg2);      // per-type getInfo handler
Raw dispatch line (callee is indirect; service absent in src):
    uVar4 = (**(code **)(**(int **)(iVar3 + 0xc) + 0x3c))(*(undefined4 *)(iVar2 + 0x4c),local_10,local_c);
Every failure branch returns 0. **UNRESOLVED:** RI+0x7c residency flag and vtable
+0x14 (vf5), FUN_0051f4f0 contract (only observed return 0 / 0x80000001), and the
`+0x3c` per-type handler. src has no typed-data/native-type-handler layer (see the
shortcut at `src/game/runtime.cpp:327`), so port only the guard chain and return 0.
INVENTED: stopping at the missing layer and returning 0 for a live game object; the
exe would dispatch to the type's handler.

### 0x4db7f0 java.util.resource.GameRef.setActiveCollision()V
    getArgs(ctx, &self);
    H = unwrap(self, GameRef-class);
    if (H == 0) { log("vm_gameref_setactivecoll: scripted resource ... not found in file ..."); return; }
    FUN_00501da0(H);
      RI = H->target (+0xc);
      if (RI == 0 || (RI->flags (+0x84) & 0x10000000) != 0) return;
      RI->flags (+0x84) |= 0x10000000;
      n = alloc(0x34);                                  // FUN_00567a20(0x34)
      n = (n == 0) ? 0 : FUN_0047b090(H);
      n->+4 = &DAT_00605250; n->+8 = DAT_00605258;      // push global pending list
      DAT_00605258->+4 = n; DAT_00605258 = n;
**UNRESOLVED:** FUN_0047b090 and the 0x34-byte request node / DAT_00605258 list
(pending active-collision requests; no collision service in src). **INVENTED:** the
port sets only the RI flag bit (the observable state src models, same word as
`Resource::flags`, RI+0x84) and skips the request node; void return on every path.

### 0x4debd0 java.game.GameLogic.kismajomCheck([Ljava/lang/String;)I
    n = intField(arg0, "length");                  // missing/non-int -> 0
    if (n == 0) return -1;
    do {
      n = n - 1;
      elem = arrayElem(arg0, n);
      s = unwrap(elem, <String class>);             // char* bytes
      // compare s[last..0] against the engine table at PTR_DAT_005dadb8
      if (<all bytes matched>) { *<table byte> = 0; return n; }
    } while (true);                                 // n == 0 -> return -1
Raw inner loop (constants/layout unresolved):
    pcVar9 = PTR_DAT_005dadb8;
    do {
      iVar3 = iVar4;
      if (iVar3 == 0) break;
      pcVar7 = pcVar1 + -1; pcVar1 = pcVar1 + -1;
      pcVar10 = pcVar9 + -1;
      pcVar9 = pcVar9 + 0xf;
      if ((char *)((int)&DAT_005fe3bc + 3) < pcVar10) { pcVar9 = pcVar10; }
      iVar4 = iVar3 + -1;
    } while (*pcVar7 + -1 == (int)*pcVar9);
    if (iVar3 + -1 < 0) {
      puVar8 = PTR_DAT_005dadb8 + -1;
      if (puVar8 < &DAT_005fe3c0) { puVar8 = PTR_DAT_005dadb8 + 0xf; }
      *puVar8 = 0;
      return iVar5;
    }
What is established: each input byte is compared **minus 1** with the table byte
(`*pcVar7 + -1 == *pcVar9`), highest index first, and the matching index is returned.
Corpus confirms the shift and the five commands: `bin/the game/sl/Scripts/game/GameLogic.java`
lines 269-274 store `"mfunfspd", "cfhgpsnpofz", "sfeefwjm", "uftunfme", "gjmmnzhbsbhf"`
commented `letmeroc, begformoney, reddevil, testme, fillmygarage`; each char −1 gives
that comment. `Garage.java:1926-2009` switches on the returned code 0..4.
**UNRESOLVED:** contents/layout of the engine table `PTR_DAT_005dadb8` and its end
`DAT_005fe3bc/0x5fe3c0` (the `+0xf` stride/clamp is not readable as an index by the
decompiler; no index appears in the table pointer), and the purpose of the `*puVar8 = 0`
store. Port uses the five commands above as the table, one entry per input index
(script order), marked `INVENTED: engine table bytes not read; content from shipped
GameLogic.java comments`. A full match = decoded candidate equals that entry.

## Files
- edit: `src/script/natives.cpp`
  - add `bool gr_get_info(Vm &, const Args &a, Value *out, std::string *err)` next to
    the other GameRef natives (~line 1182): `need_rt`, `want(who,a,3,err)`,
    `value_handle(a[0])`; missing handle → `fprintf(stderr, "vm_gameref_getinfo: scripted resource not found\n")`,
    then `Value::make_int(0)` and `return true`; handle id 0 or no target → 0; otherwise
    `// shortcut: typed data / per-type getInfo dispatch (T+0x4c, M vtable+0x3c) not modelled.`
    and return 0.
  - add `bool gr_set_active_collision(...)`: `need_rt`, `want 1`, `value_handle`; missing
    handle → stderr `"vm_gameref_setactivecoll: scripted resource not found"`; else
    `r->flags |= 0x10000000u` when `r->target` and the bit is clear; `*out = Value::make_null()`.
    Comment `// shortcut: pending collision request node (0x34, FUN_0047b090) not modelled.`
  - add `bool gl_kismajom_check(Vm &, const Args &a, Value *out, std::string *err)`:
    static call, one argument: `want(who,a,1,err)`; if `a[0].kind != Value::Array
    || !a[0].arr` → return -1 (exe's "length" read yields 0). Iterate `i` from
    `elems.size()` down; for `Value::Str` elements compare `decoded[k] = (char)(s[k]-1)`
    with the command string; return `(int)i` on match; non-Str elements are no match.
    Return -1 after the loop. `// 0x4debd0` comment plus `INVENTED:` for the table and
    for not emulating the exe's null dereference / byte-store.
  - registry rows (grep `kismajomCheck`, `"getInfo"`, `setActiveCollision`): replace
    the `nullptr, true` stub entries with `gr_get_info, false` (both getInfo rows),
    `gr_set_active_collision, false`, `gl_kismajom_check, false`.
- read only: `src/game/runtime.h` (`Handle{id,target}`, `Resource::flags`)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`;
  every gap above is present as a `shortcut:`/`INVENTED:`/`UNRESOLVED` note, not silently guessed
- `getInfo` returns 0 and `setActiveCollision` returns void on every missing-service path;
  `kismajomCheck` returns 0..4 for the shipped GameLogic.java array and -1 for an empty array
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
