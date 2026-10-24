---
id: 121
status: todo
branch:
depends: []
---
# vmboot: install a synthetic input device and press Enter so the shipped main menu leaves the splash

## Goal
`tools/vmboot` boots the shipped game and runs `FrameLoop` 6000 simulated frames
(~100 s), but no input backend is ever installed (`input::install_backend` has no
caller), so the device table is empty: `Input.getDeviceName()` returns null,
`ControlSet.load` remaps every control to a negative device id, the MENUSET
mappings are never created, and `Input.checkHotkeys` always reads 0. The shipped
main menu therefore idles on the splash ("PRESS ENTER") forever.

Install a `SyntheticBackend` in vmboot (keyboard + mouse descriptors identical to
the DirectInput backend's) and drive the frame loop with it. At frame 3000 hold
Enter (DIK scan 0x1c = the ControlSet MENUSET bind for `AXIS_SELECT` = 34) for 60
frames, then release. The press makes the focused Osd's `AXIS_SELECT` virtual axis
reach 1.0 on the next 50 ms HotkeyWatcher tick, which queues
`{Osd, hotkey}` code 0x100000; script dispatch calls `Osd.handleEvent(Hotkey)` →
`hk.handler.osdCommand(34)` on the Gates, setting `showMenu = 1`, after which the
menu animation thread runs `showPrimaryVisuals()` and sets `showMenu = 2`.

Success is read from the shipped script state itself: before the press
`GameLogic.actualState.window.showMenu == 0`; after the run it is >= 2. Do not
weaken the check if it does not move — report the blocked link instead (see Done
when).

The injection hook already exists: `input::install_backend(Backend*)`
(src/input/device.cpp:374, mirrors the startup init 0x565fc0) plus
`input::SyntheticBackend` (src/input/backend.h:69). **No src/input file is
modified by this card**; the whole change is in src/tools/vmboot.cpp.

Shipped-script chain that must fire (game facts, not exe):
- `sl/Scripts/game/MainMenu.java:181` `osd.createHotkey(Input.AXIS_SELECT, VIRTUAL|HK_STATIC, Input.AXIS_SELECT, this)`;
  `:297-310` `osdCommand(AXIS_SELECT)` sets `showMenu = 1`, `modeInit = -1`;
  `:685-693` showMenu 1 -> `mmaThread.execute(1)` -> `:229-240` `showPrimaryVisuals()`
  sets `showMenu = 2`.
- `system/.../io/src/Hotkey.java:41-44` `activate()` -> `Input.createHotkey(key, flags, this, osd, osd, eventFilter)`.
- `system/.../render/src/Osd.java:1397-1411` menu keys; `:652-745` `handleEvent(Hotkey)`
  calls `hk.handler.osdCommand(hk.command)`.
- `system/.../io/src/ControlSet.java:311` `define(38, 2, AXIS_SELECT, KEYBOARD, RCDIK_ENTER=0x1c, 0,1,0,1,0)`
  (same values in the shipped `save/controls/active_control_set`).
- 2.2.1 `system/.../render/src/Frontend.java:112-125` HotkeyWatcher: every 50 ms
  `Frontend.inputQueue.lastElement()` then `Input.checkHotkeys(cursor.controller, focusedOsd)`
  (thread started at `:49-50`); CHECKPOINT.md records this thread alive and
  sleeping in the current boot.

## Exe evidence (cleaned pseudocode, renamed fields)
The exe controller block is src's `input::Controller`: `+0x0` mapping count,
`AxisRecord[76]` from `+0x4` (stride 0x30), `Mapping[152]` from `+0xe44`
(stride 0x20), next at `+0x2144`. src/input/controller.h reproduces it with the
C++ member offsets 4 less (its `axes_` starts where the exe block has `+0x4`).

### 0x4de750 Input.checkHotkeys (native; slot table 0x5fe478, 512 x 0x20)
```
void Input_checkHotkeys(GameRef inst) {
  GameRef *ref; uint infocus_osd_id;
  resolve_ref(inst, 0, &ref, &infocus_osd_id);          // 0x4b1af0
  Controller *c = Game_getInfo(ref, 0x1b, 0);           // 0x47c640: controller block
  int first = 0, count = 0;
  for (slot = 0x5fe478; slot < 0x602478; slot += 0x20) {
    if (slot->used == 0) continue;                       // +0x0 flags
    if (slot->osd != 0 && slot->osd != infocus_osd_id) continue;   // +0xc owner Osd
    float value = 0.0f;
    if (slot->flags & 1)        value = Controller_getAxisVal(c, slot->axis);  // +4, VIRTUAL
    else if (slot->flags & 2)   value = key_pressed(0, slot->axis);            // 0x544900: getAxis(0,axis)>0.1
    bool pressed = (double)value > 0.2;                  // double at 0x5a6850
    if (slot->latch == 0) { slot->latch = 1; }           // +0x1c: first eval only arms
    else if (slot->state != pressed) {                   // +0x10 state, +0x18 event mask
      uint mask = pressed ? slot->events & 1 : slot->events & 2;   // press / release
      if (mask) { if (count == 0) first = slot; count++; }
    }
    slot->state = pressed;
    Hotkey_setField(slot->hotkey /*+8*/, "state", pressed);   // 0x4ed490
  }
  if (count > 0)                                          // only the FIRST changed slot
    Game_queueEvent(first->handler /*+0x14*/, 0, 0x100000, first->hotkey /*+8*/, 0); // 0x47d550
}
```
The Gates hotkey is created before `super.show()` (which creates the Osd menu
keys), so it is the earlier slot and wins the press edge; the release edge goes
to the Osd's PRIVATEEVENT select slot (press|release).

### 0x565b60 Controller_getAxisVal (ECX = controller block, arg = vaxis)
```
if (C+v*0x30+0xc (stamp) != g_stamp) {
  sum = 0;
  for each of 152 mappings at C+0xe44, stride 0x20:
    if (m.vaxis (C+0xe44) != vaxis) continue;
    raw  = Device_getAxis(m.device /*C+0xe48*/, m.index /*C+0xe4c*/);  // 0x544830
    kind = Device_kind(m.device, m.index);        // 0x5447e0
    if (kind != C+v*0x30+8 (record kind)) {       // 10% takeover rule
      if (fabs(m.in_max - m.in_min) * 0.1 < fabs(raw - m.last_raw)) {
        record.kind = kind; m.last_raw (C+0xe50) = raw;
      }
      if (kind != record.kind) continue;          // this mapping contributes nothing
    }
    type = Device_type(m.device);                 // 0x5448d0
    if (m.in_min <= raw && raw <= m.in_max)       // C+0xe54 in_min, C+0xe58 in_max
      sum += (m.out_max - m.out_min) * (raw - m.in_min) / (m.in_max - m.in_min) + m.out_min;
    m.last_raw = raw;
  }
  sum = clamp_unit(sum);
  if (record.kind == 0) {                          // analog filtering
    rate = (center_range /*C+0x10*/ <= sum || sum <= -center_range)
             ? (0 <= value*sum ? same /*C+0x1c*/ : opposite /*C+0x18*/)
             : center_factor /*C+0x14*/;
    if (force_f /*C+0x2c*/ != 0) rate *= 0.5;
    step = axis_scale /*0x7a5b5c*/ * g_frame_ms /*0x7a5b54*/ * speed /*C+0x20*/ * rate;
    step = min(step, 1.0); delta = sum - value; if (step < |delta|) delta *= step/|delta|;
    if (smooth /*C+0x30*/ <= 0) value = old + delta;
    else { f = min(smooth * 0.01 * g_frame_ms * axis_scale, 1.0); value = old + delta*(1-f); }
  } else if (type == 2) value = sum;                // mouse: pass through
  else { ... 0.02 step / 0.3 lerp filter ... }
  value = clamp_unit(value); stamp = g_stamp;
}
... power curve (C+0x24; sqrt/0.25 steps), already in src ...
```
For a keyboard mapping `kind == 0`, `in 0..1 -> out 0..1`, speed 1, smooth 0,
force 0: pressed raw 1.0 gives sum 1.0, `step` clamps to 1.0, so value reaches
1.0 on the first evaluation — > 0.2.

### 0x544830 Device_getAxis (device table 0x78d0f0, record stride 0x7a8)
```
float Device_getAxis(int device, int index) {
  int rec = 0x78d0f0 + device*0x7a8;
  int type = *(int*)(rec + 0x30);
  if (type == 1) {                                  // keyboard
    if (index < 0 || *(int*)(rec + 0x38) <= index) return 0.0f;
    if (-1 < (signed char)*(rec + 0x644 + (index & 0xff))) return 0.0f;
    return 1.0f;                                    // raw key byte high bit set
  }
  if (type == 2) return Mouse_getAxis(...);         // 0x543f70
  if (type == 3) return Joystick_getAxis(...);      // 0x543a90
  return 0.0f;
}
```
Keyboard records force `control_count = 0x100` (src/input/device.cpp:156-157);
the polled raw state is at record+0x644 (src `RawState.keys[256]`).

### 0x566160 input control step (called from FrameLoop every >= 16 ms)
```
g_frame_ms (0x7a5b54) = dt * 1000.0;
poll_ms (0x7a5b58)    = Device_poll();     // 0x544ba0: refresh every device raw state
g_stamp (0x7a5b60) += 1;
for (Controller *c = controllers (0x7a5b68); c; c = c->next (+0x2144)) {
  tick over 76 kind-0 axis records; force-feedback pass over 152 mappings;
}
```
`input::update` already does this (src/input/controller.cpp:262-274) and polls
`backend()` only when non-null.

## Why nothing fires today (src state, not exe)
- `grep -rn install_backend src/` -> only the declaration/definition; the
  DirectInput backend is created by `make_platform_backend`, which has no caller
  in the headless tools.
- `devices().count_ == 0` -> `Input.getDeviceName(i)` returns null for i=0 ->
  `ControlSet`'s nDevices is 0; the saved names "SysKeyboard"/"SysMouse" are
  appended to its own list with remapID -1/-2, so every control gets
  `deviceID < 0`; `Controller::add` rejects `device < 0` (controller.cpp:62) and
  no MENUSET mapping exists.
- `Controller::get_axis_val` therefore sums nothing and the hotkey reads 0.

## Implementation (one file: src/tools/vmboot.cpp)
1. Includes: `#include "input/backend.h"` and `#include "input/device.h"`.
2. File-scope `input::SyntheticBackend *g_input = nullptr;` and
   `void install_synthetic_input();` that:
   - builds device 0: `desc.type = input::kKeyboard; desc.name = "SysKeyboard";`
     (exe startup 0x565fc0 + platform_backend.cpp:213-233),
   - builds device 1: `desc.type = input::kMouse; desc.name = "SysMouse";` with
     the same 10 controls as platform_backend.cpp:235-266
     (0x8002/0x8004/0x8008, 0x1000..0x1003, 2/4/8),
   - push_backs each as `input::SyntheticBackend::Device` (`desc`, default
     `raw`, `available = true`), calls `input::install_backend(g_input)` and keeps
     the pointer in `g_input`.
   `INVENTED:` the whole helper — the exe enumerates DirectInput at startup; the
   headless boot substitutes a state source (the object lives for the process).
   The names are not invented: the saved control file remaps by name against
   `Input.getDeviceName`, and platform_backend.cpp already uses exactly these two
   strings. Comment that.
3. Call `install_synthetic_input();` at the top of `selfcheck()` (after the chdir
   to the game dir, before `Boot boot;`), so `ControlSet.load` during the
   `Init(int)` constructor sees two devices.
4. In `game_init_check()` replace the single 6000-frame loop with:
```
const int kPress = 3000, kRelease = 3060;   // INVENTED: 1 s hold so the 50 ms watcher samples it
FrameLoop loop(0);
for (int i = 1; i < kPress; i++) loop.frame(i * 16);
int show_before = -1, seek_before = -1;
std::string perr;
bool probed = menu_probe(boot, &show_before, &seek_before, &perr);
c.check(probed && show_before == 0,
        "main menu waits for input before the press (showMenu == 0)" + ...);
for (int i = kPress; i <= kFrames; i++) {
    g_input->devices[0].raw.keys[0x1c] = i < kRelease ? 0x80 : 0;  // MENUSET AXIS_SELECT
    loop.frame(i * 16);
}
```
   keep the existing thread-report/failed print afterwards.
5. Probe helper (INVENTED instrumentation, reads shipped script state):
```
// GameLogic.actualState is the active MainMenu (GameLogic.java:164,896);
// MainMenu.window is the shown Gates (MainMenu.java:14-20); showMenu is 0
// while the splash waits and 2 after showPrimaryVisuals (MainMenu.java:69,238).
bool menu_probe(Boot &boot, int *show, int *seek, std::string *err);
```
   `get_static_field(*boot.vm(), "java.game.GameLogic", "actualState")`
   -> require `Value::Obj` -> `get_instance_field(obj, "window")` -> require
   `Value::Obj` -> read int fields `showMenu` and `messageSeek`.
6. After the loop check `show_after >= 2` (print the value; 2 = THOR path ran).
   If it is 1, the press reached `osdCommand` but the THOR/animation path did not
   finish; if 0, the press never reached the hotkey. In both cases leave the check
   failing and report the evidence (thread report, probe error, messageSeek) —
   do not relax the assertion or edit scripts.

## Files
- edit: `src/tools/vmboot.cpp` (`selfcheck`, `game_init_check`, new file-scope helper)
- read only: `src/input/backend.h` (SyntheticBackend/Device/RawState/DeviceDesc/kKeyboard/kMouse)
- read only: `src/input/platform_backend.cpp:213-266` (descriptor names and mouse control list to copy)
- read only: `src/input/device.h` (`backend()`, `install_backend`), `src/input/controller.h:25-50` (record/mapping offsets)

## Done when
- The installed devices, the press/release frames and both probed script states
  are printed; the run still ends with `failed: 0` and the existing thread report.
- The new check fails when the press lines are removed (backend installed, no
  key) and passes with them; that is the sanity check, not a second test.
- No src/input change: the injection uses the existing `install_backend` +
  `SyntheticBackend`. The exe addresses (0x4de750/0x565b60/0x544830/0x566160)
  are cited in the new vmboot comments; 0x4de750 and 0x565b60 are already
  implemented in natives.cpp:5260 and controller.cpp:132. All vmboot-only glue
  is `INVENTED:`.
- `make -C src clean check` green (vmboot runs capped by the Makefile:
  `ulimit -v 1500000 && timeout 300`); no new unit tests or hand-made expected
  values (`showMenu` 0/2 come from MainMenu.java).
- card frontmatter: status implemented, branch name.

## UNRESOLVED
- The 2.3.1 `system.rpk` HotkeyWatcher class was not decompiled here; the
  reference is the 2.2.1 `Frontend.java` and CHECKPOINT.md's report that the
  watcher thread exists and sleeps. If `showMenu` stays 0, print
  `natives::thread_report` first: if the watcher is absent or still sleeping
  before `Input.checkHotkeys`, that is the blocked link, not the injection.
- `Game_getInfo(ref, 0x1b)` (0x47c640) is a stub in src (returns 0); src's
  in_check_hotkeys maps the passed Controller GameRef through `controller_of`
  instead (src/script/natives.cpp:5264). Equivalent only while one Controller
  maps to one resource — true for this boot; note it, do not "fix" it here.
- The shipped `save/controls/active_control_set` was byte-checked in an earlier
  session for the group-2 vaxis 0x22 / device 0 / axis 0x1c row, but this card
  does not re-parse it; `ControlSet.defaults()` has the same bind. If the press
  registers nowhere, dump the file's group-2 row before changing the scan code.
- Frame 3000 assumes the splash has shown "PRESS ENTER" by then (48 s of
  simulated time). If the pre-press probe reports `messageSeek != 0`, move the
  press later; do not change the probe's expectation.
