---
id: 057
status: draft
branch:
depends: []
---
# script VM: Object.toString / Object.errorVM (wait/notify documented, left stubs)

## Goal
Replace the `java.lang.Object.toString()Ljava.lang.String;` and
`java.lang.Object.errorVM(Ljava.lang.String;)V` registry stubs with ports of the exe
natives (0x4e03f0, 0x4dfba0). `toString` returns the class name and the handle as
`ClassName<hex handle>`;
`errorVM` writes the `JVM ERROR:: … with message: …` line to the engine log.

`Object.wait/notify/notifyAll` (0x4e0440/0x4dfc70/0x4dfc90) stay explicit stubs: the
exe needs a thread monitor/wait-list service that this VM does not have (see the last
section). Their exe return type is `V` (no value); do not register a no-op that would
silently drop the blocking semantics.

## Exe evidence (cleaned pseudocode, renamed fields)
`getArgs` = `Native__getArgs 0x4b1af0`; the receiver is output 0 (`a[0]` in our
native signature), declared parameters follow in order.

### 0x4e03f0 java.lang.Object.toString()Ljava.lang.String;
    receiver = getArgs(ctx, &receiver);          // a[0], an Instance*
    push(receiver);                              // deliberately left on the stack
    ECX = [receiver + 0xc];                      // instance -> class record
    name = ClassRecord__name(ECX);               // 0x549ff0: MOV EAX,[ECX+8]; MOV EAX,[EAX]; RET
                                                 // (no RET n, so the pushed receiver stays)
    sprintf(buf, "%s<%0x>", name, receiver);     // 0x528030 with fmt @0x5a54c4
                                                 // "%0x" consumes the leftover receiver arg
    return JVM__newString(ctx->vm, buf);         // 0x55b980, 256-byte stack buffer
The exe prints the class name stored in the instance's class record and the handle as
lowercase hex; `Object.hashCode` (0x4dfc50) returns that same instance payload.
Our model: `object_class_name(o)` = `class_state_unit(o->cls)->class_name(unit->self_class)`
(the existing helper at natives.cpp:167; `ClassState` is opaque in vm.h), and the
handle is `(uint32_t)(uintptr_t)o`, exactly the value `object_hashcode` returns.

### 0x4dfba0 java.lang.Object.errorVM(Ljava.lang.String;)V
    receiver = getArgs(ctx, &receiver, &message);   // a[0] Instance*, a[1] char* payload
    cls    = [receiver + 0xc];                      // instance -> class record
    jcls   = [cls + 4];                             // JavaClass: +0x24 pool, +0x3c class const
    name   = ConstantPool__getString(pool, ConstantPool__getClass(pool, [jcls+0x3c]));
    log_append(&DAT_007a2838, &DAT_005a5ad8, 0x100);   // see UNRESOLVED below
    log_append(&DAT_007a2838, "JVM ERROR:: ", 0x100);
    log_append(&DAT_007a2838, name, 0x100);
    log_append(&DAT_007a2838, " with message: ", 0x100);
    log_append(&DAT_007a2838, message, 0x100);
    log_flush(&DAT_007a2838);                        // 0x5695c0
    DAT_007a2838 = 0;
`name` is the receiver's own class name; `message` is the String payload. 0x5695c0 is
the same log sink as `System.log` (0x4e5700, ported as `system_log`). Return: `V`
(no value).

UNRESOLVED: `FUN_00569320(&DAT_007a2838,&DAT_005a5ad8,0x100);` — `0x5a5ad8` is not in
`analysis/strings.tsv`; it sits between `'deleteNative'` @0x5a5ac8 and
`'java.util.resource.ResourceRef'` @0x5a5ae0. Probably the empty-string buffer reset;
treat it as appending nothing (do not invent output).

### Monitor natives (trace only; do not implement)
The receiver is the monitor object. `ctx+4` is the current engine Thread
(notes/06: `ctx = {vm, thread, &args, nativeInfo}`).
    # 0x4e0440 Object.wait()V
    monitor = getArgs(ctx, &monitor);
    Monitor__wait(monitor, ctx->thread);        # 0x54d430(thiscall this=monitor, arg=thread)
    # 0x54d430
    Thread__wait(thread, monitor);              # ECX=thread, arg=monitor (0x564950)
    if (!monitor->waiters(+0x14)) monitor->waiters = T_Container{0,0,0};
    push thread into monitor->waiters;          # T_Container: [0]=cap,[1]=data,[2]=count
    # 0x564950 Thread__wait
    if (flags(+0x2c) & 0x80) error "Thread::wait: thread already died";
    if (flags & 0x10) error "Thread::wait: already waiting";
    unlink thread from run list (+4/+8); flags |= 0x10;
    relink thread into the VM's waiting list ([vm+0x1c]):
      thread+4 = [[[thread+0x10]+0x1c]+0x18]+4; thread+8 = ...;
    thread+0x34 = monitor;
    # 0x4dfc70 Object.notify()V -> Monitor__notify 0x54d320(ECX=monitor)
    # 0x4dfc90 Object.notifyAll()V -> Monitor__notifyAll 0x54d3a0(ECX=monitor)
    # both pop monitor->waiters (+0x14, count--, slot cleared) and call
    # Thread__notify(0x5629a0) on each non-null thread; notify stops after the
    # first notified thread, notifyAll drains the list. Null slots are skipped.
    # Thread__notify: errors on dead/not-waiting; flags &= ~0x10; if (!(flags&0x20))
    # relinks the thread into the run list ([vm+0x18]).
No monitor queue and no way to suspend the calling thread from a native exist in
`src/`: `NativeFn` gets only `(Vm&, Args, out, err)` and `pump_threads` can only run
whole 256-token slices. Keep the three registry rows as `nullptr, true` engine stubs.

## Files
- edit: src/script/natives.cpp — new `object_class_name(Object*)`, `object_tostring`,
  `object_error_vm` next to `object_hashcode` (java.lang.Object section); registry rows
  0x4e03f0 and 0x4dfba0 get the new functions, `false`.
- read only: src/script/vm.h (`Object`, `Value`), src/script/classfile.h
  (`ClassUnit::class_name`, `self_class`).

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- wait/notify/notifyAll remain stubs and the missing-service reason is in a comment
  next to their registry rows
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
