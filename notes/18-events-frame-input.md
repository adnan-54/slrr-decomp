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

## 4. Game deferred lists and script timers (Game object at 0x5fde90)

All lists are intrusive doubly linked lists with head/tail sentinel nodes (`vtbl, next, prev`) embedded in Game; the first node is `headSentinel.next`, the walk stops when `node.next == 0` (tail sentinel). Nodes derive from `ResourceRefDLNode` (ctor 0x47b090, 0x34 bytes): an embedded Handle at `+0x24 {+0x24 link, +0x28, +0x2c id, +0x30 Resource*}` bound to the queued resource through 0x40cac0.

| list | head sentinel / tail ptr | count | add | drained by |
|---|---|---|---|---|
| unload | +0xe8 / +0x114 | +0x220 | `Game__queueUnload` 0x47e340 (ResourceRef.unload on type-1 resources, 0x4ec800) | 0x47d070 |
| delete | +0x134 / +0x160 | +0x224 | `Game__queueDelete` 0x47b970 (0x4e9450: ResourceRef.destroy, Viewport.destroy); if the RI is a game instance (`+0x7c` = resource type == 1) it first fetches typed data 0x80000000 (result unused) | 0x47d070 |
| generic objects | +0x1cc / – | – | not traced | 0x47d070 (unlink + `vf0(1)`) |
| timers (`TimedEvent`, 0x48 bytes) | +0x180 / +0x1ac | +0x228 | `Handle__addTimer` 0x4e7a60 from `GameType.addTimer` 0x4da3c0 | `Game__runTimers` 0x47b700 |

**0x47d070 `Game__flushDeferred`** (runs after every physics sub-step in `Game__simStep` 0x47e050, and once when no sub-step ran; the main loop 0x47d760 also calls it every iteration outside the 15 ms gate, after 0x47ba10 and before the SFX listener/service): repeat until a full pass did nothing: (1) each unload node: if its handle id ≠ 0 and `R = handle.res`: if `R+0x7c == 1` dispatch `0x47c760(handle, 0, code 2, 0, 0)`, then `R->vf[+0x1c](1)` (unload); unlink the handle from R's ref list (`R+0x78`); count−−; delete node. (2) each delete node: same, plus `R->vf0(1)` (destroy R) after the unload call. (3) generic list: unlink, `vf0(1)`. After the loop, `Game+0x234 ≠ 0` → 0x559f60 (not traced). So destroy/unload are deferred to the end of the sub-step and announced by event code 2 (`EVENT_DELETE`) to game instances (type 1). Correction: `R+0x7c` is the resource **type** (it indexes the manager per-type lists `mgr + type*0xf0`, 0x518320/0x5182b0/0x518240), not a residency state; see §4.1.

**Timers**: `TimedEvent` (ctor 0x4e7530) `+0x34 due, +0x38 period (0), +0x3c flags, +0x40 param, +0x44 method (strdup)`. `addTimer(deadline, id)` = `addTimer(simTime + deadline, flags 0x80000080, param id, method 0)`; sim time is the float at 0x60251c (`*PTR_DAT_005dadf0`, `System.simTime` 0x4e5810). `removeAllTimers` 0x4da930 → 0x4e93c0 zeroes/unbinds the handle of every timer with the receiver's id (nodes stay). `Game__runTimers` 0x47b700 (main loop each control step, notes/00): next taken before dispatch; id 0 → delete; `due <= simTime` → `0x47c760(&handle, 0, flags & 0x0fffffff, param, method)` (0x80 = `GameRef.EVENT_TIME`), sign bit → delete (one-shot), else `due += period`. Card 073.

### 4.1 What the drain does to a resource (unload `vf[+0x1c]` / destroy `vf0`) [V]

Vtables (RTTI, `notes/classes.md`; slot 7 = `+0x1c` unload, slot 0 = scalar deleting dtor):

| class (type) | vftable | vf0 | vf7 unload |
|---|---|---|---|
| `ResourceInstance_Game` (1, `ins_game`) | 0x5a7d70 | 0x519500 | 0x51a1b0 |
| `ResourceInstance_Render` (3, `ins_render`) | 0x5a7ed0 | 0x51b8a0 → dtor 0x51b5a0 | 0x51c630 |
| `Resource_Viewport` (0x12, `res_viewport`) | 0x5a9240 | 0x526c50 → dtor 0x526bd0 | 0x526ef0 |
| base `ResourceInstanceNode` / `ResourceNode` | 0x5a8370 / 0x5a833c | 0x51eda0 / 0x51edd0 | – |

