---
id: 023
status: implemented
branch: card/023
depends: [020, 021]
---
# input: Input.createHotkey / deleteHotkey / checkHotkeys / flushHotkeys

## Goal
Make the four hotkey natives non-stubs on the global `input::hotkeys()` table
(card 020). `HotkeyTable` already mirrors the exe table, but it stores a C++
`HotkeyState*`; the exe stores the script `Hotkey` object and writes its int
field `"state"` through `FUN_004ed490(obj, "state", v)`. Change the slot to hold
an opaque script-object pointer and report state writes through a callback.

## Exe evidence (cleaned pseudocode, renamed fields)
Slot (stride 0x20, 512 slots at 0x5fe478): +0 flags, +4 axis, +8 hotkey obj,
+0xc osd, +0x10 state, +0x14 handler, +0x18 events, +0x1c latch.

### 0x4de880 Input.createHotkey(int axis, int flags, Hotkey hk, GameRef handler, Osd osd, int events)  (static)
    for (i = 0; i < 512; i++) if (slot[i].flags == 0) {
        slot[i].flags = flags; slot[i].axis = axis; slot[i].hotkey = hk;
        slot[i].handler = handler; slot[i].osd = osd; slot[i].state = 0;
        slot[i].events = events; slot[i].latch = 0;
        return;
    }
    // table full: silently ignored. No handler validation here.
= existing `HotkeyTable::create(axis, flags, hotkey, handler, osd, events)`.

### 0x4de920 Input.deleteHotkey(Hotkey hk)  (static)
    for (i = 0; i < 512; i++) if (slot[i].hotkey == hk) { slot[i].flags = 0; return; }
    // only flags is cleared; matching does not require flags != 0
= existing `HotkeyTable::remove`.

### 0x4de970 Input.flushHotkeys()  (static; raw bytes)
    for each slot: if (flags != 0) latch = 0;
= existing `HotkeyTable::flush`.

### 0x4de750 Input.checkHotkeys(GameRef inst, Osd infocus)  (static)
    C = Game.getInfo(inst, 0x1b, 0);         // controller block of inst (see card 022)
    count = 0; first = 0;
    for (i = 0; i < 512; i++) {
        s = &slot[i];
        if (s->flags == 0) continue;
        if (s->osd != NULL && s->osd != infocus) continue;
        v = 0.0f;
        if (s->flags & 1)      v = C->get_axis_val(s->axis);          // 0x565b60
        else if (s->flags & 2) v = (getAxis(0, s->axis) > 0.1f) ? 1 : 0;   // 0x544900
        pressed = (double)v > 0.2;            // constant at 0x5a6850
        if (s->latch == 0) s->latch = 1;
        else if (s->state != pressed && (s->events & (pressed ? 1 : 2))) {
            if (count == 0) first = i;
            count++;
        }
        s->state = pressed;
        FUN_004ed490(s->hotkey, "state", pressed);   // set script int field
    }
    if (count > 0)
        Game__queueEvent(slot[first].handler, 0, 0x100000, slot[first].hotkey, 0);   // 0x47d550
`HotkeyTable::check` matches this except the state write and the controller argument.

## Changes
1. src/input/hotkeys.h/.cpp:
   - remove `struct HotkeyState`; `HotkeySlot::hotkey` and `QueuedEvent::hotkey` become `void *`
     (script Hotkey object identity); `create`/`remove` take `void *hotkey`.
   - `check(Controller *controller, DeviceTable &devices, void *infocus_osd,
     void (*set_state)(void *hotkey, int state, void *ctx), void *ctx)`:
     `controller` may be null -> INVENTED: virtual-axis slots read 0.0 (exe would crash);
     after storing `s.state`, call `set_state(s.hotkey, s.state, ctx)` when `set_state`
     is non-null (replaces the old `s.hotkey->state = ...` line). Keep everything else.
2. src/script/natives.cpp — four natives (static, args start at a[0]) with `// 0xADDR Input.name`:
   - createHotkey: `hotkeys().create(axis, flags, obj(a[2]), obj(a[3]), obj(a[4]), events)` where
     `obj()` = `a[i].kind == Value::Obj ? a[i].obj : nullptr`.
   - deleteHotkey: `hotkeys().remove(obj(a[0]))`.
   - flushHotkeys: `hotkeys().flush()`.
   - checkHotkeys: `C = controller_of(value_resource(a[0]))` (card 021 helper);
     `hotkeys().check(C, devices(), obj(a[1]), cb, &vm)` where `cb` does
     `vm.set_instance_field((Object*)hk, "state", Value::make_int(state), &e)` when `hk` non-null
     (ignore a set error, like the exe's field setter).
   - Switch the four rows (`createHotkey`, `deleteHotkey`, `checkHotkeys`, `flushHotkeys`) to `<fn>, false`.
   - Event dispatch: `Game__queueEvent` has no counterpart yet (GameRef.queueEvent is a stub);
     the event stays in `hotkeys().queue`. Add `// shortcut: queued hotkey events are never
     dispatched or drained; upgrade when the Game event queue (0x47d550) exists.`

## Files
- edit: src/input/hotkeys.h, src/input/hotkeys.cpp, src/script/natives.cpp
- read only: src/input/controller.h, src/input/device.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green (also `make -C src -f Makefile.win` compiles if the mingw toolchain is present); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
