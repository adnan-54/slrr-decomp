---
id: 117
status: draft
branch:
depends: []
---
# VM: null receiver in a windowed field access (`ct.length`)

## Goal
`MouseCursor.cleanup` does `ResourceRef[] ct = this.getChildNodes(); for (i = ct.length - 1; ...)`
and in our vmboot `ct` is null, so `Thread::field_member` (src/script/vm.cpp) hits its
`explicit_recv` guard and kills the thread with `fail("field access on non-object")`.

The exe does **not** turn this into a thread failure. Its name evaluator
`Thread::evalName` 0x562280 (called from `Thread::run` case 0x1011) reports
`Thread::evalName: null.methodcall()` through the script-error reporter 0x559d20 — which
only prints and never touches the thread — and then **dereferences the null instance**
(`[null+0xc]` at 0x562340, and again for the field-ref lookup at 0x5624e4). So the exe
prints the error and then faults; it returns neither 0 nor null, and it does not set a
kill flag on the thread. (Whether an outer SEH frame catches the fault is unresolved,
see the end of this card.)

Change `Thread::field_member` for the explicit-null-receiver case only: report the exe's
text and continue with a defined value (0 for `length`, null otherwise) instead of
failing the thread. The value is a port substitution for the exe's fault and must carry
an `INVENTED:` comment. Do not touch `Thread::field_name` (0x401c path) in this card.

## Exe evidence (cleaned pseudocode / decoded assembly)
### 0x562d70 `Thread::run`, case 0x1011 (exact decompile)
```c
case 0x1011:
  iVar4 = FUN_00562280(in_ECX + 0x24);           // Thread::evalName(&thread->ip)
  if (iVar4 == 0) {
    FUN_00559d20(*(undefined4 *)(in_ECX + 0x10),"name cannot be parsed");
  }
  FUN_00562c40(iVar4);                           // push result (0 => null value)
  *(int *)(in_ECX + 0x24) = *(int *)(in_ECX + 0x24) + 8;
```
No thread-state write on the error path. (The `iVar4 == 0` return only happens for the
*by-name member miss* further down, not for a null receiver.)

### 0x562280 `Thread::evalName`, later-segment null check (exact decompile)
```c
  else {                                   // bVar4 == false: 2nd and later segment
    if (piVar8[2] == 0) {                  // previous segment's Value payload (instance/array)
      if (piVar8[3] == 0) {
        FUN_00559d20(*(undefined4 *)(in_ECX + 0x10),"Thread::evalName: null.methodcall()");
      }
      else {
        FUN_00559d20(*(undefined4 *)(in_ECX + 0x10),"Thread::evalName: null.methodcall() on <%s>",
                     piVar8[3]);           // value name at +0xc
      }
    }
    cVar1 = *(char *)**(undefined4 **)(*(int *)(piVar8[2] + 0xc) + 8);   // <-- deref null
    if ((cVar1 != 'L') && (cVar1 != '[')) {
      FUN_00559d20(*(undefined4 *)(in_ECX + 0x10),
                   "Thread::evalName: field access on primitive variable");
    }
    iVar10 = piVar8[2];
LAB_0056236b:
    iVar6 = *(int *)(iVar10 + 0xc);        // <-- deref null again
  }
```
Decoded assembly (same lines):
```
0x56230d  83 7b 08 00    cmp  dword [ebx+8],0      ; ebx = Value* of the previous segment
0x562311  75 2a          jnz  0x56233d
0x562313  8b 43 0c       mov  eax,[ebx+0xc]        ; optional name for "on <%s>"
0x56231b  68 90 5f 5b 00 push 0x5b5f90            ; "Thread::evalName: null.methodcall() on <%s>"
0x56232d  68 bc 5f 5b 00 push 0x5b5fbc            ; "Thread::evalName: null.methodcall()"
0x562335  e8 ..          call 0x559d20             ; 0x559d20(&vm, msg)
0x56233d  8b 43 08       mov  eax,[ebx+8]          ; null payload, reused
0x562340  8b 40 0c       mov  eax,[eax+0xc]        ; ACCESS VIOLATION (read at 0xc)
```
The corresponding field-ref tail (instance branch, `bVar5 == 0`) passes the same null
payload as the receiver:
```
0x5624e4  8b 4b 08       mov  ecx,[ebx+8]          ; receiver instance (0)
0x5624e7  e8 ..          call 0x54caa0             ; getFieldRef by index
```
`0x54caa0` is `mov ecx,[ecx+0x10]; jmp 0x548d70` (`this` = the instance; field container
at +0x10), so ECX=0 faults there too.

Local values: 0x5624f6 loads the `Variable*` itself from the locals array
(`piVar8 = locals[operand]`), so a local holding a null reference still has a non-null
`Value*` whose payload at +8 is 0 — the check above fires.

