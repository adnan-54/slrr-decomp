# Game events, per-frame input tick, dyno → engine curve (2026-10, cards 040-047)

All [V] = read from the decompile of the cited address; [I] = inferred.

## 1. Event dispatch is synchronous: `Game__queueEvent` 0x47d550 → 0x47c760

`0x47d550` is a 9-byte jump stub to `0x47c760` (`push ebp; mov ebp,esp; pop ebp; jmp`).
Despite the name nothing is queued: the event is dispatched immediately. [V]

`0x47c760(target_node, sender_node, code, param, method)` (target = ref node
{prev,next,id,RI}):
1. Bind a local handle to `target->RI`; need `RI+0x7c == 1` (live), the streaming
   check `0x51f4f0(0xa0000000)` ≥ 0 and `typed = RI->vtbl[3](1.0f)`. [V]
2. If `typed+0xa0 (event mask) & code` and (no native handler `typed+0x4c`, or
   the native handler `vtbl+0x38(typed+0x4c, sender, code & 0x0fffffff, param)`
   returned 0) and `typed+0x50` (script instance) != 0 → `0x47c960` script
   dispatch. [V]
3. Always (mask or not) walk the notification list `typed+0x5c` (head first,
   sentinel `typed+0x58`): entry `n` (class `Notification`, 0x44 bytes, ctor
   0x4e7480): `+0x24` watcher ref node (`+0x2c` id, `+0x30` RI), `+0x34` mask,
   `+0x38` alias, `+0x3c` custom message (char*, not copied), `+0x40` custom
   method (copied). For each entry (next captured first):
   - `n.id == 0` (watcher gone) → delete entry, continue.
   - `!(code & n.mask)` → continue.
   - `c = (n.alias & 0x0fffffff) ? n.alias : code`; `p = (n.alias & 0x10000000) ? n.msg : param`.
   - sender given and `n.mask & 0x40000000` → skip the forward; else
     `0x47c760(&n.watcher, sender ? sender : target_handle, c & 0x0fffffff, p, n.method)`.
   - `n.mask & 0x80000000` (one-shot) → delete entry. [V]
4. Returns 1.

### Script side `0x47c960(target, sender, code, param, typed, method)` [V]
- `obj = sender ? sender : target`; `ev = code & 0x0fffffff`.
- `code == 0x100000` (EVENT_HOTKEY, full compare): `typed.script.handleEvent(param)`
  — one object argument (the `Hotkey`), via `0x47dd50(typed, "handleEvent", 0, 4, param, 0x4f)`; return.
- param by `ev`: 1 collision / 4 damage / 0x20,0x40 trigger build a string from the
  contact record in `param`; 0x80 (EVENT_TIME) passes `param` as **int**; default:
  `param` is a `char*` → script String (null pointer → null).
- `asyncMode` int field of the script != 0 → method `handleEventAsync`, else
  `handleEvent`; a notification's custom method overrides both.
- call `method(GameRef obj, int ev, String|int p)`.

### Method call `0x47dd70(typed, name, want_result, va_list)` [V]
Needs `typed+0x50` script and `typed+0x44` RI (loaded); creates a VM thread named
`"THRD-RUN VMI <id> <name>"` (`0x5612f0`), pushes args from a tagged va-list
(`0x47d250`: tag 2 int, 3 double→float, 4 object/string (0 → null), `0x4f` end),
`0x561700` resolves the method by name + pushed args (not found → `Thread::callMethod: "%s" not found`),
then `0x562d70(0.0)` = `Thread::run` with **no time slice** → runs to completion
(or until the script blocks). So handlers run synchronously, nested in the caller.

### Natives
| addr | native | body |
|---|---|---|
| 0x4db590 | `GameRef.queueEvent(ResourceRef ro, int type, String param)` | `queueEvent(handle(this), ro, type, param, 0)`; missing handle → log `vm_gameref_queueevent: scripted resource …` |
| 0x4dc780 / 0x4ebd90 | `GameType.setEventMask(m)` | `typed+0xa0 |= m` (null handle dereferences: crash) |
| 0x4dc4d0 / 0x4e8820 | `GameType.clearEventMask(m)` | `typed+0xa0 &= ~m` |
| 0x4da280 / 0x4da140 → 0x4e78e0 | `GameType.addNotification(GameRef ref, etype, ealias, msg[, method])` | `ref == null` → nothing; on **ref's** list (`ECX = ref`): remove the entry with watcher id == this id and mask == etype, then insert a new one at the head (watcher = this) |
| 0x4db6c0 → 0x4e92d0 | `GameType.remNotification(GameRef ref, etype)` | remove the first entry on ref's list with watcher == this and mask == etype |
| 0x4da3c0 | `GameType.addTimer(delay, id)` | `0x4e7a60(game_time + delay, 0x80000080, id, 0)`; fired by `0x47b700` (timer list `game+0x184`) through `0x47c760` — needs the game clock (`*PTR_005dadf0`, advanced in the sim step `0x47e050`) |

