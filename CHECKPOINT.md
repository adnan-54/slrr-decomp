# CHECKPOINT — resume point for a new session or agent

Read this first, then `CLAUDE.md`, then the notes index below. Run `tools/checkpoint.sh` to verify the environment and print the live state.
Update this file (section "State") and commit whenever you stop. Last update: see `git log -1 -- CHECKPOINT.md`.

## Goal and decisions (from the user)
- End goal: a **fully functional, rebuildable source project of `StreetLegal_Redline.exe`** (SLRR 2.3.1, build 940, 32-bit PE, image base 0x400000).
- Target level **A: behaviour-equivalent rebuild** (not byte-matching).
- Rules: use the **rea MCP tools** (Ghidra provider) for analysis; write findings to `notes/`; **never commit anything in `bin/`** (`.gitignore` covers it); commit + push when asked/at milestones (trailer: `Co-Authored-By: Claude Sonnet 5.5 <noreply@anthropic.com>`).
- **Naming**: no `slrr_`/`Slrr`/`SLRR` prefix or suffix in `src/`. The engine is Invictus' and shared with other games; keep engine code generic (`Quat`, `Mat34`, `engine/math.h`). Only game-specific layers (vehicle/game logic) may be game-flavoured.
- Strategy: hybrid replacement (swap re-implemented subsystems into the original exe via ASI hooks and compare), 2,819 `.java/.class` pairs as VM conformance suite, `.rdb` rprop tables as struct layouts.

## Inputs (all under `bin/`, ignored by git — they must exist on the new machine)
- `bin/the game/` full install (exe: `bin/the game/StreetLegal_Redline.exe`, sha256 `f44fb88a…efc2`), `bin/tools/` community tools (SlrrLib, class decompiler), `bin/2.2.1 sources/system/Scripts/**` (readable `.java` for engine `java.*` packages), plus samples named in `CLAUDE.md`.
- Original exe path used by rea: **`/root/slrr-re/bin/the game/StreetLegal_Redline.exe`**.

## Environment (this container; recreate if missing)
- `.NET 10` SDK in `/opt/dotnet` (official dot.net install script; Debian apt has none). `tools/SlrrCli` builds against `bin/tools/SlrrLib/SlrrLib.Core.csproj` (headless net10.0 + `Compat/Media3D.cs` shim; both under `bin/`, see `notes/04`).
- Toolchain decided (`notes/15`): mingw-w64 i686 GCC 14.2 (`gcc-mingw-w64-i686`/`g++-mingw-w64-i686`) builds PE32 EXE/DLL; D3D9/DSound/DInput8 import libs verified; the install's `dinput8.dll` is Ultimate ASI Loader (ASI injection viable). No 32-bit Wine, so PE execution can't be tested here.
- `make -C src clean check` builds the engine library + tools and runs the real-data checks only (class parse/.java cross-check, game script runs, natives vs exe table, 2,907-file structural conformance, RPK/RDB, cfg, boot, SCX corpus). Self-written synthetic tests were removed on purpose; see the Tests rule in `CLAUDE.md`.
- Python 3 only (no pip). Scratch space: use the session scratchpad; large rea outputs are auto-saved to a JSON file — extract with python.
- **rea usage gotchas**: `open_binary` takes only `path` (no provider arg); `procedure_pseudo_code`/`procedure_callers` take `procedure` (address string); `batch_decompile` takes `addresses` (list) and returns huge JSON (`tools/decomp.py <saved.json>` prints it); `read_function_instructions` for tiny/undecompilable code. Ghidra renames are **session-only**: after (re)opening, names exist only if you re-apply `analysis/names.json` (`set_addresses_names`). Check with `procedure_pseudo_code` — names like `Thread__evalName_562280` mean they are applied.
- Shell gotcha: always quote heredocs (`<<'EOF'`) when text contains backticks.

## Repository map
| path | what |
|---|---|
| `notes/00-overview.md` | architecture, startup chain, main loop (some early guesses corrected in later notes) |
| `notes/01-rpk-rdb-format.md` | RPK/RDB formats (verified) |
| `notes/02-tooling-and-strategy.md` | tools review, RTTI, hybrid strategy |
| `notes/03-script-vm.md`, `05-thread-run-opcodes.md` | script VM (JVM) architecture, `Thread::run`, opcodes, Value model, operator semantics |
| `notes/04-slrrlib-headless.md` | SlrrLib CLI, the two "rdb" formats |
| `notes/06-native-methods.md` | native-method dispatch + all 422 registered natives, conventions, decompiled Math/System/PhysicsRef/GameRef |
| `notes/07-221-system-sources.md` | 2.2.1 `.java` system sources vs. native table |
| `notes/08-physics.md` | physics world step, bodies, RK4 integrator, forces, contacts, shapes, math helpers |
| `notes/09-vehicle.md` | vehicle params, wheel/Pacejka, `Vehicle::update`, drivetrain, raycast suspension |
| `notes/10-engine-dyno.md` | engine/dyno model |
| `notes/11-collision-shapes.md` | V-Clip feature kinds, dead constraint solver, trigger spring |
| `notes/12-render.md` | D3D9 pipeline, materials, shadows, flares, UI, palette, gamma |
| `notes/13-car-structs.md` | car/part/wheel/engine/SfxTable/DynoData layouts, all 114 car natives |
| `notes/14-audio-input.md` | DirectInput8 devices/hotkeys, DirectSound8 mixer/SFX |
| `notes/15-toolchain.md` | mingw-w64 i686 decision, PE/ASI proof, ABI risks |
| `notes/16-package-registry.md` | resource id scheme, classpath registry, gaps |
| `notes/17-scx-mesh.md` | SCX mesh format and loaders v1–v6, material grammar, collision/render binding, cross-check |
| `notes/18-events-frame-input.md` | event dispatch (0x47c760, immediate, no queue), notification list, main-loop input update 0x566160 |
| `notes/classes.md` | RTTI class table |
| `analysis/names.json` | ~1,817 address→name map (apply to Ghidra); `natives.tsv` (422 natives), `procs.tsv`, `strings.tsv`, `slrr.json` |
| `tools/` | `parse_rpk.py`, `rtti*.py`, `strxref.py`, `natives.py`, `native_notes.py`, `decomp.py`, `SlrrCli/` |
| `src/` | engine library + CLI tools: `script/` (TUFA parser, VM, natives), `resource/` (RPK/RDB, registry), `parts/` (cfg), `physics/`, `app/boot`, `engine/math.h`, `tools/`; build with `make -C src check` |

