---
id: 191
status: todo
branch:
depends: [189, 190]
---
# input: native mouse cursor (type_cursor) commands, per-frame tick and EVENT_CURSOR events

## Goal
Port `native cursor` (frontend.rpk 0x2a "cursor", type record data `native cursor`;
GameRef.RID_CURSOR, created by MouseCursor in Input.java:404). The tick moves the cursor from
its controller, picks with card 190's `screen_pick`, and sends the EVENT_CURSOR (0x10000)
strings that Group.handleEvent (shipped render/Group) turns into `g.click()` -> osdCommand.
Then make vmboot's `menu_select` (MainMenu ghost button, CMD_MENU_SELECT 0xB007) a real
click instead of the INVENTED direct osdCommand. Background: notes/18 s7 (line 223).

## Exe evidence (cleaned)
### 0x4b7a10 type_cursor vf3 (create), state `C` (0x1bc typed)
```
C.event_mask |= 0x10 (0x4ebd90(C, 0x10, 0))        // commands reach vf14
C.pick_id_init(+0x4c) = -1; C.flags50 = 3           // bit0 pick enabled, bit1 ?, bit2 standby
C.pos(+0xa4,+0xa8,+0xac) = 0; button state +0xb0..+0xe8 = 0; picked point +0x164..+0x170 = 0
C.mode(+0x1a8) = 1; C.mode_ch(+0x1ac) = "" ; +0x190 = 3.0f
sens_x = sens_y = cfg "mouseSensitivity" * 1000      // DAT_005daa78 / DAT_005daa7c
```
Type object (0x4b9b80): drag threshold `type+0x20 = 0.02f`.
### 0x4b7d70 vf14 commands (code must be 0x10; first word of the string)
- `cursor <vp> <cam> <id>`: resource loads only (0x51fa40/0x51f9b0) -> nothing to model.
- `mode <int> <1 char> <id>`: +0x1a8 = int, +0x1ac = char; pointer shape only (render): store, no effect.
- `activate <a> <b>`: C.active(+0x10) |= 1. INVENTED: controller = resource id `a` (MouseCursor.enable
  sends "activate "+controller.id(); the exe's controller link C+0x1c is not traced).
- `deactivate`: C.active &= ~1.
- `enable`: C.flags50 |= 1. `disable`: C.flags50 &= ~5, then clear hover/button links.
  `standby`: C.flags50 = (C.flags50 & ~1) | 4, clear links.
- `move x,y` / `relmove dx,dy` (format "%*s %f,%f"): set / add pos, clamp each to [-1, 1].
- `sens`: re-read mouseSensitivity. `lock`/`unlock`/`setpos`: SysCursor only -> ignore.
- returns 1 (handled).
### 0x4b79e0 vf11 tick(dt) -> 0x4b8430 when C.active & 1
```
if !controller: return
dx = axis(0x43, 0x2a) * sens_x * dt; dy = axis(0x43, 0x2b) * sens_y * dt
moved = dx*dx + dy*dy > 1e-6; if moved && (flags50 & 4): flags50 |= 1
pos += (dx, dy); clamp pos.x, pos.y to [-1, 1]
if !(flags50 & 1): return
hit = screen_pick(pos, &far, &near)            // card 190 (t = 1 when nothing)
picked = near + (far - near) * hit.t           // C+0x164
if hover_ref != hit.ref or hover_body != hit.body:
    if hover_ref: msg = "16 %d %d %d %d" (cursor id, hover_ref id, hover_body id or 0, moved)
                  queue(hover_ref, msg); queue(cursor, msg)
    hover_body = hit.body; hover_ref = hit.ref
    if hover_ref: msg = "15 %d %d %d %d" (same fields, new values); queue(hover_ref); queue(cursor)
left = axis(0x19, 0x2d) * (1/127) > 0.2       // int info, value*0.007874016
if !left and L.down:                            // release
    L.down = 0; msg = "2 %d %d %d" (cursor, L.ref id, L.body id); queue(cursor); if L.ref: queue(L.ref)
    if !L.dragged and L.ref:
        queue(L.ref, "5 %d %d %.3f %.3f %.3f" (cursor, L.body id, L.local x y z))      // LCLICK
        queue(cursor, sender=L.ref, "5 %d %d %.3f %.3f %.3f %.3f %.3f %.3f"
              (cursor, L.ref id, L.local xyz, picked xyz))
    elif L.dragged:
        queue(cursor, "10 %d %d" (cursor, L.body id)); if L.ref: queue(L.ref, same)   // drag end
        if L.ref and hit: (drop) queue(hit.ref, "11 %d %d %d %d %d" (cursor, hit.body id, L.ref id, L.body id))
                          queue(cursor, sender=hit.ref, "11 %d %d %d %d %.3f %.3f %.3f %d"
                                (cursor, hit.ref id, L.ref id, picked xyz, L.body id))
    clear L.ref/L.body
elif left and !L.down:                          // press
    L.down = 1; L.dragged = 0; L.start = pos
    if hit: L.ref = hit.ref; L.body = hit.body; L.local = hit.point (body-local)
    queue(L.ref if any, "1 %d %d %d %.3f %.3f %.3f" (cursor, L.ref id, L.body id, L.local))
    queue(cursor, same)
elif left and !L.dragged and |pos - L.start|^2 >= 0.02^2:
    L.dragged = 1; msg = "9 %d %d" (cursor, L.body id); queue(cursor); if L.ref: queue(L.ref)
right button: axis(0x19, 0x2e), codes 3 down / 4 up / 6 click / 12 drag begin / 13 drag end /
14 drop, same shapes with its own link set (C+0xfc ref, C+0x114 body, C+0x128 local).
```
Ids: "ref id" = Resource id of hit.ref (Group), "body id" = id of hit.body (the PhysicsRef
instance) or 0. Group.java uses LDOWN token(3), LUP token(3), LCLICK token(2) = physics id.
Every queue is `Runtime::queue_event(target, sender_or_null, 0x10000, Value::make_str(msg))`.
The SysCursor (DAT_005e9398) path and the debug-text draw (DAT_005fe39c) are not ported.

### MouseCursor natives
`getPos` 0x4df270 / `getPickedPos` 0x4def10 read getInfo 2 (pos x,y; z = pick t) / 0x3b
(picked point) of the cursor: install an `input::CursorSource` (src/input/cursor.h) that
returns C.pos and C.picked.

## src hooks
- create: in the place card 147/174 hook native types (src/game/car.cpp, `has_native_ref(rt,
  type, "bot")` block ~line 1147) add a `"cursor"` branch: event_mask |= 0x10, create C.
  Put the cursor code in a new src/game/cursor.cpp (+ .h; add to src/Makefile like bot_avoid.cpp).
- commands: add `!game::cursor_command(...)` to the chain in Runtime::queue_event (runtime.cpp ~864).
- tick: call `game::cursor_tick(rt, dt)` once per frame from natives::advance_sim
  (natives.cpp ~7665). INVENTED placement: the exe ticks types from its type list.
- controller axes: read them like car.cpp does for a player controller (car.cpp ~2085-2100:
  `natives::controller_of(res)->get_axis_val(v)`; info 0x43 = the float, 0x19 = int of it).

## vmboot (src/tools/vmboot.cpp, `menu_select` ~line 228)
INVENTED harness: find the ghost button's PhysicsRef instance (MainMenu.ghost_button gadget
`phy`), scan cursor positions on a 64x64 grid with `screen_pick` until `hit.body` is that
instance, send `move x,y` to the cursor, then press/release the mouse button for a few frames
through the synthetic input backend (src/input/backend.h RawState.mouse_buttons; add a mouse
device to vmboot's synthetic device list if none). Keep the existing `osd_command` path only
as a FAIL message if no grid point hits. Check (existing style): `c.check(clicked, "main menu
ghost button clicked through the native cursor")` and the flow continues as before.

## Files
- new: src/game/cursor.cpp, src/game/cursor.h; edit: src/Makefile, src/game/car.cpp (create
  branch only), src/game/runtime.cpp (command chain), src/script/natives.cpp (advance_sim call,
  CursorSource), src/tools/vmboot.cpp (menu_select)

## Done when
- `// 0xADDR` comments; INVENTED on controller link, tick placement, vmboot harness.
- `make -C src clean check` green (vmboot --selfcheck and --race unchanged otherwise).
- frontmatter: status implemented, branch card/191. If Flash runs out of turns, stop with
  status blocked and a note; the orchestrator finishes with opus.
