---
name: runtime-inputs-rpk-class-only
description: The game runs from .rpk + .class; .rdb is never read by the exe; .java is only a load-time fallback the exe compiles when a .class is missing/stale
metadata:
  type: project
---
A stock install needs only .rpk packages and compiled .class scripts. .rdb (text DB) is made from .rpk for modding and converted back; the exe never reads it. .java is source; the exe does not need it, BUT its record loader (0x51fcc0 -> 0x55aca0 -> 0x55bec0 -> JVM::compileSource 0x5598f0) compiles `<dir>\src\Name.java` when `<dir>\Name.class` is missing, has a different mtime than the source, or has a bad header (TUFA v4, build 0x11ae1/0x24f9d).

**Why:** user pointed out (2026-10-23) that src required .rdb; a later claim "the exe never compiles on demand" was wrong and had to be retracted.

**How to apply:** src runtime loads everything from .rpk and .class; .rdb/.java only as optional tool cross-checks. The compile fallback is a SERVICE MISSING item, not a reason to require .java. See [[no-self-written-tests]].
