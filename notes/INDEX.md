# Notes index

Read a note with `Read offset/limit` using the line numbers below; do not open the big ones whole. History of sessions: `LOG.md` (grep only).

| file | topic |
|---|---|
| 00-overview | architecture, startup chain, main loop (early guesses, corrected later) |
| 01-rpk-rdb-format | RPK/RDB formats (verified) |
| 02-tooling-and-strategy | tools review, RTTI, hybrid strategy |
| 03-script-vm, 05-thread-run-opcodes | script VM, `Thread::run`, opcodes, Value model |
| 04-slrrlib-headless | SlrrCli, the two "rdb" formats |
| 06-native-methods | native dispatch + all 422 natives, Math/System/PhysicsRef/GameRef |
| 07-221-system-sources | 2.2.1 `.java` system sources vs native table |
| 08-physics | world step, bodies, RK4, forces, contacts, pass order of `PhysicsWorld::step` |
| 10-engine-dyno | engine/dyno model |
| 15-toolchain | mingw-w64 i686 decision, ASI proof, ABI risks |
| 16-package-registry | resource ids, classpath registry |
| 17-scx-mesh | SCX mesh format, loaders v1-v6, materials |
| 18-events-frame-input | event dispatch 0x47c760, main-loop input 0x566160, s7 (line 223) mouse cursor events 0x4b8430 + screen pick 0x4eb000 + raycast 0x4fe1a0, s8 (line 265) cursor leftovers (dead RCLICK hand-off, write-only fields, Osd click targets), s9 (line 287) car/bot/part native dtors 0x49beb0/0x4805a0/0x4c3a50 + setParent 0x4ec210 + second-race probe |
| 20-script-compiler | .java -> TUFA compiler plan |
| 21-hud-osd | car HUD: gauge record G (M+0x1a44+i*0x48) defaults 0x4909b0/0x49ab20, osd_* keywords 0x5da7b8, Chassis.getOSD/setOSD 0x4b1fd0/0x4b4850, per-frame block of type_car__vf13 0x494910 + gauge_update 0x4b1d50 |
| classes | RTTI class table |

## Big notes: section -> start line
**09-vehicle** (98 KB): script-field accessors 3 · refresh params 0x4aebf0 17 · wheel record 41 · Vehicle::update 61 · car fields 195 · engine block 214 · suspension S 283 · Pacejka 319 · open questions 335 · drive path 343 · control path (player input) 389 · 103 m/s fall 487 · setMatrix placement 496 · part nocollision def|=8 573 · steer help 0x498300 580 · steer frame 0x4aa830 605
**12-render** (90 KB): globals 9 · video init 23 · frame sched 83 · viewports/cameras 111 · render groups 210 · transforms/fog/lights 295 · materials 307 · meshes 370 · textures 378 · script natives 387 · video 446 · open 449
**13-car-structs** (57 KB): native payload paths 14 · struct sizes 37 · car/part P 59 · manager M 114 · OSD 180 · wheel template 197 · wheel record 212 · slot record 320 · engine block 355 · SfxTable 372 · DynoData 386 · native summaries 420 · java confirmations 577 · open 597 · part install 617
**14-audio-input** (50 KB): input 19 · audio 326 · open 922
**19-ground-traffic** (64 KB): ground handle 10 · traffic 77 · pedestrians 161 · water 205 · no-service 226 · cards 246 · map collision 259 · race bots 446 · bot obstacles/solver 719 · bot steer rounding 758 · circuit race 774
**11-collision-shapes** (44 KB): shape 6 · V-Clip 44 · distance 65 · contact gen 100 · c_collision 108 · other 238 · Voronoi error 244 · script triggers 277 · hull contact refresh 334

## Where to grep (no semantic index; exact names win)
- exe facts: `analysis/names.json` (addr->name), `procs.tsv`, `strings.tsv`, `natives.tsv`
- engine `java.*` sources: `bin/2.2.1 sources/system/Scripts/**`; game scripts: `bin/the game/**/*.java`
- cards: `tasks/` (open) and `tasks/done/` (merged)
- tools: `tools/` (`checkpoint.sh`, `decomp.py`, `ds-*.sh`, `vmboot-capped.sh`)
