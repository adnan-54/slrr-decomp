# SLRR 2.3.1 – high-level architecture overview

Target: `bin/StreetLegal_Redline.exe`, sha256 `f44fb88a4335c657e20701cdb84afb8d1d85f7c3aceb93d30113e6e6e44fefc2`
32-bit x86 PE, image base `0x400000`, MSVC (VS2015+ CRT, CFG enabled), 6630 procedures, 4749 defined strings.
Tooling: rea + Ghidra 12.1.4, snapshot `analysis/slrr.json`. All function names below are my labels (binary is stripped; Ghidra names are `FUN_xxxxxxxx`).
Confidence: **observed** = read from decompile/xrefs; **inferred** = guessed from strings/call shape, not verified.

## Sections
| name | range |
|---|---|
| .text | 0x401000–0x592600 |
| .rdata | 0x593000–0x5d9e00 |
| .data | 0x5da000–0x7ab00c |
| .tls | 0x7ad000 |
| .rsrc | 0x7af000–0x80b600 |
| .reloc | 0x80c000–0x81f600 |

## Startup chain (observed)
1. `entry` 0x56b21a → `__scrt_common_main_seh` 0x56b0b2 → WinMain-equivalent `FUN_00569690` (called with hInstance 0x400000).
2. `FUN_00569690` (WinMain): formats build string (build `0x3ac` = 940, "Dec 14 2025"), creates mutex `StreetLegalMutex` (single-instance check), `GlobalMemoryStatus`/`GetSystemInfo`, calls `FUN_004ed680`, then releases the mutex.
3. `FUN_004ed680` (game bootstrap, my name: `game_main`), in order:
   - logs `SL2 build %d ... distr. v2.3.1`; checks disk (`FUN_0041d780`).
   - `FUN_00517bd0` init resource/package manager; `FUN_00518940("system.rpk")` registers `system.rpk` (fails with "Data is missing. (distr. v2.3.1 is needed)").
   - `FUN_0047cd20` loads config; `FUN_0047c110(name)` = config int getter, `FUN_0047c1a0` = config string getter, `FUN_0047cff0` = config key-exists. Checks `acceptBuild` (≤0x3ac) and `version` == `v2.3.1`.
   - `FUN_00421280(video_x, video_y, video_depth, windowed, vsync, msaa, aniso)` – video/D3D9 init; reads `video_*`, `restart_video_*`, `shadows`, `resource_refreshrate`.
   - `FUN_00420350(...)` resource-manager/cache sizing (args 0x200, 0x10, refreshrate...).
   - `FUN_00565fc0`, `FUN_00546da0`, `FUN_00546ba0/be0/bf0` – input / sound init (`Sound_3D_HW`, `Sound_Mix_HW`).
   - **`FUN_0047d760` – main loop** (below).
   - Shutdown: `FUN_00526330` (writes `texture.log`), `FUN_0047c030`, `FUN_00518f60`, `FUN_0047b7b0`, `FUN_00546d00`, `FUN_005449f0` (releases sound/DirectSound objects), `FUN_00420b80` (destructor of the video/renderer-side lists and buffers), `FUN_00517110`.

## Main loop – `FUN_0047d760` (observed; this is a `__thiscall`, ECX = game object)
- Init: reads `fps_lock`, `res_lock`; runs `FUN_00523c80(..., "GameInit")` (calls into script side); timers via `FUN_00569730` (ms clock).
- Exit flag: `DAT_005f0c74` (set when `FUN_0056a1c0()` returns nonzero, i.e. window close/quit).
- Per frame:
  - dt in `DAT_005f0c88` (seconds = ms*0.001). Smoothed value in `DAT_005f0c80` (from `FUN_0056a260(7)`).
  - Fixed-step-ish sub-loop `FUN_004563d0` / `FUN_004562c0` / `FUN_004563c0`, run unbounded when `DAT_005da464 <= 0` else capped to `res_lock` iterations (**inferred**: event/resource queue drain).
  - Every >15 ms: `FUN_00566160(dt)`, `FUN_0047e050`, `FUN_0047b700`.
  - Every >7 ms: `FUN_0055cf20(10.0f)` if `[this+0x234]`.
  - `FUN_0047ba10(min(dt, DAT_005da468))` – main simulation update (clamped dt). Callees: `0x40cac0 0x47dd50 0x4e8860 0x4ebdd0 0x51f4f0` (**inferred**: script tick + world/physics update; the 4 indirect calls are unresolved).
  - `FUN_0047d070` → `0x47c760`, `0x559f60`.
  - Camera/listener setup from `DAT_005fe110..144` → `FUN_00568bb0` (set listener/camera), `FUN_005690a0`.
  - `FUN_00517f60` (resource streaming tick: calls `0x51e310 0x51e900 0x51f9b0 0x542c30`), `FUN_00518f80`.
  - If frontend present (`FUN_0055b100("java.render.Frontend")`, `FUN_00549750("render")`) → `FUN_0054d3a0` (frontend render).
  - Every `fps_lock` ms: `FUN_0044e980()` → `FUN_0047b5b0` or `FUN_0044ee30` (**inferred**: render/present).

