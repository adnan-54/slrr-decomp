---
id: 084
status: todo
branch:
depends: [083]
---
# app/audio: per-frame listener + voice service in FrameLoop; SfxItem owner = resource id

## Goal
1. Run the exe's per-frame audio step from `FrameLoop::frame`: `SfxPool__setListener`
   0x568bb0 then `SfxPool__service` 0x5690a0, every main-loop iteration (outside the 15 ms
   control gate), on the process-wide `audio::sfx_service()` from card 083. Without it, loop
   voices are never stopped and new events never start.
2. Fix the owner word of SfxTable entries and direct engine nodes: the exe stores the
   resource id there, src stores `resource + 0x80`.

Files (3): `src/app/boot.cpp`, `src/audio/sfx.cpp`, `src/audio/sfx.h` (comment only).

## Exe evidence (Claude-traced)

### Game__mainLoop 0x47d760, per iteration (after the control gate block)
    0x47ba10(min(dt, DAT_005da468)); Game__flushDeferred();          // 0x47d070
    pos = vel = 0; fwd = (0,0,1); up = (0,1,0);
    if (DAT_005f1e44 > 0) {   // camera present: basis from 0x566800(2|1), pos DAT_005fe130..138
        ... }
    SfxPool__setListener({pos, vel, -fwd, -up});   // 0x568bb0: copy to 0x7a7598.., 0x546cd0
    SfxPool__service();                              // 0x5690a0
    0x517f60(); 0x518f80(); ... render
src has no camera, which is the exe's `DAT_005f1e44 <= 0` branch: listener position (0,0,0).

### ResRefLink__set 0x40cac0 (binds an SfxTable entry / engine node)
    node+0xc = RI; node+0x8 = *(RI + 0x80);          // the RI id word
`SfxTable__evaluate` 0x4eb7a0 passes ECX = entry to `Game__postEvent`, which uses
`node+8` as the pool owner; the scan stops at the first entry whose `+8 == 0`
(`CMP [ESI-0xc],0` at 0x4eb81e). With no resource (`ResRefLink__set(0)`) the word is 0.

## Changes
1. `src/app/boot.cpp`: `#include "audio/sound.h"`; at the end of `FrameLoop::frame` (after
   the `if (now_ms - last_control_ms_ > 15) {...}` block, unconditional):
   ```cpp
   // 0x47d760: listener + voice service every frame. No camera in src = the exe's
   // DAT_005f1e44 <= 0 branch, listener at the origin.
   audio::SfxService &sfx = audio::sfx_service();
   const float listener[3] = {0.0f, 0.0f, 0.0f};
   for (int i = 0; i < 3; i++) sfx.pool.listener[i] = listener[i];   // 0x568bb0
   sfx.mixer.set_listener(listener);                                  // 0x546cd0
   sfx.pool.service(*sfx.backend);                                    // 0x5690a0
   ```
   (The exe runs this after `Game__flushDeferred`; src calls `flush_deferred` inside the
   gate today (shortcut, sim step not ported), so "end of frame" keeps the same relative
   order.)
2. `src/audio/sfx.cpp`: in `SfxTable::add_item` and `EngineSfx::emit_node` replace
   `resource + 0x80` with `resource` and cite `0x40cac0: node+8 = *(RI+0x80) (the id)`.
   In `src/audio/sfx.h` change the two comments that say `owner word = resource + 0x80` /
   `+0x08 = resource + 0x80` to `resource id (0x40cac0: *(RI+0x80))`.

## Check
`make -C src check` green. No synthetic test.

## UNRESOLVED
- Listener velocity/orientation (0x7a75a8..0x7a75d0) are not stored by `VoicePool`; only
  the position is used by src (pan/doppler are INVENTED-not-ported, notes/14 §2.2).
- The engine SFX consumer (`type_car__vf13` 0x494910) is still not wired to
  `audio::post_event`: `SfxItem::resource` holds `Runtime::Resource::id`, while the service
  (card 083) is keyed by resource node; a follow-up must map the entry to its node (the exe
  entry holds the RI pointer at +0xc).
