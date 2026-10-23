---
id: 101
status: merged
branch: card/101
depends: [080]
---
# script VM: Thread.sleep / suspend / resume / stop / isAlive use the exe thread flags

## Goal
Port the thread-state natives to the exe's flag model (engine Thread +0x2c): sleep 0x08,
waiting 0x10, suspended 0x20, dead 0x80. Every flag in mask 0xfa stops the running thread at
the next instruction (Thread::run 0x563290, already modelled by `Thread::request_stop`, card
080). Remove two src inventions: `Thread.sleep` pumping other threads inline and advancing
the sim clock, and `Thread.isAlive` returning the real state (exe always returns 0).
Running threads from the frame loop is card 102; until then nothing pumps threads.

## Exe evidence (Claude, decompiled + bytes)
ctx = {+0 vm, +4 calling engine Thread, +8 &args, +0xc nativeInfo} (card 080);
src's calling thread = `Scheduler::current` (card 080). `0x4ecc70(obj, DAT_007a2620)` =
the script Thread object's engine Thread; src = `thread_state(vm, o)`.

### 0x4e6130 Thread.sleep(F)V   (bytes: 8b 4e 04 = mov ecx,[esi+4] -> CALLING thread)
    getArgs(ctx, &recv, &ms);  Thread__sleep(ECX = ctx+4, ms);          // 0x564860
### 0x564860 Thread__sleep(ms)
    now = 0x569730();          // QueryPerformanceCounter * 1000 / freq = milliseconds
    flags |= 0x08;  wake(+0x30) = (float)(now + ms);
### 0x4e6250 Thread.suspend()V -> if (h) 0x564890
    if (!(flags & 0x20)) { flags |= 0x20; if (!(flags & 0x10)) move thread to the VM wait list; }
### 0x4e6070 Thread.resume()V -> if (h) 0x562d10
    if (flags & 0x20) { flags &= ~0x20; if (!(flags & 0x10)) move thread to the VM run list; }
### 0x4e6200 Thread.stop()V
    if (h) { 0x5648f0(h); h+0x18 = 0; 0x4ed320(obj, field, 0) /* unbind handle */ }
### 0x5648f0 Thread__stop   (asm: mov ecx,[esi+0x34]; push esi; call 0x54d290)
    if (flags & 0x10) Instance__notifyThread(ECX = thread+0x34 /* wait object */, thread);
    flags |= 0x80;
### 0x54d290 Instance__notifyThread(ECX = monitor, thread)
    c = monitor+0x14 container;
    for (i = c->count; i != 0; ) { i--; if (c->data[i] == thread) { c->data[i] = 0;
        Thread__notify(thread) /* 0x5629a0: clears 0x10 */; return; } }
    log("Instance::notify(Thread*): inconsistency");
    (the slot is nulled, count unchanged; notify/notifyAll skip null slots, so erasing the
    element from src's vector is observably the same)
### 0x564950 Thread__wait (card 080) also stores thread+0x34 = monitor.
### 0x4e6040 Thread.isAlive()I  bytes: 55 8b ec 51 8d 45 fc 50 ff 75 08 e8 .. (getArgs)
    35 20 26 7a 00 / ff 75 fc / e8 .. (0x4ecc70) / 83 c4 10 / 33 c0 / 8b e5 5d c3
    => always returns 0.
### Scheduler wake test (0x55caf0, ported fully in card 102)
    if ((flags & 8) && wake(+0x30) < now) flags &= ~8;   // strict <
    if (!(flags & 8)) run the thread

## Changes (src/script/natives.cpp only)
1. `ScriptThreadState`: add `bool sleeping = false;  // +0x2c bit 0x08`,
   `double wake_ms = 0;  // +0x30`, `Object *wait_on = nullptr;  // +0x34 wait object`.
2. `Scheduler`: add `double now_ms = 0;  // 0x569730 ms clock; set by FrameLoop (card 102)`
   (INVENTED: src has no QPC clock here; the host passes its frame clock).
3. `object_wait`: also set `t->wait_on = o;`.
4. `thread_sleep` (0x4e6130/0x564860): `want`/`get_f` unchanged; `ScriptThreadState *t =
   s.current; if (!t) { null; return true; }` (shortcut, same reason as object_wait);
   `t->sleeping = true; t->wake_ms = s.now_ms + ms; t->runner->request_stop();`.
   Delete the `sim_seconds` advance and the `pump_threads` call (the sim clock is the
   physics world time, advanced by the sim step 0x47e050 -> card 102).
5. `thread_suspend` (0x564890): `if (t && !t->suspended) { t->suspended = true; if (t ==
   s.current) t->runner->request_stop(); }` (the 0xfa test stops it at the next instruction).
6. `thread_resume` (0x562d10): only clears `suspended` (unchanged behaviour; add the comment).
7. `thread_stop` (0x4e6200/0x5648f0/0x54d290): if `t->waiting`: find `t` in
   `s.monitors[t->wait_on]` searching from the back; if found erase that element and call
   `monitor_notify_thread(t)`, else `fprintf(stderr, "Instance::notify(Thread*):
   inconsistency\n")`; drop an emptied map entry. Then the existing `started = false; done =
   true; target = nullptr;`, and `if (t == s.current && t->runner) t->runner->request_stop();`.
8. `thread_is_alive`: return `Value::make_int(0)` always, comment `// 0x4e6040: xor eax,eax`.
   Remove the comment that justified reporting the real state.
9. `pump_threads`: before the skip test, `if (t->sleeping && t->wake_ms < s.now_ms)
   t->sleeping = false;` (0x55caf0 wake test) and add `|| t->sleeping` to the skip condition.
   It stays the pump until card 102 replaces it; nothing calls it after change 4.

## Files
- edit: src/script/natives.cpp (ScriptThreadState ~line 114, Scheduler ~127, pump_threads
  ~196, thread_* natives ~220-340, object_wait)

## Done when
- each change carries its `// 0xADDR` comment; the clock source is `INVENTED:`
- `make -C src clean check` green; no new tests
- card frontmatter: status implemented, branch name