Field map of a game instance (asm 0x519500: its typed data `ResourceInstance_Game_Data` is **embedded at R+0x108**, so `typed+X` = `R+0x108+X`): `+0x128` handle (typed+0x20, not identified), `+0x140` handle → `+0x14c` GameType resource (typed+0x38/+0x44), `+0x150` malloc block (typed+0x48), `+0x154` native type instance (typed+0x4c), `+0x158` **script object** (typed+0x50 = src `Resource::script`), `+0x164` notification list (typed+0x5c = src `notifications`), `+0x30` child list (TreeNode, first child), `+0x80` id, `+0x84` flags, `+0x94` bit0 loaded.

**`ResourceNode__unloadCommon` 0x51f810** (first call of every unload): release the `+0x100/+0x104` dependency array (0x463380 each, free), `+0x9c = 0`, `vf[+0x18](0)`, `+0xa8 = frame`. Not modelled in src.

**Game unload 0x51a1b0(param)** (drain passes 1; the dtor passes 1):
1. 0x51f810. If `!(param & 1) && (flags+0x84 & 0x200)`: keep (re-list 0x518320), return 1 — never taken from the drain.
2. each child (list `+0x30`) with type 1: `child->vf[+0x1c](param | 2)`; if any returns ≠0 → return 1 (abort).
3. if loaded (`+0x94 & 1`):
   - GameType `T = R+0x14c`: make resident (`T->vf[+0x14](1.0)` if `T+0x7c != 1`), typed data `D` (0x51f4f0(0,0,0) ≥ 0, `T->vf[+0xc](1.0)`); if `D`:
     - `+0x154` native instance: `D+0xc->vf[+0x10](inst, param)`; returns 0 → return 1 (abort); else `+0x154 = 0`.
     - `+0x158` script object: if the VM global `DAT_005fe0c4 != 0` → `0x54c600(ECX = obj)`: if `obj+0x18 & 4`, move obj to the list at `obj+0xc → +0xc → +0x10 → +8` and clear bit 4 (unroot; no Java method, no finalize). `+0x158 = 0`.
     - `D+0xc == 0 && D+0x10 == 0` → log `"unload: Undefined GameType Destructor! %08X (%s)"` (R+0x148, T+0x90 name).
     - temp handle bound to R (0x40cac0), `0x477320(ECX = Game, &h)` = **empty function** (`RET`), handle dtor 0x488080. So **no event/handler call from unload itself**.
   - unlink handle `+0x140` (0x488080); delete every node of `+0x164` (notifications, `vf0(1)`); unlink handle `+0x128`.
   - each child (`+0x30`): delete (`vf0(1)`) if `(child.id>>16 == R.id>>16 || child.id>>16 == 0xffff)` and not `(child.id>>16 == 0xffff && child.flags & 0x200 && !(param & 1))` — with param 1: every same-package or runtime-created (0xffff) child is destroyed.
   - free `+0x11c`, `+0x13c`, `+0x124` (array of 0x20-byte entries; `entry+0x1c` → 0x4fb000 on `DAT_005dadf0` first), `+0x150`.
   - 0x51f790(0) (clear loaded bit, move to the manager's unloaded list), `DAT_00773ab4--`, clear flag 0x200.
4. return 0.

**Render unload 0x51c630**: 0x51f810; if loaded (and not the keep case above): `0x51f9b0(ECX = R+0x2ac, 0)` (unbind that handle), `0x51c6c0(ECX = R+0x108)` (render data release: delete lists `+0x2c`, `+0x1f4`, `+0xc4`; free the text string `+0x170` **only if** `+0x16c & 1`; free mesh/buffers `+0xc/+0x10/+0x14`, `+0x1c8`, `+0x1d4`, delete `+0x18`, `+0x1c`; `+0x16c &= ~0x4000`), `+0x98 = 0`, 0x51f790(0), `DAT_00773ad0--`. Children untouched.

**Viewport unload 0x526ef0**: if loaded: delete every node of the list at `R+0xd8` (`0x465320(n)` = `n->vf0(1)`), `+0x98 = 0`, 0x51f790(0). UNRESOLVED: node class of the `+0xd8` list (raw: `while ((iVar1 = *(int *)(in_ECX + 0xd8), *(int *)(iVar1 + 4) != 0 && (iVar1 != 0))) FUN_00465320(iVar1);`).

**Destroy chain** (vf0(1)): class dtor → (Game: unload 0x51a1b0(1) again, `ResourceInstance_Game_Data__dtor` 0x5193b0 on R+0x108 unlinks its two handles; Render: inline copy of 0x51c630 then render-data dtor 0x51b630; Viewport: 0x526ef0, delete list `+0xd8` of the data part) → `ResourceInstanceNode__dtor` 0x51ebe0 (release `+0x100` array, unlink handle `+0xf0`) → **`ResourceNode__dtor` 0x51eca0** (asm-checked):
1. `ResourceManager__freeId` 0x5173e0 (`ECX = DAT_005db008`, arg `R+0x80`): if `id & 0xffff0000 == 0xffff0000` push the low 16 bits on the free-id stack (`mgr+0x1600/+0x1604/+0x1608`) → runtime ids are **recycled**.
2. free `+0x88` (only for 0xffff ids) and `+0x90` (name).
3. **`ResourceNode__clearRefs` 0x51f970 (`ECX = R+0x74`)**: for every handle on the ref list `R+0x78`: `link = next = id = res = 0` (no unlink walk, the list is just dropped); then `R+0x74 = 0`. So every bound handle (script `ResourceRef`s, queue nodes, timers, notification watcher refs) reads **id 0 / res 0** afterwards. This is why the drain's own unlink after `vf0` is a no-op (its handle is already zeroed).
4. `TreeNode__dtor` 0x569890: `while (first child at +0x30) child->vf0(1)` — **all** children destroyed recursively (no event 2 for them, no package filter); `TList__dtor` 0x41fd90 on `+0x28`; `TListNode__dtor` 0x569b70 unlinks R from its four intrusive lists (`+4/+8` sibling list of the parent, `+0xc/+0x10` manager per-type list, `+0x14/+0x18`, `+0x1c/+0x20`).
5. Game: the 0x1bc block goes back to the pool `DAT_00773ac8+0x10` (no heap free); others `operator delete` 0x56a42f.

Queue node: `ResourceRefDLNode__vf0` 0x47b4b0 unlinks its handle from R (if still bound), `TListNode__dtor` (removes it from the Game list), frees. `ResourceRefDLNode__ctor` 0x47b090 binds via `ResRefLink__set` 0x40cac0, which copies **R's** id (`R+0x80`), not the caller's handle id.

### 4.2 Event code 2 [V]
`GameRef.EVENT_DELETE = 0x00000002` (`bin/2.2.1 sources/system/Scripts/util/resource/src/GameRef.java:22`). The drain dispatches it only when `R+0x7c == 1` (game instance), **before** the unload, through `Game__dispatchEvent` 0x47c760 (`target = node handle, sender 0, code 2, param 0, method 0`): delivered like any event — native type handler (`typed+0x4c`) first, script `handleEvent(GameRef obj, int 2, String null)` (0x47c960 default branch: param `char*` 0 → null) when `typed+0xa0 & 2` and `typed+0x50` script exists, then matching notifications forward it. No shipped 2.2.1 script references `EVENT_DELETE` (grep), so it only matters for mods/notification watchers.

### 4.3 Callers [V] (procedure_callers)
- `Game__queueUnload` 0x47e340 ← only `Handle__unload` 0x4ec800 ← only `ResourceRef.unload` 0x4e4500. 0x4ec800: `id != 0 && res != 0` → type 1 ? queue : `res->vf[+0x1c](1)` immediately.
- `Game__queueDelete` 0x47b970 ← `Handle__queueDelete` 0x4e9450 (`id != 0 && res != 0`), plus engine code: `type_car__vf14` 0x4a2e60, 0x4be600, 0x4becc0, `type_part__vf4` 0x4c3a50, `type_part__vf14` 0x4c4400, `GameRef__getPhysBody` 0x4eabc0, `ResourceInstance_Render__vf12` 0x51d550.
- `Handle__queueDelete` 0x4e9450 ← natives `ResourceRef.destroy` 0x4e32b0, `Viewport.destroy` 0x4e6c90, `Camera.destroy` 0x4d8fc0 (an extra 0x4e9450 call when the camera typed `+0x110` id is a runtime id; its ECX not traced), and 44 engine functions (native `type_*` handlers vf4/vf13/vf14 of BOT, building, button, camera, car, cursor, ground, object, part, ped, player, trigger, trlight; `Camera__chaseAndDebugHud` 0x47f980; 23 unnamed: 0x416400 0x417080 0x4177a0 0x41cd70 0x41e580 0x41ed70 0x47c030 0x485600 0x487740 0x4877a0 0x488ae0 0x4a0c10 0x4ad330 0x4ad770 0x4ae5c0 0x4b1d50 0x4bdb90 0x4c2b10 0x4c6140 0x4c72d0 0x4c75a0 0x4d1fe0 0x4d34b0 — none is in `analysis/natives.tsv`). UNRESOLVED: whether any native reaches those unnamed helpers.
So the only script natives that must go through the queues are `ResourceRef.unload` (type 1), `ResourceRef.destroy`, `Viewport.destroy` and `Camera.destroy` (src: engine stub). Card 074.

## 5. How far the vmboot flow gets (2026-10-24, after card 136)
vmboot 30000 frames: MainMenu -> NEW CAREER -> Garage -> used dealer -> buy -> Garage ->
CMD_TESTDRIVE (134) -> `EventList(MODE_FREERIDE)` (Garage.java:1598-1600). 0 failed threads,
but `final actualState` stays `java.game.EventList`. Cause is the **vmboot driver, not the VM**:
- `EventList implements GameState` has only `mode`/`eGen` + enter/exit (EventList.java:14-42);
  the race UI and `osdCommand(int)` live in `Generator extends Dialog` (EventList.java:44+,
  `eGen = new Generator(mode)` in enter). `CMD_RACE = 0x10E` is a Generator constant;
  its button `button_gorace.addItem("", CMD_RACE)` (EventList.java:345) is created while
  `osd.globalHandler = this` (Generator ctor), and Osd buttons get `globalHandler` as their
  target (2.2.1 Osd.java:85,1052). The R hotkey also targets `this` = Generator (:156).
- vmboot sends `osdCommand(0x10E)` to the EventList; `find_method` resolves it to
  `java.lang.Object.osdCommand(I)V` (empty body, shipped Object.class), so THRD-EVENT-RACE
  finishes `done` doing nothing. GameState (GameState.java) declares nothing. VM is right.
- Probe (local vmboot patch, reverted): sending 0x10E to `EventList.eGen` instead reaches
  `java.game.Track` at frame 20001, still 0 failed threads; Track.game = `java.game.Freeride`,
  Track.map = `GroundRef`, Track.parentState = the EventList, `java.game.Track<..>` thread
  waits in `GameType.run` line 157 (message loop), Navigator/Timer watcher threads sleep.
  No `native not implemented` on the way (Track.enter's natives all exist).
Next blocker after that (not a script failure, engine services missing): the car does not
move. `Game__physicsTick` 0x47e050 sub-steps `PhysicsWorld__step` 0x502060 (step size
`*(PTR_005dadf0+8)`, last split halved), each followed by `0x47b700` timers +
`0x47d070` deferred flush, then refreshes the per-camera follow snapshots (`DAT_005f1e44`
list, `GameRef__getPhysBody` 0x4eabc0 -> body+0xb4 frame `+0x20c*0xd4`). src
`natives::advance_sim` ports only the clock part; there is no process-wide physics world
and no native ground object (`type_ground__vf3` 0x4bade0, notes/19 §1/§4), so the player
car is never put into a world nor integrated. Card 137 = driver fix + hard check.

## 6. Next gameplay step after card 162: career race entry (2026-10-24)
Freeride Track loop needs nothing new: Freeride.java has no handleEvent; Track.handleEvent only
re-arms its 1 s timer and message boxes (Track.java:1773-1867); Track.animate runs only through an
animate hook (no shipped freeride path enables it). A real race starts from the Garage:
CMD_EVENTLIST 133 (Garage.java:1587) -> `player.car.isDriveable()` -> `EventList(MODE_AMATEUR)`
(careerEvents = EVENT_ROOT children, GameLogic.java:372) -> Generator CMD_RACE -> `Track(cEvent.careerGM)`.
First blocker: `isDriveable()` fails ("no engine installed", then "missing a battery") because src
never fills `chassis.attachedParts` (0x4ca390 add_part, notes/13 §15.2) and does not load
record-named classes on demand (0x55add0). Card 163. With both fixed (scratch), the first career
event loads `java.game.CircuitRacing` on a Track, 0 failed threads; car/physics checks of the
freeride harness do not apply there (different map/grid). NEXT after 163: vmboot career flow
(CMD_EVENTLIST + hint dialog + CMD_RACE) and what CircuitRacing needs per tick (countdown timers,
checkpoint Triggers, bots via createBotCars/Bot/Navigator).
