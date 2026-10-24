---
id: 118
status: todo
branch:
depends: [117]
---
# runtime: MouseCursor instance has no child nodes (cleanup -> ct.length on null)

## Goal
`tools/vmboot-capped.sh --selfcheck` (600 simulated frames after GameInit) reports
`java.io.MouseCursor.cleanup line 147: field access on non-object` in 2 threads.
cleanup does `ResourceRef[] ct = this.getChildNodes(); for (i = ct.length-1; ...)`
(2.2.1 equivalent: bin/2.2.1 sources/system/Scripts/io/src/Input.java:516).
Card 117 (tasks/117-*.md) shows the EXE would fault on a null `ct` (prints
"Thread::evalName: null.methodcall" then dereferences null), so in the real game the
cursor instance always has children. Do NOT change the VM's null handling.
Find why our MouseCursor's resource has no children and fix that.

## Where to look
- Class: `bin/the game/system/scripts/io/MouseCursor.class` (no .java ships; dump with
  `src/build/classdump`, constants show fields cursor (GameRef), vp, camera, pointers
  (ResourceRef), pointer (String), RID_CURSOR, WORLDTREEROOT, createNativeInstance,
  setFlags, destroy). Input.java (2.2.1) has the older code for the same class family.
- Which native creates the cursor and its render/pointer children (likely
  GameRef.create / RenderRef.create / createNativeInstance in src/script/natives.cpp),
  and whether those add the new instance to the parent's `children` (src/game/runtime.cpp
  `instantiate` does; check every create path that makes a render/native instance, e.g.
  ones that build a Runtime::Resource without `instantiate`).
- Also check which resource `this.getChildNodes()` resolves for a MouseCursor receiver
  (its handle binding): an unbound handle gives 0 children.
- Run ONLY capped: `tools/vmboot-capped.sh --selfcheck`. A temporary debug print is fine,
  remove it before committing.

## Files
- edit: src/script/natives.cpp and/or src/game/runtime.cpp (whatever the root cause needs)

## Done when
- MouseCursor.cleanup no longer fails; report the new
  `tools/vmboot-capped.sh --selfcheck 2>&1 | grep -E '^  (thread|frames)' | grep -v 'THRD-CREATE: done' | head`.
- `make -C src clean check` green. Root cause in one paragraph. Untraced logic `INVENTED:`.
