---
id: 060
status: draft
branch:
depends: []
---
# script VM: java.lang.Object.runMethod overloads (int/float/Object/void)

## Goal
Replace the four `java.lang.Object.runMethod` registry stubs (0x4dff40, 0x4dfda0,
0x4e00d0, 0x4e0260) with ports. Each resolves the receiver's resource through its
native handle, takes the resource's script instance (`T+0x50`) and calls the named
script method synchronously with one argument (none for the void overload), returning
the method's raw value as an int. Missing handle → log + 0; missing script/typed data,
or a method that does not run → 0.

## Exe evidence (cleaned pseudocode, renamed fields)
`getArgs` = `Native__getArgs 0x4b1af0` (receiver, then declared args; our `a[0]`, `a[1]`, ...).
`instanceToHandle` = 0x4ecc70 (`h+0xc` = resource instance RI; notes/13). `getTyped` =
`Resource__getTyped 0x51f4f0`. Our `Runtime::call_method` is the port of 0x47dd70
(src/game/runtime.cpp:303, comment "0x47dd70").

### 0x4dff40 java.lang.Object.runMethod(Ljava.lang.String;I)I
```
getArgs(ctx, &receiver, &name, &arg);          // arg: int
h = instanceToHandle(receiver);
if (!h) {
    log("vm_object_runmethod_int: scripted resource ");       // 0x5a6744
    log(instanceToHandle(receiver));                          // second call, as %x
    log(" not found in file \""); log(vm->[+0x74]); log("\" at line ");
    log(vm->[+0x70]); log_flush();
    return 0;
}
RI = h->0xc; if (!RI) return 0;
if (RI->0x7c != 1) RI->vf5(0);                 // vtable +0x14, arg 0 here (raw: `(0)`)
if (Resource__getTyped(RI, 0xa0000000, 0, 0) < 0) return 0;
T = RI->vf3(1.0f);                             // vtable +0xc: typed data
if (!T || !T->0x50) return 0;                  // T+0x50 = resource's script instance
return call(T, name, 0, 2, arg, 0x4f);         // 0x47dd50: want_result=0, tag 2 (int)
```

### 0x4dfda0 java.lang.Object.runMethod(Ljava.lang.String;F)I
Same body; error prefix `"vm_object_runmethod_float: scripted resource "` (0x5a03dc);
`getArgs(ctx, &receiver, &name, &arg)` with `arg: float`; call `(T, name, 0, 3, (double)arg, 0x4f)`.
Raw line of the typed-data fetch in this variant: `iVar2 = FUN_0051f4f0();` (Ghidra
dropped the arguments; the other three show `(0xa0000000,0,0)`).

### 0x4e00d0 java.lang.Object.runMethod(Ljava.lang.String;Ljava.lang.Object;)I
Same body; error prefix `"vm_object_runmethod_ref: scripted resource "` (0x5a0650);
`arg: Instance*`; call `(T, name, 0, 4, arg, 0x4f)`.

### 0x4e0260 java.lang.Object.runMethod(Ljava.lang.String;)I
```
getArgs(ctx, &receiver, &name);
h = instanceToHandle(receiver);                // same failure log + return 0:
                                               // "vm_object_runmethod_void: scripted resource " (0x5a0870)
RI = h->0xc; if (!RI) return 0;
if (RI->0x7c != 1) RI->vf5(0);
if (Resource__getTyped(RI, 0xa0000000, 0, 0) < 0) return 0;
T = RI->vf3(1.0f);
if (!T || !T->0x50) return 0;
return call(T, name, 0, 0x4f);                 // no args: the terminator is the first tag
```

### 0x47dd50 -> 0x47dd70 Method call (notes/18 §42-48)
```
0x47dd50(T, name, want_result, tag, ..., 0x4f) forwards to
0x47dd70(T, name, want_result, va_list):
    if (T->0x50 == 0) return 0;
    RI2 = T->0x44; if (!RI2) return 0;
    if (RI2->0x7c != 1) RI2->vf5(1.0f);
    if (Resource__getTyped(RI2, 0x80000000,0,0) < 0) return 0;
    if (RI2->vf3(1.0f) == 0) return 0;
    thread = new ScriptThread("THRD-RUN VMI <id> <name>", prio 10);   // 0x5612f0
    push_args(thread, tag, va);              // 0x47d250, tags below
    rv = FUN_0054c550(thread, name);         // notes/18: resolve + run to completion
    if (rv == 0) FUN_00562d70(0);            // failure side effect (untraced)
    if (want_result == 0 && rv == 0) return 0;
    v = FUN_00562a30();                      // result value descriptor
    if (v->type[0] == 'L' && is_subclass("java.lang.Native", v->type))
        return instanceToHandle(v->value);   // 0x549d80 + 0x4ecc70
    return v->value;                         // raw 32-bit slot
```
`0x47d250` va-list tags: `2` = int (`v[2] = i`), `3` = double→float (`v[2] = (float)d`),
`4` = object ref (`v[2] = ptr`, 0 → null), `0x4f` (79) = end of list.

