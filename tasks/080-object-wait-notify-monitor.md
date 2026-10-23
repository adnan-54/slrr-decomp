---
id: 080
status: implemented   # gated 2026-10-23 (0x54d430/0x564950/0x54d320/0x54d3a0/0x5629a0 + Thread::run 0x563290)
branch: card/080
depends: []
---
# script VM: Object.wait/notify/notifyAll on the cooperative Scheduler monitor

## Goal
Replace the three `nullptr` registry rows `java.lang.Object.wait()V` (0x4e0440),
`notify()V` (0x4dfc70) and `notifyAll()V` (0x4dfc90) with ports of the exe natives.
They are a monitor over the script Object receiver: `wait` appends the **calling
engine Thread** to the receiver's wait list (exe: container pointer at object+0x14)
and clears its run-list membership (thread+0x2c bit 0x10 = waiting); `notify` pops
the **last** waiter (LIFO) and makes it runnable again; `notifyAll` pops and wakes
every waiter. All three return `V`, i.e. no value. In src the equivalent of the
"current engine Thread" is the `ScriptThreadState` whose `runner` is executing
inside `pump_threads`; a `waiting` flag makes `pump_threads` skip it, which replaces
the exe's unlink/relink of the engine Thread between the VM's run and wait lists.

## Exe evidence (cleaned pseudocode, renamed fields)
`getArgs` = 0x4b1af0 (documented in cards 057/079): output 0 = receiver, then one
32-bit slot per declared signature char; it reads the caller frame at `ctx+8` and
never touches `ctx+4`.
`ctx` = `{+0 vm, +4 engine Thread, +8 &args, +0xc nativeInfo}` (notes/06). The
dispatch builds it explicitly in `Thread::callMethod` 0x561830:
`local_54 = thread->+0x10 (vm); local_50 = thread; local_4c = &args; local_48 = nativeInfo; fn(&local_54)`.
So `*(ctx+4)` is the thread executing the native.
Helper names: 0x54d430 `Monitor__wait`, 0x564950 `Thread__wait`, 0x54d320
`Monitor__notify`, 0x54d3a0 `Monitor__notifyAll`, 0x5629a0 `Thread__notify`.
`T_Container` = `{+0 capacity, +4 data, +8 count}` (0x567a20 alloc 0xc, 0x5679b0
realloc, 0x567950 free; plain growable array, tail appended / tail popped).

### 0x4e0440 java.lang.Object.wait()V
    monitor = getArgs(ctx, &monitor);                 // a[0]
    Monitor__wait(ECX = monitor, arg = (void*)*(ctx+4));   // 0x54d430
Raw bytes at 0x4e0440: `... 8b 4d fc 83 c4 08 ff 76 04 e8 d0 cf 06 00 5e 8b e5 5d c3`
(`mov ecx,[ebp-4]` = receiver -> ECX, `push [esi+4]` = ctx+4 -> stack arg, call 0x54d430).

### 0x4dfc70 java.lang.Object.notify()V
    monitor = getArgs(ctx, &monitor);
    Monitor__notify(ECX = monitor);                   // 0x54d320
Raw bytes: `8b 4d fc 83 c4 08 e8 95 d6 06 00` (call 0x54d320).

### 0x4dfc90 java.lang.Object.notifyAll()V
    monitor = getArgs(ctx, &monitor);
    Monitor__notifyAll(ECX = monitor);                // 0x54d3a0
Raw bytes: `8b 4d fc 83 c4 08 e8 f5 d6 06 00` (call 0x54d3a0).

### 0x54d430 Monitor__wait(ECX = monitor, arg = thread)
    Thread__wait(ECX = thread, arg = monitor);        // 0x564950, arg = self (esi)
    if (*(void**)(monitor + 0x14) == 0)               // lazy container
        *(void**)(monitor + 0x14) = T_Container{0,0,0};   // 0x567a20 alloc 0xc
    c = *(T_Container**)(monitor + 0x14);
    if (c->capacity < c->count + 1) grow(c, c->capacity + c->count + 1);  // 0x5679b0
    c->data[c->count] = thread;                        // tail append
    c->count += 1;
