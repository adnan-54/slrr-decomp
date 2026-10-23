---
id: 108
status: todo
branch:
depends: [107]
---
# VM: typed default for uninitialised locals, without the GameInit runaway

## Goal
The exe's locals are typed Variables: `float y;` with no initializer reads as 0.0 (int 0,
object null), not as an untyped null. A one-hunk change in `Thread::step` (src/script/vm.cpp,
case for the type token, the `if (f.pending_decl >= 0)` line) implemented that:

```cpp
        if (f.pending_decl >= 0) {
            // Type of a local declaration: the exe's locals are typed Variables,
            // so `float y;` without initializer reads as 0.0, not null.
            if ((size_t)f.pending_decl < f.locals.size())
                f.locals[(size_t)f.pending_decl] = default_for_desc(desc);
            return true;
        }
```

With it, `vmboot --selfcheck` gets GameInit past Dialog line 225 (good) but then runs away:
memory grows until bad_alloc (an uncapped run froze the machine). That hunk is already
applied in the working tree of main (uncommitted). Find WHY it runs away and fix the root cause.

Likely suspects (check, do not assume): (a) the type token of a declaration WITH an
initializer is executed after the initializer value was stored, so the reset overwrites
it (e.g. a loop counter or `String s = ...` reset every pass -> endless loop/allocation);
(b) a declaration inside a loop body resets a variable that is declared once but read after
the loop; (c) pending_decl indexes the wrong slot (locals are stored reversed, see commit
9ea2301 "int->float parameter conversion used the wrong parameter index (locals are reversed)").
Compare the executed method with its `.java` source (shipped sources under
`bin/the game/` Scripts dirs, e.g. find with `find "bin/the game" -name Dialog.java`).

## How to run (MANDATORY caps; never run src/build/vmboot directly)
- `tools/vmboot-capped.sh --selfcheck` (1 GB virtual memory cap, 60 s timeout; env
  VMBOOT_TIMEOUT=<s> to change the timeout). bad_alloc / exit 124 = still running away.
- To find the loop, add a temporary env-gated trace in vm.cpp (e.g. `VM_TRACE=1` prints
  class.method + source line on each call/return, rate-limited) and run with
  `VM_TRACE=1 tools/vmboot-capped.sh --selfcheck 2>&1 | tail -200`. Remove the trace
  before committing, or keep it only if it is off by default and costs nothing.

## Files
- edit: src/script/vm.cpp (Thread::step type-token case; whatever records pending_decl)
- read only: src/script/vm.h, the relevant `.java` sources under bin/

## Done when
- `tools/vmboot-capped.sh --selfcheck` finishes (no bad_alloc, no timeout) and GameInit gets
  at least as far as before (past Dialog line 225); report the new stop point/error line.
- `make -C src clean check` green.
- In the report: the root cause in one paragraph, with the exact .java line(s) that looped.
- If the fix needs exe behaviour you cannot confirm from the code (e.g. when exactly the exe
  initialises a declared local), mark it `INVENTED:` and say so in the report.