Note: notes/18 §42-48 names 0x561700 for resolution and 0x562d70(0.0) for the run;
the decompiled 0x47dd70 shows 0x54c550 / 0x562d70(0) / 0x562a30. Semantics (synchronous,
0 on missing method) are the same and are already encapsulated in `Runtime::call_method`.

## Mapping to src (established in notes/13, cards 050/057)
- `h = value_handle(a[0])` (Runtime::Handle*, 0x4ecc70); null → log + return 0.
- `h->target` = `RI` (`h+0xc`); null → silent 0.
- streaming/typed-data calls (`vf5`, `getTyped`, `vf3`) have no analogue: our
  `Runtime::Resource` *is* the typed data (notes/13 §1). `T+0x50` is `Resource::script`,
  fetched with `rt->script_instance(h->target)` (creates the default instance, as in
  runtime.cpp:285). Null → 0.
- The call is `rt->call_method(self, name, args, &result, &e)`; false (missing method /
  thread did not finish) → return 0 and ignore `e` (the exe logs from the thread layer;
  `ch_force_update` at natives.cpp:2602 does the same).
- Arg vector: int `{Value::make_int(a[2].i)}`, float `{Value::make_float(a[2].f)}`,
  ref `{a[2]}` (may be Null), void `{}`.

## Files
- edit: src/script/natives.cpp — in the java.lang.Object section next to
  `object_hashcode` (~line 955) add one helper and four native functions; registry rows
  4801-4804 change from `nullptr, true` to those functions, `false`.
- read only: src/game/runtime.h — `Handle::target`, `Resource::script`,
  `script_instance`, `call_method`.
- read only: src/script/vm.h — `Value` kinds, `Object`.

Implementation shape:
```cpp
// 0x4dff40 / 0x4dfda0 / 0x4e00d0 / 0x4e0260 Object.runMethod
static int32_t call_result_i(const Value &v)
{
    switch (v.kind) {
    case Value::Int: return v.i;
    case Value::Float: { int32_t b; memcpy(&b, &v.f, sizeof b); return b; }  // raw slot
    case Value::Obj: return (int32_t)(intptr_t)v.obj;   // exe returns piVar4[2]
    default: return 0;  // INVENTED: raw String/array slots have no rebuild equivalent
    }
}

static bool object_run_method(const char *who, const Args &a, int argi,
                              Value *out, std::string *err)
{
    if (!want(who, a, argi >= 0 ? (size_t)argi + 1 : 2u, err)) return false;
    Object *o;
    const std::string *name;
    if (!get_o(who, a[0], &o, err) || !get_s(who, a[1], &name, err)) return false;
    Runtime *rt = game_rt();
    Runtime::Handle *h = value_handle(a[0]);            // 0x4ecc70
    *out = Value::make_int(0);
    if (!h) {
        // 0x4dff40: frame file/line is not available to NativeFn; prefix only.
        std::fprintf(stderr, "%s: scripted resource not found\n", who);
        return true;
    }
    Object *self = h->target ? rt->script_instance(h->target) : nullptr;  // T+0x50
    if (!self) return true;
    std::vector<Value> args;
    if (argi >= 0) args.push_back(a[argi]);
    Value rv; std::string e;
    if (!rt->call_method(self, *name, args, &rv, &e)) return true;  // 0x47dd70 -> 0
    *out = Value::make_int(call_result_i(rv));
    return true;
}

bool object_runmethod_int(Vm &, const Args &a, Value *out, std::string *err)
{ return object_run_method("vm_object_runmethod_int", a, 2, out, err); }
// float and ref: same with their prefixes, argi 2; void: argi -1.
```

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED
- 0x4dfda0 raw line `iVar2 = FUN_0051f4f0();` — arguments lost by the decompiler;
  the int/ref/void variants call `(0xa0000000,0,0)`.
- 0x47dd70 `'L'` return path: `FUN_0055add0("java.lang.Native")`, `FUN_00549d80` and
  `piVar4[1]` are untraced; whether our `Runtime::Handle*` (or the object pointer) is the
  right int for a Native-subclass result is unresolved. Non-Native objects return the raw
  instance pointer (`return piVar4[2];`), which `call_result_i` uses.
- 0x47dd70 failure side effect `FUN_00562d70(0)` is untraced (raw line above).
- Null `name` (String arg is null): the exe passes `0` into method resolution; our
  `get_s` rejects it with a script error instead. Exe behaviour with a null name is unverified.
- `instanceToHandle`'s second argument `DAT_007a2620` (script class) is ignored by our
  `value_handle`; only its return handle is modelled (notes/13).