No logging and no null checks at this level.

### 0x564950 Thread__wait(ECX = thread, arg = monitor)
    if (thread->flags(+0x2c) & 0x80) log(vm, "Thread::wait: thread already died");
    if (thread->flags & 0x10)        log(vm, "Thread::wait: already waiting");
    unlink(thread) from its list using (+4, +8) links;
    thread->flags |= 0x10;                              // waiting
    thread->+4 = [[thread->+0x10 (vm)] + 0x1c]->+0x18 ...   // link into VM wait list
    thread->+8 = ...;
    thread->+0x34 = monitor;                            // wait object
Both log checks fall through: an already-waiting thread is still relinked and the
caller still appends it to the monitor container a second time.

### 0x54d320 Monitor__notify(ECX = monitor)
    c = *(T_Container**)(monitor + 0x14);
    if (c == 0 || c->count == 0) return;
    for (;;) {                                          // pops the tail = LIFO
        n = c->count;
        if (n == 0) { log(vm, "T_Container: underflow"); continue; }
        c->count = n - 1;
        t = c->data[n - 1];
        c->data[n - 1] = 0;                             // slot cleared
        if (t != 0) { Thread__notify(ECX = t); return; } // exactly one thread
    }
Null slots are skipped (not produced by src's vector).

### 0x54d3a0 Monitor__notifyAll(ECX = monitor)
Same loop and same `Thread__notify(ECX = t)` tail call, but **no return** after a
non-null thread: it drains the container (tail to head) until `count == 0`.

### 0x5629a0 Thread__notify(ECX = thread)
    if (thread->flags(+0x2c) & 0x80) log(vm, "Thread::notify: thread already died");
    if (!(thread->flags & 0x10))     log(vm, "Thread::notify: not waiting");
    thread->flags &= ~0x10;                             // no longer waiting
    if (!(thread->flags & 0x20)) {                      // 0x20 meaning UNRESOLVED
        unlink(thread) from the wait list (+4/+8);
        relink thread at the tail of [thread->+0x10 (vm)]->+0x18 ...  // run list
    }
Return `V` / no value.

## Gate addendum (Claude, verified in Thread::run asm): the thread stops right after wait()
Thread::run 0x562d70 loop tail, executed after EVERY instruction (only the time check is
gated by the 256-instruction counter at [ebp-0x34]):

    0x563261: cmp byte [ebp-0x15],0 ; jnz 0x563290
    0x563267..0x56328a: every 256th instruction: if (now >= budget) goto exit 0x564607
    0x563290: test byte [thread+0x2c], 0xfa
    0x563294: jnz 0x564607            // any flag except 0x1/0x4 set -> leave run()
    0x56329d: jmp 0x562e20            // next instruction
    0x564607: save ip into the frame (+0x2c); and [thread+0x2c], ~4 (clear running); return

So after `wait()` sets 0x10 the thread leaves its slice at the next instruction boundary;
the remaining instructions of the slice must NOT run. src's `run_slice` only stops at the
instruction budget, so add (this is change 0, done before the others):

0. `src/script/vm.h` class `Thread`, public: `void request_stop() { stop_ = true; }`
   (comment: `// 0x563290: thread+0x2c & 0xfa checked after every instruction`),
   private: `bool stop_ = false;`. `src/script/vm.cpp` `Thread::run_slice`: after a
   successful `step()`, `if (stop_) { stop_ = false; status_ = frames_.empty() ? Done :
   Yielded; return status_; }`. In `object_wait`, after `t->waiting = true;` call
   `t->runner->request_stop();`.
   shortcut: Thread.suspend of the running thread itself still runs to its slice end in
   src (exe: same 0xfa test; suspend's bit not traced). Do not change suspend here.

## Files
- edit: `src/script/vm.h`, `src/script/vm.cpp` (change 0), `src/script/natives.cpp` — `ScriptThreadState`, `Scheduler`,
  `pump_threads`, new `object_wait` / `object_notify` / `object_notify_all` next to
  `object_hashcode`, and the three registry rows at ~5835.

## Changes
1. `ScriptThreadState` (natives.cpp:113), after `in_slice`:
   ```cpp
   bool waiting = false;   // engine thread+0x2c bit 0x10: on the monitor wait list
   ```
2. `Scheduler` (natives.cpp:126), after `pumping`:
   ```cpp
   // 0x54d430 gets the executing engine Thread as ctx+4; the equivalent here is the
   // ScriptThreadState whose slice is running (set by pump_threads).
   ScriptThreadState *current = nullptr;
   // Receiver's wait list: the exe keeps a T_Container* at object+0x14. Object
   // identity is the key; entries are appended and popped at the tail (LIFO).
   std::map<Object *, std::vector<ScriptThreadState *>> monitors;
   ```
3. `pump_threads` (natives.cpp:188): add `|| t->waiting` to the skip condition and
   bracket the slice with the current thread:
   ```cpp
   if (!t->runner || t->suspended || t->done || t->waiting || !t->started ||
       t->in_slice)
       continue;
   t->in_slice = true;
   s.current = t;                                  // ctx+4
   Thread::Status st = t->runner->run_slice(256);
   s.current = nullptr;
   t->in_slice = false;
   ```
   (`thread_sleep` calls `pump_threads` from inside a slice; the `s.pumping` guard
   keeps `s.current` pointing at the sleeping thread, which is correct.)
4. New natives next to `object_hashcode` (natives.cpp:1272):
   ```cpp
   // 0x5629a0 Thread__notify (ECX = thread): clear the waiting flag and make the
   // thread runnable again.
   void monitor_notify_thread(ScriptThreadState *t)
   {
       // INVENTED: exe flag 0x80 (dead) is modelled by our `done`.
       if (t->done) std::fprintf(stderr, "Thread::notify: thread already died\n");
       if (!t->waiting) std::fprintf(stderr, "Thread::notify: not waiting\n");
       t->waiting = false;
       // shortcut: exe skips the run-list relink when thread+0x2c bit 0x20 is set
       // (meaning UNRESOLVED); pump_threads already gates suspended/done threads.
   }

   // 0x4e0440 Object.wait()V -> 0x54d430(monitor, ctx+4 thread) -> 0x564950.
   bool object_wait(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       Object *o;
       if (!want("Object.wait", a, 1, err) ||
           !get_o("Object.wait", a[0], &o, err))
           return false;
       *out = Value::make_null();
       Scheduler &s = sched(vm);
       ScriptThreadState *t = s.current;
       // shortcut: a native invoked synchronously (Vm::invoke / run_field_init,
       // e.g. a static initializer or an engine callback) has no executing script
       // thread in src; the exe always has ctx+4. No-op, out stays null (V).
       if (!t) return true;
       // INVENTED: 0x80 (dead) maps to our `done`.
       if (t->done) std::fprintf(stderr, "Thread::wait: thread already died\n");
       if (t->waiting) std::fprintf(stderr, "Thread::wait: already waiting\n");
       s.monitors[o].push_back(t);      // monitor+0x14 tail append
       t->waiting = true;               // thread+0x2c |= 0x10, off the run list
       return true;
   }

   // 0x5629a0 0x54d320 Object.notify()V: wake the last waiter of this object.
   bool object_notify(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       Object *o;
       if (!want("Object.notify", a, 1, err) ||
           !get_o("Object.notify", a[0], &o, err))
           return false;
       *out = Value::make_null();
       Scheduler &s = sched(vm);
       auto it = s.monitors.find(o);
       if (it == s.monitors.end() || it->second.empty()) return true;
       ScriptThreadState *t = it->second.back();   // tail pop, LIFO
       it->second.pop_back();
       monitor_notify_thread(t);
       if (it->second.empty()) s.monitors.erase(it);
       return true;
   }

   // 0x54d3a0 Object.notifyAll()V: drain the object's wait list, tail to head.
   bool object_notify_all(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       Object *o;
       if (!want("Object.notifyAll", a, 1, err) ||
           !get_o("Object.notifyAll", a[0], &o, err))
           return false;
       *out = Value::make_null();
       Scheduler &s = sched(vm);
       auto it = s.monitors.find(o);
       if (it == s.monitors.end()) return true;
       for (size_t i = it->second.size(); i-- > 0;)   // exe pops tail first
           monitor_notify_thread(it->second[i]);
       s.monitors.erase(it);
       return true;
   }
   ```
   Do not call `pump_threads` from notify/notifyAll: the exe only relinks the thread;
   woken threads run on the scheduler's next pass (`shortcut:` src's pass is the
   next `Thread.sleep`).
5. Registry rows (natives.cpp:5832-5837), replacing the stub comment and rows:
   ```cpp
   // 0x4e0440/0x4dfc70/0x4dfc90: monitor natives over Scheduler::monitors; wait
   // appends the pumped thread and marks it waiting (0x54d430/0x564950), notify
   // wakes the last waiter, notifyAll drains the list (0x54d320/0x54d3a0/0x5629a0).
   {"java.lang.Object", "wait", "()V", object_wait, false},
   {"java.lang.Object", "notify", "()V", object_notify, false},
   {"java.lang.Object", "notifyAll", "()V", object_notify_all, false},
   ```
   Only these three rows change; `Object.toString/disableGC/enableGC/hashCode` and
   `errorVM` rows are untouched.

## UNRESOLVED
- **Thread+0x2c bit 0x20** in 0x5629a0: the relink into the run list is skipped when
  it is set, but what it means was not traced. Raw line:
  `if ((*(byte *)(in_ECX + 0x2c) & 0x20) == 0) { ... relink ... }`. The port clears
  only `waiting`; `pump_threads` already gates `suspended`/`done`, so the observable
  scheduling matches whether 0x20 is the suspend bit or not. Do not add a new field
  for it.
- **VM run/wait list linkage** (`[vm+0x18]` run list, `[vm+0x1c]` wait list,
  `*(thread+0x10)` = vm) is not modelled. Raw line:
  `iVar1 = *(int *)(*(int *)(in_ECX + 0x10) + 0x1c);`. src's scheduler iterates
  `Scheduler::threads` and uses the `waiting` flag instead; registration order, not
  run-list order, decides slice order (pre-existing scheduler design).
- **Engine-side monitor wake not ported**: notes/12 (`FUN_0054d3a0` called by the
  frame pass for `java.render.Frontend`'s `render` monitor) — scripts that
  `Frontend.render.wait()` are still only woken by a script `notify/notifyAll` in
  src, because no frame pass calls the wait-list drain. Belongs to a FrameLoop card.
- **Thread dies while waiting**: what the exe does with the thread's monitor entry
  on Thread teardown was not traced. src leaves it until a later notify, which then
  logs `Thread::notify: thread already died` and clears the flag (0x5629a0 path).
- **No current thread**: a native invoked by a synchronous `Vm::invoke` /
  `run_field_init` (static initializer, engine callback) has no engine Thread in
  src because `NativeFn` only receives `Vm&`. The exe cannot be in that state
  (ctx+4 is always set); the port is a no-op and the V native still yields null.
- **Receiver guard**: `get_o` rejects a non-Object receiver; the exe would treat
  the raw value as an Instance and dereference +0x14 (UB). Kept consistent with the
  existing `object_hashcode`/`object_tostring` guards (`INVENTED:` at most).

## Done when
- each ported function carries its `// 0xADDR Name` comment; untraced logic is
  `INVENTED:` / `shortcut:` / `UNRESOLVED:` as above; no exe behaviour "improved"
- `wait()` on a pumped script thread appends it to that receiver's list once per
  call (duplicates allowed), sets `waiting`, and `pump_threads` then skips it;
  `notify()` wakes only the most recent waiter of that exact receiver; `notifyAll()`
  wakes all of them; other receivers' waiters are untouched; no waiters = no-op
- a second `wait()` logs `Thread::wait: already waiting` and appends again; notify
  of a non-waiting/dead thread logs the 0x5629a0 lines
- `wait/notify/notifyAll` outside `pump_threads` are no-ops returning null (V)
- the three registry rows point at the new functions with `engine = false`; the old
  "need the engine thread monitor" stub comment is gone
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