## State (update this on every checkpoint)
Done (RE): arch map; RPK/RDB formats + C++ refinements; RTTI map; car/part/wheel/engine/SfxTable/DynoData structs (notes/13); Vehicle::update incl. inline Pacejka, drivetrain split and raycast suspension/trigger spring (notes/09); collision shapes are V-Clip feature kinds and the constraint-list solver is dead in 2.3.1 (notes/11); render pipeline + materials/shadows/flares/palette/gamma (notes/12); DirectInput8 + DirectSound8 systems (notes/14); toolchain proof (notes/15).
Done (code, `src/`, `make -C src clean check` green): strict TUFA parser + full VM (branches/objects/arrays/invokes/scheduler; structural parse conformance 2,907 files/9,717 bodies/639k instr, 0 failures (not execution-verified)); RPK/RDB loaders; package+classpath registry (part 0xD2 → `java.game.cars.Axis_FL_door` + `FL_door.cfg`); cfg parser (3,189/3,194 shipped cfgs); physics core (RK4, manifolds, friction/restitution, direct resolve loop); boot path (`game.Init` static init → 100, `Vector3.length()` → 5); vehicle simulation + SCX mesh/collision loader; Win32/mingw PE32 DLL proof; runtime resource/instance model (GameRef/ResourceRef/GameType natives); car payload model (`src/game/car.*`) with Chassis 26/39, WheelRef 35/35, Part 25/29 and Vehicle.hasCrime implemented; render model/recording+D3D9 backends; input (DirectInput8 device/controller/hotkeys/cursor); audio (`src/audio/`: 64-voice pool, software mixer, Ds8 backend, SfxTable/engine SFX evaluator, music state machine). Native registry 422 (185 implemented, 237 explicit engine stubs).
In progress: none. Round 1 (cards 001-009, 020-026, 040-047) implemented by DeepSeek Flash headless (`claude -p --settings .claude/settings-deepseek.json`), merged, reviewed vs exe (Claude), all status merged. New in src: Sound music/volume/hw natives, src/vehicle/dyno.* engine model + DynoData natives, Input/Controller/hotkey/MouseCursor/FindFile natives, fs::find, GameType event mask/notifications, Runtime::queue_event dispatch, GameRef.queueEvent, input::update + FrameLoop, CarData engine curve, Chassis.forceUpdate, refreshParameters scalars.
Known open spots: physics: mode 6->7 conversion still after the loop (exe: inside, via 0x4ffd20 when 0x4fc610 fires), contact list rebuilt per sub-iteration (exe keeps creation order); audio: pan/doppler not ported (INVENTED), bus 2D flag and per-sound mindist/maxdist not wired; Chassis subsystem natives (mass/CM/OSD/SFX/force update) and Part getLogo/getMass/flap/install_OK stay explicit stubs; missing `.class` refs that exist only as `.java` (runtime source compilation); 237 native stubs; CMaterial partial classes/font atlas; VM `>>>`; engine/service startup (GameType, type registry, streaming); no way to run PE32 here (no wine32). Audio service-sort tie order is deterministic but not qsort-identical; DS8/DirectShow runtime untested (hooks only).

## Next-steps queue (in order)
0. Trace card 047 INVENTED-0 starts of heat_capacity car+0x1ebc / cooling_coeff car+0x1ec8. Byte search for disp 0x1ec8 hits: 0x48f020 (+0x3a), 0x4992e0 (+0xf5), 0x49ab20 (+0x661, manager default init), 0x49c8e0 (+0x77b, +0x9d4), 0x4aebf0 (refreshParameters). Start with 0x49c8e0/0x49ab20, then card it for DeepSeek. Headless DeepSeek runs: `tools/ds-loop.sh 048 049 ...` (one card after another, ff-merge, make check; stops at first failure). The rtk PreToolUse hook rewrites git/make to `rtk git`/`rtk make`; allow those in the runner's --allowedTools (agents otherwise work around the gate). Blocked/not carded: startup backend install (no WinMain host), force-feedback effect 0x5434e0, script timers (need sim step 0x47e050), C++ handlers on native objects (typed+0x4c), part-attach path 0x4a2e60->0x4942b0, CarData->vehicle sim bridge, cursor source from renderer. Unverified: default event mask (card 040 INVENTED 0), receiver-as-script-object (0x5240e0 not decompiled).
1. Remaining car natives once render/audio/physics services exist (Chassis mass/CM/OSD/SFX/force update, Part logo/flap/install_OK).
2. Render/audio/input native implementations on D3D9/DirectSound8/DirectInput8 (toolchain ready); ASI hook DLL first.
3. Hybrid validation against the original exe on Windows/wine32 (blocked here); then full exe build.