## Subsystems
### Renderer (D3D9) – observed imports/strings, functions inferred
- Imports `d3d9.dll`, `d3dx9_32.dll` (D3DXMatrix*/Vec*, `D3DXCreateTextureFromFileExA`, `...FileInMemoryEx`, `D3DXAssembleShaderFromFileA`, `D3DXLoadSurface*`). String `IsD3DStateValid failed` @0x59487c.
- Init `FUN_00421280`; teardown `FUN_00420b80`. Resource types use `RESTYPE_*` names (see below). Config: `video_x/y/depth/windowed/vsync/msaa/aniso`, `shadows`.
### Physics
- Resource classes (RTTI @0x5e58a0–0x5e5a78): `ResourceInstance_Physics`, `ResourceType_Physics_Body`, `ResourceType_Physics_Constraint`, plus type names `RESTYPE_PHYSICS_BODY/CONSTRAINT/PARTICLE` (0x5a74fc..), `INSTANCE_PHYSICS` (0x5a7480). Engine appears to be an in-house/own rigid-body system (no third-party physics DLL in imports). Script bindings: `vm_physicsref_create / create_box / create_sphere / setmatrix / getpos / getori`, `vm_vehicle_getspeedsquare`. Simulation entry not yet pinned down (likely under `FUN_0047ba10`).
### Script VM
- Java-flavoured script language ("java.util.resource.PhysicsRef", "java.render.Frontend", `getScriptInstance`, `isScripted`). Script dirs (0x5b44e8–0x5b45f8): `system\scripts\{lang,net,io,util,render,sound}`, `sl\scripts\game`, `parts\scripts`, `parts\accessories\scripts`.
- Native bindings are named `vm_*` (strings 0x5a03dc–0x5a46f0): `vm_object_runmethod_{int,float,ref,void}`, `vm_resourceref_*`, `vm_renderref_*`, `vm_gameref_*` (create/setpos/setmatrix/addtimer/addnotif/queueevent…), `vm_physicsref_*`, `vm_sfxref_play/stop`, `vm_cam_*`, `vm_viewport_*`, `vm_mouse_getpickedpos`. Binding-registration code references them from around 0x4dbd00 (e.g. `vm_gameref_setpos` string is referenced from `FUN_004dbd00`, 0x4dbd00–0x4dbf14).
- Script errors: `!Script error\n\nThread: %s\nError:` referenced from `FUN_00559d20` (0x559d20–0x559e23) – error reporter; also "script error"/"fatal script error" strings @0x5b5c5c/0x5b5df0. `libcall.dll` string @0x5a64bc (native lib call from script, **inferred**).
- Not done: locating the bytecode interpreter loop/opcode dispatch.
### Resource loading (.rpk)
- Package files referenced by name: `system.rpk`, `frontend.rpk`, `cars.rpk`, `sl.rpk`, `particles.rpk`, `sound.rpk`, `decals.rpk`, `humans.rpk`, `part.rpk` (strings 0x599ce0–0x5a0130, 0x5a234c, 0x5a6ee0).
- Package registry: `FUN_00518940(name)` – looks up the package by name (via `FUN_0051e2f0`), appends if absent (`FUN_0051e2a0`), returns index; index 0xffff = failure. The manager object has the package count at `+0x166740` (so the manager is very large, ~1.4 MB, **inferred** static object). Companion functions `FUN_00517bd0` (init), `FUN_00517f60` (per-frame), `FUN_00517110` (shutdown), `FUN_00518f60/f80`.
- Resource types are config/`.cfg` driven: `Fatal! Cannot open .cfg file` @0x5a7230 (ref'd from `FUN_004f6430`), `Missing .cfg: %s` @0x5a83e0/0x5a8448.
- **`.rdb`: no string containing ".rdb" exists in the defined-string inventory.** Handling of `.rdb` is unverified; it may be built by concatenation or live in an undefined string. Open item.
### Audio / input / misc
- `DSOUND.dll`, `DINPUT8.dll` (DirectSound/DirectInput), `WINMM.dll`, `steam_api.dll`, `PSAPI.DLL`, `ole32/OLEAUT32`, `KERNEL32/USER32/GDI32`, `mscoree.dll` (CRT string only).
- Sound init `FUN_00546da0`, teardown `FUN_005449f0` (virtual-releases up to `DAT_0078d0ec` sound devices).

## Open items / next steps
1. Name and decompile `FUN_0047ba10` (sim update) and its 6 unresolved indirect calls – find the physics step and the script tick.
2. Find the script interpreter (xref `vm_*` table from `FUN_004dbd00`; find the registration table).
3. Locate the .rpk reader (`FUN_0051e2a0`/`FUN_0051e2f0`, file open helpers `FUN_005657b0/00565950/00564dd0`) and the on-disk header format; resolve `.rdb`.
4. Identify the window proc / message pump (`FUN_0056a1c0` is the quit-check candidate).
5. Full import table dump (only DLL names were taken from strings here).
