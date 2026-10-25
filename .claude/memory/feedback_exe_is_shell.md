---
name: exe-is-shell-scripts-hold-game-facts
description: "The exe is a generic shell; car/game rules live in the .java scripts; maps are mods, never hardcode them"
metadata:
  node_type: memory
  type: feedback
  originSessionId: a4e760fe-f689-48e3-a3bd-ff9290332a02
  modified: 2026-10-24T17:11:02.833Z
---

The exe is a "shell": car/game-specific rules (e.g. "no engine installed", "missing a battery" in Vehicle.isDriveable) live in the game's .java/.class scripts, not in the exe. Port only the exe side (natives, command handlers like add_part -> Chassis.addpart, record loading); never re-implement script-level rules in src/.

No map is hardcoded in the exe. In 2.3.1 every map is a mod except city, test_track and the Nevada desert, and even those load dynamically like mod maps. Keep src/ engine code free of map names; maps come through the generic record/rpk/PHYS-blob paths. Only corpus checks (src/tools) may name shipped maps.

**Why:** the user corrected a summary that framed the car-driveable messages as exe behaviour and mentioned "something with the maps".
**How to apply:** when a card or finding mentions a game message or rule, check which .java defines it before writing a card; in cards, say "script does X, exe provides Y". Related: [[feedback-generic-naming]].
