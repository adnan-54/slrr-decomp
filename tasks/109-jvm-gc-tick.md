---
id: 109
status: draft
branch:
depends: [102]
---
# script VM: JavaMachine GC tick 0x55cf20 (mark / incremental sweep / finalizer pump) and the GC natives

## Goal
Port the garbage collector behind the frame loop's `runThreads` call: `0x55cf20`
(tick: finalizer pump, start/continue an incremental collection, smoothed cost,
then `runThreads`), the mark/sweep helpers `0x559f80`/`0x55a020`/`0x55a170`/
`0x55a2d0`/`0x55a430`/`0x55a5b0`/`0x55a560`, the finalizer pump `0x559e80` with
`0x54c660`, the System.gc phase `0x559e30`, and Object.disableGC/enableGC
(`0x54c5a0`/`0x54c600`). After this card `Vm` owns a real mark/sweep collector:
unreachable script Objects are queued, their script `finalize()` runs on a later
tick, arrays are reclaimed, and GC-disabled objects are never swept. The
`system_gc` stub (`src/script/natives.cpp:809`) and the no-op
`object_gc_toggle` (`:1351`) disappear.

## Exe evidence (cleaned pseudocode, renamed fields)

Notation: `gc` = the JavaMachine singleton (`this`); `obj->[0xc]` = type; the type's
`+0xc` owner is the JavaMachine. Object flags live at `obj->[0x18]`:
bit0 = marked, bit1 = finalized, bit2 = no-GC, bit3 = condemned (pending finalize).
`[x]` is a dword read at byte offset x.

### 0x47d760 call site (already ported as FrameLoop, card 102)
```
if (7 < now - currentTimeMs) {              // every >= 8 ms
    if (*(int *)(game + 0x234) != 0) FUN_0055cf20(10.0f);
    currentTimeMs = now;                    // advanced even when no VM
}
```

### 0x55cf20 JavaMachine::tick(float slice)
```
++gc->[0x40];                               // tick counter, low byte gates the pump
FUN_00559e80(gc);                           // finalizer pump
t0 = FUN_00569730();                        // QPC milliseconds
if (gc->[0x34] > gc->[0x38]) {              // alloc_since > threshold
    if (gc->[0x20] == 0) {                  // no gray stack: begin collection
        FUN_00559f80(gc);                   // create gray stack
        FUN_0055a020(gc);                   // global roots
        FUN_0055a170(gc);                   // thread roots
        FUN_0055a2d0(gc);                   // class statics
    } else {                                // continue collection
        FUN_0055a430(gc);                   // pop at most 250 gray objects
        if (gc->[0x20] == 0) {              // stack emptied -> sweep
            FUN_0055a560(gc);
            gc->[0x34] -= gc->[0x38];
        }
    }
}
t1 = FUN_00569730();
gc->[0x24] = (int)((double)gc->[0x24] * 0.2 + (double)(t1 - t0) * 800.0);
FUN_0055caf0(gc, param_1);                  // runThreads(10.0f, 0)
```
Constants verified in the data section: `0x5a6850` = double 0.2, `0x5b5250` =
double 800.0. `0x569730` is the QPC delta in milliseconds (notes/03).

### 0x559e80 finalizer pump
```
if ((uint8_t)gc->[0x40] == 0) {             // every 256th tick
    A = length(gc->[0x44] list);            // pending-finalize queue
    B = length(gc->[0x30] list);            // finalized/recycle list
    budget = ((B / gc->[0x3c]) + A) >> 6;   // IDIV [gc+0x3c]; old budget must be != 0
    if (budget < 4) budget = 4;             // CMOVL
    gc->[0x3c] = budget > 0x40 ? 0x40 : budget;
}
for (i = 0; i < gc->[0x3c]; i++) {
    if (gc->[0x44] empty) break;
    FUN_0054c660(head of gc->[0x44]);       // finalize one, moves it to gc->[0x30]
    if (i == gc->[0x3c] - 1 && gc->[0x44] emptied exactly now) {
        splice gc->[0x30] into list at *(gc->[0x2c] + 0x18);  // allocator recycle
        re-init gc->[0x30];
    }
}
```

### 0x559f80 begin collection (gray stack)
```
c = alloc 0xc-byte T_Container; c->data = alloc(0, 0x100);  // 64 dword slots
c->cap = 0x40;                                               // 64 objects
gc->[0x20] = c;                                              // null on alloc failure
```