`GameType.createNativeInstance` 0x4dc510 passes the receiver script object to the
factory `0x5240e0`; [I] it becomes `typed+0x50`, i.e. the receiver is the object
that gets `handleEvent`.

## 2. Per-frame input tick — **correction to notes/14 §1.6**

notes/14 said the main loop has no input update. Wrong: `0x47d760` calls
`0x566160(dt)` every frame where `now − last_control > 15 ms`, with
`dt = (now − last_control)·0.001`, followed by `0x47e050` (sim step) and
`0x47b700` (timers); then `last_control = now`. [V]

`0x566160(dt)` = input/controller frame [V]:
```
frame_ms (DAT_007a5b54) = dt*1000
poll_ms  (DAT_007a5b58) = DeviceTable_poll()      // 0x544ba0: QPC interval, per-device GetDeviceState / mouse / joystick
stamp    (DAT_007a5b60)++
for C in controller list (head DAT_007a5b68, next C+0x2144; newest first):
    force pass over the 76 axis records (kind == 0), == Controller::tick
    for each of the 152 mappings m (C+0xe44, stride 0x20):
        if m.index < 1 && axes[m.vaxis].kind != 0 && device_type(m.device) == 3 (joystick):
            0x544b40(m.device, m.index, -axes[m.vaxis].force_c, axes[m.vaxis].force_f)
```
`0x544b40(dev, idx, a, b)`: dev out of range → -1; if FF enabled (`DAT_0078d0e0`)
`0x5434e0(&record[dev], b, 0, a, 0)` (DirectInput force-feedback effect; `idx` unused). Not decoded.
Controller registration `0x566010` (raw bytes, no Ghidra function): `C+0x2144 = head; head = C; count(DAT_007a5b64)++`,
called from the Controller type ctor (0x4d240c, 0x4d265c). There is no unregister.
Input init `0x565fc0` (startup, after video init): `0x544a60` (DirectInput init),
zero frame_ms/poll_ms/stamp/list/count, scale `DAT_007a5b5c = 1.0`.

## 3. Dyno tables → car engine curve (`Vehicle__refreshParameters` 0x4aebf0) [V]

- `e+0xc30` (dyno native) is written only by `Vehicle__onPartAttached 0x4942b0`
  (called from `0x4a2e60`, the car's part-attach handler): if the car has no part
  in slot 401 (`0x4c6fd0(car, 0x191)`), it reads the **car script's** `dynodata`
  field; if the attached part is the slot-401 part, it reads **that part's**
  `dynodata`. Either way it requires class `java.lang.Native` and stores
  `instanceToHandle(dynodata)`; also sets car flag `0x4000` unless `DAT_007a75df`.
- `refreshParameters(car)` (manager in ECX; called by `Chassis.forceUpdate 0x4b5890`
  and `0x494910`): clears car flag `0x800000`; clones the engine block if still the
  template; skips everything when the script int field `suspend_update` != 0;
  else zeroes wheel wear and recopies Pacejka, calls script `updatevariables()`
  synchronously (`0x47dd50(.., "updatevariables", 0x4f, 0x4f)`), reapplies wear
  records (`0x4a8060`), then reads `engine_inertia → e+0xe4`, `e+0xd4 = e+0xd8 = 1.0`,
  `starter_torque → e+0xd0`, `engine_friction_fwd/rev → e+0xf4/+0xf8`,
  `engine_rpm_idle·π/30 → e+0xf0`, `engine_mass·8000 → car+0x1ebc` (cooling
  `car+0x1ec8` rescaled by old/new), health `car+0x1e04/+0x1e08 = e+0xd4/+0xd8 · f(car+0x1e9c)`.
- Curve (dyno `d = e+0xc30`, read **after** `updatevariables`):
  `d == 0` or `d+0x1dc == 0` → both tables 1 point `(0,0)`, `e+0x14 = 0` (only when d == 0; else `e+0x14 = d+0x1c0`).
  Else `n = d+0x1d4`, `step = (float)d+0x1d0`; tables resized to n when n changed;
  `e[4][i] = (i·step, torque[i])`, `e[0xc][i] = (i·step, nitroTorque[i] or torque[i])`, `e+0x14 = d+0x1c0` (RPM_maxTorque).
- Then `ratio[]` (≤ 8) → `e+0x24..`, `gears → e+0x20`, `rearend_ratio`, `maxRPM`, `RPM_limit`, `ClutchF`, SFX ids,
  `rpm_trans_fwd/rev`, `sfx_starter_rpm`, `car+0x1e00 = 0`, `tank_nitro`, `consumption_nitro`
  (nitro clamped to tank), wheel slot rebinding, `drag_center` (Vector3 x/y/z) + `C_drag`,
  and always `Vehicle__applyTuningOverrides 0x498020`.
