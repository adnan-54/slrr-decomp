---
name: cap-vmboot
description: Always run vmboot/script VM under a memory+time cap; an uncapped runaway froze the CT (4 GB RAM + 4 GB swap)
metadata:
  node_type: memory
  type: feedback
  originSessionId: 29694dfb-13ad-422c-a6cb-48468448eb18
  modified: 2026-10-23T21:28:55.274Z
---

Run `src/build/vmboot` (and anything that executes game scripts) only as `(ulimit -v 1500000; timeout 120 src/build/vmboot ...)`. Never let it drift into the background uncapped.

**Why:** 2026-10-23 a VM change made GameInit allocate without end; the uncapped `vmboot --selfcheck` outlived the tool timeout, ate all RAM+swap and froze the container. The user had to force-restart it.

**How to apply:** `make check` already caps vmboot (src/Makefile). Any manual run needs the cap too. On `bad_alloc` or a timeout, treat it as a VM loop bug and find it. Don't raise the limit.
