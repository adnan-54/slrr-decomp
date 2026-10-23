---
id: 103
status: todo
branch:
depends: [102]
---
# game: scripted instance creation runs the constructor in a THRD-CREATE thread (0x523c80)

## Goal
`Runtime::instantiate(..., scripted = true)` creates the script object but never runs its
constructor (shortcut in `Runtime::script_instance`). The exe (0x523c80, used by
GameRef.create and by the main loop's "GameInit") creates an engine Thread "THRD-CREATE"
(priority 10, flags 0, linked into the VM run list), calls `<init>` with ONE int argument
= the new instance id, and runs that thread untimed right away (`Thread::run(0)`). If the
constructor sleeps / waits / suspends, the thread stays on the run list and the scheduler
(card 102, `run_threads`) continues it later. Port that.

## Exe evidence (Claude)
### 0x523c80 create(typeRef, parentRef, params, alias) — scripted part, in order
    id = 0x518a40();  node = register(id, alias or "_gameinst", type 1 ins_game ...);
    ...  piVar2[0x21] |= 0x200;  ...
    cls = 0x468b00(type)  (script entry of the type: +0xc native ctor, +0x10 script class)
    if (cls->native) cls->native->construct(...)        // not in src
    if (cls->script /* +0x10 */) {
        t = new Thread(vm [0x5fe0c4], prio 10, flags 0, "THRD-CREATE");   // 0x5612f0
        obj = 0x549ee0();                       // new script instance of the class
        args = container; push Variable{int, value = id};      // 0x519380 / 0x548c70 / 0x562b00
        bind obj's native handle field to the new node (0x4ed320(obj, DAT_007a2620, ...));
        if (0x54c530(t, obj) == 0)              // = Thread::callMethod(cls, obj, "<init>")
            Thread__run(t, 0.0f);               // 0x562d70: budget 0 = untimed ([ebp-0x15]=1)
    }
    ... the type's native "onCreate" (vf+0x40), physics/render post steps (not in src)
### 0x5612f0 Thread::Thread(vm, prio, flags, name)
    +0x28 prio = 10; +0x2c flags = 0; name copied; pushes a base frame;
    links the thread at the TAIL of the VM run list (vm+0x18).
### 0x561700 Thread::callMethod(cls, obj, name)
    method = lookup(name, descriptor of the pushed args)  // "(I)V" here
    not found -> log "Thread::callMethod: \"%s%s%s\" not found", returns non-zero (no run)
### Thread::run untimed (0x562dcd..0x562dea): budget == 0 -> no time check; the run ends
    when the frames are done or a flag in mask 0xfa is set (sleep/wait/suspend/dead).
### Shipped data: `java.game.Init(int obsolete_id)` is such a constructor (main-loop
    GameInit, Config.init = sl:0x0040r in 2.2.1 util/src/Config.java).

## Changes
1. `src/script/natives.h` / `natives.cpp`: new
   ```cpp
   // 0x5612f0 + 0x562d70(0): register a scheduler thread (prio, flags 0) and run it
   // untimed until it finishes or a flag stops it; returns false if it failed.
   bool spawn_untimed(Vm &vm, std::unique_ptr<Thread> runner, int32_t priority,
                      const std::string &name);
   ```
   Implementation: new `ScriptThreadState` (script = nullptr, name, priority, started =
   true, runner moved in) appended to `Scheduler::threads` (tail = 0x5612f0 link). Then the
   untimed run: save `s.current` and set it to the new state, set `in_slice`, loop
   `st = runner->run_slice(256)` while `st == Thread::Yielded && !t->sleeping &&
   !t->waiting && !t->suspended && !t->done`; restore `in_slice = false` and the saved
   `s.current` (the caller may itself be a script thread inside a native, e.g.
   GameRef.create). Done -> `done = true, started = false`; Failed -> the same plus the
   existing `vm thread failed:` log, return false. A Yielded thread stays registered and
   `run_threads` continues it. Do NOT touch `s.pumping` (nested runs are what the exe does).
2. `src/game/runtime.cpp` `Runtime::instantiate`: after `r->script = script_instance(r)`
   (scripted only, and only if `r->script`): find `<init>` with descriptor `"(I)V"` via
   `vm_->find_method(r->script->cls, "<init>", "(I)V", 1, {Value::make_int((int32_t)r->id)},
   &decl, &e)`; if found and `class_state_unit(decl)` (natives.cpp helper; if not visible,
   use the same memcpy trick locally with a `shortcut:` comment or expose it in natives.h)
   start it with `vm_->start(*unit, *m, r->script, {Value::make_int((int32_t)r->id)}, &e)`
   and hand it to `natives::spawn_untimed(*vm_, std::move(runner), 10, "THRD-CREATE")`.
   Not found -> `fprintf(stderr, "Thread::callMethod: \"<init>(I)V\" not found\n")` and no
   run (0x561700). Remove the "does not run it" shortcut comment in `script_instance`
   (keep `script_instance` creating the object only; the ctor run belongs to creation).
   INVENTED: the exe binds the script object's native handle to the node before the ctor
   (0x4ed320); src's handle lookup is lazy (`handle(obj)`), so nothing extra is needed —
   say so in a one-line comment.
3. Comment `(void)params;` stays (params go to the native construct, not ported).

## Files
- edit: src/game/runtime.cpp, src/script/natives.cpp, src/script/natives.h

## Done when
- `// 0x523c80`, `// 0x5612f0`, `// 0x562d70` comments at the ported spots
- `make -C src clean check` green (if a shipped-script check now fails because a
  constructor runs, report it in the card under a `## Check impact` heading with the exact
  failing line instead of hiding it)
- card frontmatter: status implemented, branch name