### 0x55a020 global roots
```
for G in {DAT_005fe0e4, DAT_005fde5c, DAT_005fe0d4}:
    if (!G) continue;
    if (G->[0x1f] != 1) vfunc(G, +0x14)(1.0f);      // lazy init
    if (FUN_0051f4f0(0x80000000, 0, 0) < 0) continue;
    o = vfunc(G, +0xc)(1.0f);
    if (o && o->[0x50]) { o->[0x50]->flags |= 1; push o->[0x50] to gray; }
```
`o->[0x50]` is a native object's script instance (notes/06, GameRef.create).

### 0x55a170 thread roots (two lists: gc->[0x18], gc->[0x1c])
```
for each engine thread N in each list:
    if (N->[0x18]) { N->[0x18]->flags |= 1; push N->[0x18] to gray; }
    S = N->[0xc];                       // scheduler/thread state
    for each frame F of S->[8] list:
        for each Value v in container(F + 0x28): mark_value(v);   // 0x55a5b0
        for each Value v in container(F + 0x18): mark_value(v);
```
Which container is locals and which is the operand stack was not established; both
are marked, so the distinction does not matter for the port.

### 0x55a2d0 / 0x55a310 class statics
```
DAT_007a2624 = gc;
for bucket in the 512-entry class registry (gc->[0x8], stride 0xc):
    for each loaded class in bucket:
        wrap = class->[0x1d0]; if (!wrap) continue;
        container = container_of(wrap);                  // 0x548d60/0x548d70: cap@0,data@4,size@8
        for i = size-1 .. 0: mark_value(container->data[i]);
```

### 0x55a430 process gray (≤ 250 items)
```
for k = 0; k < 0xfa; k++:
    if (gray empty) break;
    o = pop(gray);                                   // data[--size]
    (exe prints "T_Container: underflow" on an empty pop)
    for each instance-field Value v of o (container at o+0x10): mark_value(v);
if (gray empty) {
    gc->[0x28] = gray->cap << 2;                     // last gray bytes
    free(gray->data); free(gray);
    gc->[0x20] = 0;
}
```

### 0x55a5b0 mark_value(Value *v)
```
if (!v) return;
switch (v->[4][0]) {                                 // type signature first char
'L':
    o = v->[8]; if (!o) return;
    if (o->[0xc]->[8][0] != '[') {                   // plain object
        if (o->[0x18] & 9) return;                   // marked or condemned: no resurrect
        o->[0x18] |= 1;
        push o to gray;                              // objects are scanned later
        return;
    }
    /* array object: fall through */
'[':
    o = v->[8]; if (!o) return;
    if (o->[0x18] & 9) return;
    o->[0x18] |= 1;
    if (o->[0xc]->[8][1] == 'L' || o->[0xc]->[8][1] == '[') {   // reference elements
        len   = ((Value *)DAT_007a261c)->[8];
        elems = ((Value *)DAT_007a2618)->[8];        // T_Container*
        for i = len-1 .. 0:
            mark_value(i < elems->cap ? elems->data[i] : 0);
    }
    return;                                          // arrays are never pushed
}
```

### 0x55a560 sweep
```
n = 0;
for each object o on the all-objects list (gc->[0x10]):
    if (!(o->flags & 1)) { FUN_0054c830(o); n++; }   // unlink, queue, flag |= 8
    else o->flags &= ~1;
return n;
```

### 0x559e30 System.gc (native 0x4e55b0)
Native `0x4e55b0` (14 bytes): `MOV ECX,[EBP+8]; MOV ECX,[ECX]; JMP 0x559e30`
(the native argument points at a context whose first dword is the GC).
```
if (gc->[0x20] == 0) { 0x559f80; 0x55a020; 0x55a170; 0x55a2d0; return -1; }
0x55a430;
if (gc->[0x20] == 0) { n = 0x55a560; gc->[0x34] -= gc->[0x38]; return n; }
return -1;
```
So every System.gc() call that *starts* a collection returns -1; only a call that
drains the last gray object and sweeps returns the sweep count.

### 0x54c830 (sweep helper) / 0x54c660 (finalize one)
```
0x54c830(o):
    unlink o from its current list; flags |= 8; clear links;
    append o to list *(*(type(o)+0xc) + 0x44);        // JavaMachine pending-finalize
0x54c660(o):
    if (flags & 2) return;                            // already finalized
    flags |= 2;
    create engine thread (0x5612f0, prio 10, name "THRD-FIN"); 0x562b00(0);
    invoke "finalize" on o via FUN_00561700(type(o), o, "finalize");   // result ignored
    0x562d70(0); 0x562a30; vfunc(1); 0x5648f0;
    if (type is array) free the element container;
    else release the 'L' values of the class's field containers;
    unlink o; append o to list *(*(type(o)+0xc) + 0x30);            // recycle list
```

