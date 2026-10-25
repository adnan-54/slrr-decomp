# 26 — Remaining explicit native stubs (2026-10-25)

Registry rows with `nullptr, true` in src/script/natives.cpp: **3** (notes/23's other 4 were done by
cards 203-206).

| native | exe | shipped callers (grep `bin/the game/**/*.java`, `.class`, `bin/2.2.1 sources`) |
|---|---|---|
| Chassis.setBuck (IIFFFF)I | 0x4b4410 | none: only the `native` declaration, parts/scripts/bodypart/Chassis.java:1160 |
| GameRef.setActiveCollision ()V | — | none (declaration only) |
| Object.runDLL | — | game/Init.java:164-168 (Steam workshop loop), source only |

runDLL is not reachable: the shipped `sl/Scripts/game/Init.class` has no `runDLL` reference (grep of
all shipped `.class`: 0 hits). The calls exist only in `Init.java`/`src/Init.java`, inside
`if (Steam.isReady())`, and `isReady` is defined nowhere in the install (no `.java`, no `.class`, not a
registered native: Steam natives are initAPI/neutralize/getPackage/setAchievement/resetAchievements).
So that source would not even compile against the shipped classes; it is a newer source than the
shipped class. Doubt (not traced): if the exe recompile rule (0x5598f0, mtime differs) ever picks
`src/Init.java`, the compile fails on `Steam.isReady`; check the fallback behaviour then.

**Result: no remaining explicit stub is reachable from shipped scripts. No cards written.**
Keep all three as stubs (setBuck/setActiveCollision: dead API; runDLL: Steam workshop, neutralized).

## Recommended next RE target
Traffic car pool (notes/19 §2, notes/25): Traffic__init 0x419da0 + 0x47d670 registration (shortcut in
src/game/runtime.cpp:435), addTrafficCar 0x4dd090, lane update 0x40ebb0 with the `car_rem` senders
0x40cd24/0x40f433/0x40f64c/0x40fa92 and 0x410680/0x40de70. It is the last script-visible gap found so
far (City.java:3082-3180 consumes car_add/car_rem; T+0x2054 is always 0 in src). Split into cards
of <= 5 functions: (a) pool create/registration, (b) addTrafficCar + getInfo 0x38/0x45, (c) lane
update + car_rem. HUD leftovers (notes/21) second.
