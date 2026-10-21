---
id: 041
status: implemented
branch: card/041
depends: [040]
---
# events: Runtime::queue_event (0x47c760) + script handleEvent call (0x47c960 / 0x47dd70)

## Goal
Port the exe's event dispatcher. `Game__queueEvent` (0x47d550) is a jump stub to
0x47c760 and dispatches **synchronously**: mask test on the target, script call
`handleEvent(...)` on the target's script instance, then forward through the
target's notification list (card 040). Add a reusable "call a script method by
name and run it to completion" helper (0x47dd70); card 046 reuses it. Also record
the receiver of `GameType.createNativeInstance` as the instance's script object so
script-created GameTypes (e.g. `Osd`) receive their events.

## Exe evidence (cleaned pseudocode, renamed fields)
`typed` = our `Resource`; `typed+0xa0` = `event_mask`; `typed+0x50` = `script`;
`typed+0x5c` = `notifications` (card 040 field names).

### 0x47c760 Game::queueEvent(target, sender, code, param, method)
    // streaming/load checks (RI+0x7c == 1, 0x51f4f0 >= 0) — always true here
    t = target;
    if ((t->event_mask & code) != 0
        && (t has no native handler (typed+0x4c) || native_handler(...) == 0)
        && t->script != null)
        dispatchScript(target, sender, code, param, t, method);      // 0x47c960
    for each n in t->notifications (head first; take next before handling n):
        if (n.watcher == null /* id 0 */) { delete n; continue; }
        if ((code & n.mask) == 0) continue;
        c = (n.alias & 0x0fffffff) ? n.alias : code;
        p = (n.alias & 0x10000000) ? n.message (null if !has_message) : param;
        if (!(sender != null && (n.mask & 0x40000000)))
            queueEvent(n.watcher, sender ? sender : target, c & 0x0fffffff, p,
                       n.has_method ? n.method : null);
        if (n.mask & 0x80000000) delete n;          // one-shot
    return 1;

### 0x47c960 dispatchScript(target, sender, code, param, t, method)
    obj = sender ? sender : target;
    ev  = code & 0x0fffffff;
    if (code == 0x100000) {                         // EVENT_HOTKEY, full compare
        call(t, "handleEvent", [param /* Hotkey object */]); return;
    }
    // ev 1/4/0x20/0x40 format the contact record in param into a String; 0x80 passes an int;
    // default: param is a char* -> String (null -> null)
    ref = new GameRef wrapping obj;                 // 0x47aff0 / 0x47b050
    if (method == null) method = (int field "asyncMode" of t->script != 0) ? "handleEventAsync" : "handleEvent";
    call(t, method, [ref, ev, p]);

### 0x47dd70 call(typed, name, want_result, args...)
    if (!typed->script || !typed RI) return 0;
    thread = new Thread("THRD-RUN VMI <id> <name>");   // 0x5612f0
    push args (tags: 2 int, 3 float, 4 object/string, 0 -> null);   // 0x47d250
    if (!callMethod(thread, name))  -> "Thread::callMethod: \"%s\" not found", thread killed, return 0
    Thread::run(0.0)  // 0x562d70: no time slice, runs to completion (or until it blocks)
    return result;

### 0x4dc510 GameType.createNativeInstance
    factory 0x5240e0(parent, &h, receiver_script_object, params, alias)
    // [inferred] the receiver script object becomes typed+0x50 of the new instance

## Changes
1. `src/game/runtime.h` (`class Runtime`, public, after `script_instance`):
   ```cpp
   // 0x47dd70: run `name` on self with args to completion. False when the
   // method is missing or the thread did not finish (error text in *err).
   bool call_method(Object *self, const std::string &name,
                    const std::vector<Value> &args, Value *result, std::string *err);
   // 0x47d550 -> 0x47c760: synchronous event dispatch. param is a Value::Str,
   // Value::Int, Value::Obj or Value::Null; method null = default handler name.
   void queue_event(Resource *target, Resource *sender, uint32_t code,
                    const Value &param, const char *method = nullptr);
   ```
   `Value` is only forward-usable by reference: add `struct Value;` next to the
   existing `struct Object;` forward declaration.
2. `src/game/runtime.cpp`:
   - `call_method`: `vm_` null → false. `ClassState *decl = nullptr;
     const MethodInfo *m = vm_->find_method(self->cls, name, "", (int)args.size(), args, &decl, err);`
     get the unit like `class_state_unit` in natives.cpp (copy that 4-line static helper:
     `const ClassUnit *unit = nullptr; if (decl) ::memcpy(&unit, decl, sizeof unit);`),
     then `vm_->invoke(*unit, *m, self, args, result, err)`.
   - `queue_event`: exactly the pseudocode above; `target == nullptr` → return.
     Script call: `if ((target->event_mask & code) && target->script)` — mark
     `// shortcut: no native type handlers (typed+0x4c) or streaming checks; upgrade when engine types exist.`
     Hotkey branch: `call_method(script, "handleEvent", {param}, ...)`.
     Default branch: `ref = wrap(obj, &e)` (`Value::make_null()` when it fails),
     `asyncMode` via `vm_->get_instance_field(script, "asyncMode", &v, &e)` (missing/non-int → 0),
     args `{ref, Value::make_int(ev), param}`.
     INVENTED: param is passed through as given for ev 1/4/0x20/0x40 (the exe formats a
     contact record; no caller of those codes exists yet).
     Ignore `call_method` failures (the exe logs and continues).
     Notification loop: iterate by index over a copy-safe scheme (handlers may add/remove
     notifications): take a snapshot `std::vector<Notification> list = target->notifications;`
     forward from the snapshot, then erase one-shot entries from the live list by
     matching `watcher/mask/alias`. Mark it `// INVENTED: snapshot iteration; the exe walks the live list.`
     INVENTED: `n.watcher == nullptr` never happens (no destroy hook) but keep the check.
3. `src/script/natives.cpp`, `gt_create_native_instance`: after `rt->bind(h, inst);` add
   `if (!inst->script) inst->script = o;  // 0x4dc510 -> 0x5240e0 receives the receiver script object (inferred typed+0x50)`.

## Files
- edit: src/game/runtime.h, src/game/runtime.cpp, src/script/natives.cpp (one line in gt_create_native_instance)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