### 0x54c5a0 disableGC / 0x54c600 enableGC (natives 0x4dfb60 / 0x4dfb80)
```
disable: if (!(flags & 4)) { unlink o; append to *(owner + 0x14); flags |= 4; }
enable:  if (flags & 4)  { unlink o; append to *(owner + 0x10); flags &= ~4; }
```
The sweep list is owner+0x10, so a disabled object is simply never seen by the sweep.

## Field map (exe -> src)
| exe | meaning | src |
|---|---|---|
| gc+0x10 | all objects | `Vm::objects_` / `arrays_` |
| gc+0x20 | gray stack NULL-ness | `gc_active_` + `gc_gray_` |
| gc+0x24 | smoothed cost (no reader traced) | `gc_cost_` |
| gc+0x28 | last gray bytes (no reader traced) | `gc_last_gray_bytes_` |
| gc+0x2c/+0x30 | allocator + finalized recycle list | `gc_recycled_` (see INVENTED) |
| gc+0x34 / +0x38 | alloc_since / threshold | `gc_alloc_since_` / `gc_threshold_` |
| gc+0x3c | finalizer budget | `gc_finalize_budget_` |
| gc+0x40 | tick counter | `gc_ticks_` |
| gc+0x44 | pending-finalize queue | `gc_finalize_queue_` |
| obj+0x18 bit0/bit2/bit3 | marked / no-GC / condemned | `Object::marked`, `gc_disabled`, `condemned` |

## Changes

### 1. `src/script/vm.h`
- `Object`: add `bool marked = false; bool gc_disabled = false; bool condemned = false;
  uint64_t gc_epoch = 0;` (`// +0x18 flags` comment).
- `Array`: add `bool marked = false; uint64_t gc_epoch = 0;`.
- `Thread`: make the constructor and a destructor out-of-line so the Vm can track live
  threads: `explicit Thread(Vm *vm); ~Thread();` (keep them private; `friend class Vm`).
- `Vm`: public
  ```cpp
  using GcRoots = std::function<void(std::vector<Object *> &)>;
  void set_gc_roots(GcRoots f) { gc_roots_ = std::move(f); }   // host-held instances
  void gc_tick(const std::vector<Object *> &extra_roots);        // 0x55cf20
  int32_t gc_phase(const std::vector<Object *> &extra_roots);    // 0x559e30
  ```
  private helpers `gc_start`, `gc_mark_obj`, `gc_mark_value`, `gc_process_gray`,
  `gc_sweep`, `gc_pump_finalizers`, `gc_finalize_one`; state
  ```cpp
  std::set<Thread *> live_threads_;                 // all Threads, incl. Vm::invoke's
  GcRoots gc_roots_;
  std::vector<Object *> gc_gray_;
  std::deque<std::unique_ptr<Object>> gc_finalize_queue_, gc_recycled_;
  std::deque<std::unique_ptr<Array>> gc_recycle_arrays_;
  bool gc_active_ = false;
  int32_t gc_ticks_ = 0;
  int32_t gc_alloc_since_ = 0;
  int32_t gc_threshold_ = 4096;      // 0x38, INVENTED initial
  int32_t gc_finalize_budget_ = 4;   // 0x3c, INVENTED initial (IDIV divisor, must be != 0)
  int32_t gc_cost_ = 0;
  size_t gc_last_gray_bytes_ = 0;
  uint64_t gc_epoch_ = 0;
  ```
  and `friend class Thread;` (the destructor unregisters).
- Add `#include <deque>`.

### 2. `src/script/vm.cpp`
- `Thread::Thread(Vm *vm) : vm_(vm) { if (vm_) vm_->live_threads_.insert(this); }`
  and `Thread::~Thread() { if (vm_) vm_->live_threads_.erase(this); }`.
  In `Vm::~Vm`, before the members die: `for (Thread *t : live_threads_) t->vm_ = nullptr;`
  (Scheduler states outlive the Vm; without this their Thread destructors would touch a
  dead Vm).
- `new_object`: `obj->gc_epoch = gc_epoch_; gc_alloc_since_ += 1;` before `push_back`.
  `new_array`: same two lines. The +1 cost is `INVENTED:` (exe's +0x34 increment and
  unit were not traced).
