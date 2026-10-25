---
name: feedback-generic-naming
description: Do not use slrr_/SLRR prefixes or suffixes in rebuilt source; keep engine code generic
metadata:
  type: feedback
---

In the rebuilt source (`src/`), avoid `slrr_` / `Slrr` / `SLRR` prefixes or suffixes on files, types and functions.

**Why:** most of the engine is Invictus Games' engine, reused in other games, so engine code should stay generic (game-specific code is the exception, e.g. vehicle/game logic).

**How to apply:** name engine pieces by what they are (`Quat`, `Mat34`, `quat_mul`, `engine/math.h`). Related: [[reverse-engineering-workflow]].