### 0x559d20 script-error reporter (exact decompile, non-fatal)
```c
FUN_00528030(local_1004,"!Script error\n\nThread: %s\nError: ", ...);
... per-frame "%s\n%s:%s (line:%d)" ...
FUN_005695c0(local_1004);
DAT_007a2838 = 0;
return;
```
No write to the thread object at all (thread state flags are +0x2c). The tail is
`0x5695c0 -> 0x544e80(10000, msg)`: with 10000 > 9999 it takes the log-only path
`0x544fa0(10000, msg)` (opens `error.log`, appends, closes) and returns — no
MessageBox, no `_exit` (the `_exit(param_1)` at 0x544ebf only runs when param_1 < 9999).

### 0x401c (plain field by name), for context only — not changed here
Decoded assembly:
```
0x564439  83 7f 08 00    cmp dword [edi+8],0          ; popped receiver payload
0x56444d  74 6b          jz  0x5644ba
0x5644bb  68 84 5d 5b 00 push 0x5b5d84                ; "Thread::run: [illegal fieldaccess] null.%s"
0x5644ae/c9 e8 ..        call 0x559d20
0x564525  8b 4d d0       mov ecx,[ebp-0x30]           ; local_34: never set on the null branch
0x56452a  89 0c 90       mov [eax+edx*4],ecx          ; pushes that stale pointer
```
So the *other* explicit-object opcode also reports and continues (with an undefined
value), never kills the thread. Our `Thread::field_name` (src/script/vm.cpp:1079) still
`fail()`s; that is out of scope for this card.

### Shipped-corpus tie
`bin/the game/system/scripts/io/MouseCursor.class`, `cleanup` body, line 0x93
(`ResourceRef[] ct = this.getChildNodes(); ... ct.length`):
```
2c 93 00 | 11 93 00 02 00 00 00 | 01 93 00 01 00 00 00 | 1b 93 00 83 00 00 00 | ...
```
`0x11` operand 2 opens the two-token receiver window; the second token is TREE `0x1b`
(field-ref const 0x83 = 131, NAME_TYPE `length:I` on `[Ljava.util.resource.ResourceRef;`),
i.e. exactly evalName's later-segment path above.

## Files
- edit: `src/script/vm.cpp` — `Thread::field_member`, inside the `if (explicit_recv)`
  block right after `vm_->materialize(recv, &recv, &e)` (currently line ~968-972)
- read only: `src/script/vm.h` (`Value::make_null`, `Value::make_int`)

## Exact change
```cpp
    if (explicit_recv) {
        if (!pop(f, &recv)) return false;
        std::string e;
        if (!vm_->materialize(recv, &recv, &e)) return fail(e);
        if (recv.kind == Value::Null) {
            // 0x56230d..0x562340: Thread::evalName reports the null receiver via
            // 0x559d20 ("Thread::evalName: null.methodcall()") and then, without
            // any thread-state change, dereferences the null instance ([null+0xc]).
            // The exe therefore has no value here and does not kill the thread.
            fprintf(stderr, "Thread::evalName: null.methodcall()\n");
            // INVENTED: the exe faults at 0x562340/0x5624e4. Use the empty-array
            // length for .length (the caller's loop then runs zero times) and null
            // for any other member so the thread keeps running.
            push(f, fname == "length" ? Value::make_int(0) : Value::make_null());
            return true;
        }
    }
```
Keep the existing `Array`/`Type`/`Obj` branches untouched; this only catches
`Value::Null` (not primitive receivers, whose exe path is the separate
"field access on primitive variable" report and is not exercised by this corpus).

Note for the report: the port has no script-error service equivalent to 0x559d20
(which also prefixes `!Script error`/`Thread:` and dumps frames); a `fprintf(stderr, ...)`
line is the minimal stand-in. Say so in the report; do not invent a VM error API.

## Unresolved (do not guess in the report)
- Whether an outer SEH frame catches the 0x562340 access violation. `Thread::run`
  0x562d70 installs MSVC C++ EH funclets (`puStack_c = &LAB_00590b45`; at 0x590b45
  `mov eax,0x5d7c78; jmp 0x56dac5`, a `__CxxFrameHandler`-style funclet, and a second
  at 0x590b67). Those are for unwinding locals; there is no evidence they handle an
  access violation, and what such a handler would do to the thread is unknown.
- The value the 0x401c null path pushes (0x56452a stores `[ebp-0x30]`, which is not
  written on that branch) is a stale stack slot, not a defined null.
- A second function with the same evalName strings exists at 0x562540 (duplicates at
  0x5b5f0c/0x5b5f30). Its caller and whether it has the same null path were not checked;
  `Thread::run` case 0x1011 is proven to call 0x562280.

## Done when
- The null-receiver branch cites 0x56230d/0x56233d/0x54caa0 and the substituted value is
  marked `INVENTED:`.
- `tools/vmboot-capped.sh --selfcheck` (never run src/build/vmboot without the caps)
  reaches the same frame count as before and the two previously failing threads no
  longer stop at `MouseCursor.cleanup` / `ct.length`; report the new thread/failure counts.
- `make -C src clean check` green; no new unit tests, no hand-made expected values.
- Card frontmatter: `status: implemented`, `branch: card/117`.