- `gc_start(extra)` (0x559f80 + root marking):
  ```cpp
  ++gc_epoch_;
  gc_gray_.clear();
  gc_active_ = true;
  for (Object *o : extra) mark_obj(o);
  if (gc_roots_) { std::vector<Object *> host; gc_roots_(host); for (Object *o : host) mark_obj(o); }
  for (Thread *t : live_threads_) {
      for (auto &f : t->frames_) {
          if (f->self) mark_obj(f->self);
          for (auto &v : f->locals) gc_mark_value(v);
          for (auto &v : f->stack) gc_mark_value(v);
      }
      gc_mark_value(t->result_);
  }
  for (auto &s : states_) for (auto &v : s->statics) gc_mark_value(v);
  ```
  where `gc_mark_obj(o)` is `if (o && !o->marked && !o->condemned) { o->marked = true;
  gc_gray_.push_back(o); }`.
- `gc_mark_value(const Value &v)` (0x55a5b0):
  - `Value::Obj` -> `mark_obj(v.obj)` and return;
  - `Value::Array` -> `if (a->marked) return; a->marked = true;` then, when
    `a->elem` starts with `'L'` or `'['`, `gc_mark_value` every element; arrays are not
    pushed (exe behavior);
  - `Value::Ref` -> `mark_obj(v.ref.obj)` and, if `v.ref.arr`, the array path; this arm is
    `INVENTED:` (src-only representation — exe containers hold plain values).
  - other kinds: nothing.
- `gc_process_gray(max)` (0x55a430): pop back up to `max` (0xfa = 250) and
  `gc_mark_value` each `o->fields` entry; when the vector empties set
  `gc_last_gray_bytes_ = gc_gray_.capacity() * sizeof(Object *)` (`INVENTED:` exe
  `cap<<2`; no reader traced) and `gc_active_ = false`.
- `gc_sweep()` (0x55a560 + 0x54c830 + array counterpart):
  iterate `objects_` preserving order into a new vector:
  - `gc_disabled` or `gc_epoch == gc_epoch_` (allocated during this collection —
    `INVENTED:` exe's allocator/write-barrier flag was not traced): keep untouched;
  - `marked`: clear `marked`, keep;
  - else: `condemned = true`, move to the back of `gc_finalize_queue_`, `n++`.
  Then the same for `arrays_`, except an unmarked array moves to `gc_recycle_arrays_`
  (`n++`): the exe queues array objects on gc+0x44 as well and only frees their element
  container when the array reaches the finalizer, so a still-running object finalizer
  may read an array field; src keeps the array alive until the same splice point
  (`INVENTED:` the exact exe free point, but deferring cannot dangle). Return `n`.
- `gc_pump_finalizers()` (0x559e80) exactly as in the pseudocode above, using
  `gc_finalize_queue_.size()` for A and `gc_recycled_.size()` for B, `/ gc_finalize_budget_`,
  `>> 6`, clamp 4, cap 0x40. When the queue empties on the last budget item, clear
  `gc_recycled_` and `gc_recycle_arrays_` (the exe splices the +0x30 list back to the
  allocator; `INVENTED:` src frees the memory here instead of threading an allocator
  free list).
- `gc_finalize_one()` (0x54c660): pop the queue front keeping the `unique_ptr`;
  `find_method(o->cls, "finalize", "", 0, {}, &decl, &err)`, and if found
  `invoke(*decl->unit, *m, o, {}, &r, &err)` with the result/error ignored (exe ignores
  them too); then push the unique_ptr onto `gc_recycled_`. Running it synchronously via
  `Vm::invoke` replaces the exe's dedicated "THRD-FIN" engine thread — `INVENTED:`.
  No `finalize` method found (or a failing native) still recycles the object.
- `gc_tick(extra)` (0x55cf20): ++`gc_ticks_`; `gc_pump_finalizers()`; measure
  `std::chrono::steady_clock`; if `gc_alloc_since_ > gc_threshold_` call
  `gc_start(extra)` when `!gc_active_`, else `gc_process_gray(250)` and, when that
  emptied the gray stack, `gc_sweep()` and `gc_alloc_since_ -= gc_threshold_`; then
  `gc_cost_ = (int32_t)((double)gc_cost_ * 0.2 + ms * 800.0);` (`INVENTED:` exe clock
  0x569730 is QPC; no reader of +0x24 was traced).
- `gc_phase(extra)` (0x559e30): exactly the pseudocode, including `return -1` on both
  non-sweep paths and `gc_alloc_since_ -= gc_threshold_` after a sweep.

### 3. `src/script/natives.cpp`
- Add a file-local scheduler-root helper next to the Scheduler:
  ```cpp
  // 0x55a170: objects the scheduler holds outside the VM (thread objects, targets,
  // monitor waiters). The VM walks its own live Thread frames.
  void scheduler_roots(Vm &vm, std::vector<Object *> &out) {
      Scheduler &s = sched(vm);
      for (auto &t : s.threads) {
          if (t->script) out.push_back(t->script);
          if (t->target) out.push_back(t->target);
          if (t->wait_on) out.push_back(t->wait_on);
      }
      for (auto &kv : s.monitors) if (kv.first) out.push_back(kv.first);
  }
  ```
- `run_threads` (0x55caf0): after the `s.pumping` guard and before the thread loop,
  `std::vector<Object *> roots; scheduler_roots(vm, roots); vm.gc_tick(roots);` with a
  `// 0x55cf20: the GC tick precedes runThreads` comment.
- `system_gc` (0x4e55b0): build the same roots and return `vm.gc_phase(roots)` as Int.
- Replace `object_gc_toggle` with `object_disable_gc` / `object_enable_gc` setting
  `a[0]`'s object `gc_disabled` and registering them at the two rows
  (`java.lang.Object` disableGC/enableGC), with `// 0x54c5a0` / `// 0x54c600`.

### 4. `src/game/runtime.cpp`
In `Runtime::activate`, next to the resource resolver:
```cpp
// INVENTED: the exe's GC roots for engine-owned script instances were not traced
// (only the three global refs at 0x55a020); root every instance this runtime owns
// so the collector can never free a Handle/Resource::script.
vm_->set_gc_roots([this](std::vector<Object *> &out) {
    for (auto &kv : handles_) if (kv.first) out.push_back(kv.first);
    for (auto &r : instances_) if (r->script) out.push_back(r->script);
    for (auto &kv : records_) if (kv.second->script) out.push_back(kv.second->script);
});
```
Clear it in `deactivate` (`vm_->set_gc_roots(nullptr);`).

### 5. `src/app/boot.cpp`
Delete the now-wrong `// INVENTED: ... 0x55cf20's GC work is not ported` comment at
`frame()`; keep the `game+0x234 == set whenever a VM exists` note (reword to one line).

### 6. `src/tools/classdump.cpp`
`System.gc` (`:727-728`) now returns -1 on a fresh VM: a call with no collection in
progress only starts one (`0x559e30` returns 0xffffffff). Change the expectation to
-1 and name the address.

## UNRESOLVED (offsets kept raw where the role is unclear)
- Initial values of gc+0x38 (threshold) and gc+0x3c (finalizer budget): constructor not
  traced; +0x3c is an IDIV divisor, so it cannot be 0 (src: INVENTED 4 and 4096).
- Where gc+0x34 is incremented and by how much (bytes? per allocation?): no increment
  site was traced; src uses +1 per object/array (INVENTED).
- Exact role of gc+0x2c/+0x30 (recycle list and its `*(gc+0x2c)+0x18` splice target).
  The card models +0x44 as the pending-finalize queue, +0x30 as finalized objects, and
  frees the latter at the traced splice point.
- Identity of the three global roots `DAT_005fe0e4`, `DAT_005fde5c`, `DAT_005fe0d4`,
  and the semantics of `FUN_0051f4f0` / vfunc slots +0xc (instance) and +0x14 (init).
- Array length/element container constants `DAT_007a261c` / `DAT_007a2618`: the values
  are read as `Value`s whose `[8]` is the length and the `T_Container*`; the producer
  was not traced.
- Which frame container is at +0x18 and which at +0x28 (locals vs operand stack).
- Whether the exe has a write barrier for reference stores during a collection (not
  traced). src marks on allocation only; an object allocated during an active
  collection is skipped by that sweep (gc_epoch).
- Whether swept objects are ever truly freed: 0x54c830 only requeues; the +0x30 splice
  into the allocator's +0x18 list is not decoded. src frees at the splice point.
- Behaviour of no-GC (flag 4) objects at shutdown (no shutdown sweeper traced).
- `java.lang.String.finalize` is a native (0x4e4f70 in the native list) but src Strings
  are `std::string`, not Objects, so they never reach the finalizer.
- The "T_Container: underflow" print in 0x55a430 is not reproduced (std::vector).

## Files
- edit: src/script/vm.h, src/script/vm.cpp, src/script/natives.cpp
- edit (small): src/game/runtime.cpp, src/app/boot.cpp, src/tools/classdump.cpp
- read only: src/game/runtime.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
  (the classdump `System.gc` value is the exe's return, not a new test)
- card frontmatter: status implemented, branch name
