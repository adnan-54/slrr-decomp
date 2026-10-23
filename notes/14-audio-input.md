# 14 — Input and audio subsystems

Investigation of the engine input stack (`java.io.Input`, `java.io.Controller`,
`java.io.MouseCursor`, hotkeys) and the audio/SFX stack (`java.sound.Sound`,
`java.util.resource.SfxRef`, `java.game.parts.SfxTable`, Chassis/Part SFX
natives). All addresses are image-base 0x400000 VAs from the Ghidra session.

Confidence markers used below:
- **[V]** verified by decompiling the code or by reading the raw bytes.
- **[I]** inferred from code shape/callers; not fully proven.
- **[?]** unknown or unresolved; listed again in the last section.

The 2.2.1 script sources under `bin/2.2.1 sources/system/Scripts` are used only
for Java-side signatures; 2.3.1 added some natives (notably the music-track API)
that the 2.2.1 sources do not declare.

---

## Part 1 — Input

### 1.1 Backend and initialization

- Backend is **DirectInput8** (`DINPUT8.dll`), not raw Win32 messages. [V]
- `FUN_00544a60` calls `DirectInput8Create`. Device enumeration uses
  `IDirectInput8::EnumDevices(4 /*DI8DEVCLASS_ALL*/, callback, 0, 1
  /*DIEDFL_ATTACHEDONLY*/)`. [V]
- `FUN_00565fc0` is the input init called from process init `FUN_004ed680`.
  It creates the DirectInput object, enumerates and opens keyboard, mouse and
  joystick devices, and resets the axis-smoothing globals (mouse sensitivity
  `DAT_00794b74`, etc.). [V]
- `FUN_005449f0` is DirectInput shutdown, called at process exit.
  **Correction to notes/00:** this function was earlier labelled sound
  teardown. It is the DirectInput teardown; sound teardown is `FUN_00546d00`. [V]
- Device table: count in `DAT_0078d0ec`, maximum 16. Device record `i` is

      B_i = 0x0078d0f0 + i * 0x7a8

  The first record (0x0078d0f0) is normally the keyboard. [V]

### 1.2 Device record layout (B)

| off | type | meaning |
|---|---|---|
| 0x000 | `IDirectInputDevice8*` | COM device pointer |
| 0x030 | int | device type: 1 keyboard, 2 mouse, 3 joystick |
| 0x034 | int | axis/control count (entries used in the +0x144 table) |
| 0x038 | int | button count [I: populated during enumeration] |
| 0x044 | float[count] | per-control last value (mouse buttons, filtered state) |
| 0x144 | int[axis] | per-control input id table (see encoding below) |
| 0x244 | char[axis][0x10] | axis names (used for mouse/joystick) |
| 0x644 | union | raw input state (see below), size ~0x110 |
| 0x754 | char[0x40] | device name (`getDeviceName`) |

The +0x644 union is shared by the three device types: [V]

- Keyboard (type 1): 256 bytes of DirectInput keyboard state at B+0x644;
  byte index is the DIK scan code, bit 0x80 = pressed. `getAxis` returns 1.0
  when the byte has the high bit set, else 0.0.
- Joystick (type 3):
  - axes at B+0x644/648/64c/650/654/658/65c/660 for control ids
    2, 4, 8, 0x10, 0x20, 0x40, 0x80, 0x81 (DInput axis object ids);
  - POV hats at B+0x664/668/66c/670 for ids 0x2000..0x2003 and 0x3000..;
  - buttons as bytes at B+0x674 + (id - 0x1000).
- Mouse (type 2):
  - accumulated deltas at B+0x644/648/64c for control ids 0x8002/0x8004/0x8008;
  - button states come from the float array at B+0x44. The exact button id
    encoding is [?].

`FUN_00544830(device, id)` is the universal axis reader: [V]

| device | returned value |
|---|---|
| keyboard | 1.0 if raw state byte has 0x80, else 0 |
| joystick axis | `(0x8000 - raw) * 3.0517578e-05` = `(32768 - raw)/32768`, i.e. -1..1 |
| joystick slider/POV | `sin/cos((int_part + frac_part) * 0.00017452778)`, wrapped to -1..1; the constant is 2*pi/36000, so the raw value is an angle in 1/100 degree |
| mouse delta | `(delta / DAT_00794b74) * 0.005` |
| button | current float value from B+0x44 |

Helper functions used by the natives:

| func | behavior |
|---|---|
| `FUN_005447e0(device, id)` | kind tag: 1 = continuous axis, 0 = button/POV [V] |
| `FUN_005448a0(device)` | returns B+0x754 (device name) [V] |
| `FUN_005448d0(device)` | returns device type B+0x30 [V] |
| `FUN_00544750(device, axis)` | axis name; keyboard names come from the static table `PTR_DAT_005e0098`, other devices from B+0x244 [V] |
| `FUN_00544900(0, 0, id)` | key-down test wrapper, `FUN_00544830(0,id) > 0.1` [V] |
| `FUN_00544930()` | buffered keyboard read (see `lastKey`) [V] |
| `FUN_00544670(device)` | `activeAxis`: scans the control table and returns a control index when exactly one control is clearly active, else -1. Active means current value >= 0.25, or a downward zero crossing (current <= -0.25 / -0.75 relative to the previous float). [? exact comparison details] |
| `FUN_00543f70` | device state refresh / negative-id reset path; not decompiled in this pass [?] |

### 1.3 `java.io.Input` natives

Native table rows (2.3.1), with the 2.2.1 Java signature and the implementation:

| native | signature | implementation |
|---|---|---|
| 0x4de990 `getAxis` | `static float getAxis(int device, int axis)` | `FUN_00544830` [V] |
| 0x4de9c0 `getDeviceName` | `static String getDeviceName(int i)` | `FUN_005448a0(i)`, null if out of range [V/I] |
| 0x4de6d0 `activeAxis` | `static int activeAxis(int device)` | `FUN_00544670` [V] |
| 0x4de700 `axisName` | `static String axisName(int device, int axis)` | `FUN_00544750` [V] |
| 0x4dea20 `mapAxis` | `static void mapAxis(GameRef inst, int l_axis, int device, int i_axis, float i_from, float i_to, float l_from, float l_to)` | resolves the GameRef and forwards to the Controller mapping layer (`user_Add` equivalent); the GameRef info id used for resolution was not re-checked in this pass [?] |
| 0x4deab0 `setAxisSmooth` | `static void setAxisSmooth(GameRef inst, int l_axis, float a, float b, float c, float d)` | resolves the GameRef and forwards to `user_SetAxisSmooth` (the 4 Java floats plus the remaining fields filled from defaults) [?] |

`Input.MouseCursor` natives (class `io.MouseCursor` in 2.3.1; the 2.2.1
`Input.java` declares the same two methods at its end):

| native | signature | implementation |
|---|---|---|
| 0x4df270 `getPos()` | `Vector3 getPos()` | resolve `this` resource via `FUN_004ecfc0(obj, "cursor")`, then `FUN_0047c640(ref, 2, &block)`; builds a `java.lang.Vector3` from block x,y (z unused) [V] |
| 0x4df3b0 `getPos(Vector3 out)` | `Vector3 getPos(Vector3 out)` | same info id 2; writes x,y into the supplied Vector3, returns it [V] |
| 0x4def10 `getPickedPos()` | `Vector3 getPickedPos()` | same resolution, info id **0x3b**; builds a Vector3 from block x,y,z [V] |
| 0x4df100 `getPickedPos(Vector3 out)` | `Vector3 getPickedPos(Vector3 out)` | same info id 0x3b; writes x,y,z into the supplied Vector3, returns it [V] |

GameRef resolution failure prints `Mighty Mouse ERROR 2` (getPos) or
`vm_mouse_getpickedpos: scripted resource ... not found in file ...` followed by
the script file name and line number, and returns null/0. [V]

`FUN_0047c640(ref, infoId, out)` is the generic GameRef info-block query used
throughout the natives (`0x1b` = hotkey/controller context block, `2` =
cursor screen position, `0x3b` = picked world position). [V for these ids]

### 1.4 Hotkeys

The hotkey table is a static array at **0x005fe478**, 512 slots of 0x20 bytes
(end 0x00602478, which is immediately followed by music set 4). Layout: [V]

| off | meaning |
|---|---|
| 0x00 | flags/spec; bit0 = virtual axis, bit1 = key. A slot is used when this is nonzero. Comes from `createHotkey` arg 1 |
| 0x04 | axis/key id (arg 0) |
| 0x08 | `Hotkey` script object |
| 0x0c | `Osd` owner/filter |
| 0x10 | current state (updated through `FUN_004ed490`) |
| 0x14 | `GameRef` handler |
| 0x18 | event mask: bit0 press, bit1 release |
| 0x1c | first-evaluation latch, zeroed on create |

Natives:

- `createHotkey` 0x4de880 — arguments `(axis, flags, hk, handler, osd,
  eventFilter)`. Validates the handler GameRef through `FUN_0047c640(ref, 0x1b,
  0)` and fills a slot with the fields above. Slot replacement / duplicate
  handling was not re-checked [?]. Java `Input.createGlobalHotkey` is a pure
  Java wrapper: `createHotkey(axis, flags, h, handler, null, Event.F_KEY_PRESS)`
  — so global hotkeys are just slots with a null Osd filter and press-only mask.
- `deleteHotkey` 0x4de920 — finds the slot whose +0x08 equals the `Hotkey`
  object and clears it. [? exact cleared fields]
- `checkHotkeys(GameRef inst, Osd infocus)` 0x4de750 — validates the GameRef,
  scans all used slots, skips slots whose Osd filter is not `infocus`, evaluates
  the control (virtual axis: `FUN_00565b60(id) > 0.2`; key: `FUN_00544900(0, 0,
  id)` i.e. `getAxis(0,id) > 0.1`), suppresses the edge on the first evaluation
  via +0x1c, updates the Hotkey state object through `FUN_004ed490`, and queues
  exactly one `Game__queueEvent(handler, 0, 0x100000, hotkeyObject, 0)` for the
  first matching changed slot per call. [V]
- `flushHotkeys` 0x4de970 — raw loop over 0x5fe478..0x602478, stride 0x20; for
  every used slot clears +0x1c (the first-evaluation latch), so the next
  `checkHotkeys` does not report a spurious edge. [V]
- `lastKey` 0x4de9f0 — calls `FUN_00544930`, which does `Acquire`,
  `GetDeviceData(DIDEVICEOBJECTDATA, 0x14)`, requires `dwOfs < 0x100` and
  `dwData & 0x80`, then `MapVirtualKeyExA` + `GetKeyboardState` + `ToAsciiEx`
  and returns `(ansi_char << 16) | scan_code`. `lastKey` returns the ANSI char
  (0 = no key) and pushes that char into a 16-byte ring buffer at
  `0x005fe3c0..0x005fe3cf` using the write index `DAT_005dadb8`, wrapping by
  -0x10. Java `Input.flushKeys()` drains it with `while(lastKey());`. [V for the
  reader; the ring's consumer is unknown [?] and the exact `lastKey` return
  value (char vs packed word) is [?]]

No separate native exists for `java.io.Hotkey`; the class is pure script state.

### 1.5 `java.io.Controller` (virtual axes)

A Controller object `C` is created per script `Controller`; the native handle is
the script object's native pointer. Layout:

    C+0x000  int count                      // number of used mapping entries
    C+0x004  AxisRecord axis[76]            // 0x30 bytes each, v=0..75
    C+0xE44  Mapping map[152]               // 0x20 bytes each
    C+0x2144 end

`AxisRecord A_v = C + 4 + v*0x30`: [V unless marked]

| off | meaning |
|---|---|
| 0x00 | current (cached) axis value |
| 0x04 | kind tag from `FUN_005447e0` (1 axis, 0 button/POV) |
| 0x08 | update stamp (`DAT_007a5b60`) |
| 0x0c | center range, default 0.1 |
| 0x10 | center factor, default 1.0 |
| 0x14 | opposite-direction factor, default 1.0 |
| 0x18 | same-direction factor, default 1.0 |
| 0x1c | speed, default 1.0 (set by `user_SetAxisSpeed`) |
| 0x20 | power, default 0.0; >1 selects the `|v|^(1/power)` shaping curve |
| 0x24 | force `c` |
| 0x28 | force `f`; nonzero halves the center range |
| 0x2c | smoothing time, default 0. Setter not located [?] |

`Mapping M_k = C + 0xE44 + k*0x20`, `k < 152`:

| off | meaning |
|---|---|
| 0x00 | virtual axis v |
| 0x04 | device index |
| 0x08 | physical axis/control id |
| 0x0c | last raw value |
| 0x10 | input min |
| 0x14 | input max |
| 0x18 | output min |
| 0x1c | output max |

Natives:

- `user_Add(vaxis, device, paxis, a, b, c, d)` 0x4d3cb0 — rejects negative
  device/paxis, returns 0 when the table is full, stores a mapping with ranges
  from a..d, sets the axis record kind via `FUN_005447e0`, increments count,
  returns **slot + 1** (0 = full, -1 = invalid). [V]
- `user_Del(vaxis, device, paxis)` 0x4d3db0 — finds and zeroes the matching
  mapping, decrements count. [V]
- `user_GetAxisVal(vaxis)` 0x4d3e60 — sums the contributions of every mapping
  for that virtual axis (`lerp` from input range to output range), clamps to
  [-1,1], then applies the per-kind filter: buttons are smoothed; mouse (device
  type 2) is passed through directly; joystick approaches the target at
  +/-0.02 or uses a 0.3 lerp [I: exact branch condition]. Re-evaluating in the
  same frame returns the cached A+0x00 value, keyed by the stamp at A+0x08.
  Finally applies the shaping curve when power > 1. [V for structure, I for
  filter constants]
- `user_Reset()` 0x4d3f10 — zeroes all axis/mapping state and restores the 76
  record defaults (center 0.1, factors 1.0, speed 1.0, power 0). [V]
- `user_SetAxisForce(vaxis, c, f)` 0x4d3fb0 — writes A+0x24/+0x28; when f is
  nonzero the center range is halved. [V]
- `user_SetAxisSmooth(vaxis, a, b, c, d, e)` 0x4d4080 — writes five tuning
  floats into the axis record. The a..e → field mapping was not re-verified
  byte-by-byte; candidate slots are +0x0c, +0x10, +0x14, +0x18 and +0x1c/+0x2c
  [?].
- `user_SetAxisSpeed(vaxis, a)` 0x4d4180 — writes A+0x1c. [V]

`Controller.java` uses `controlFile` for saved mappings; the file parser lives
script-side. `Input.mapAxis`/`setAxisSmooth` are the convenience entry points
used by the game (they take a `GameRef` rather than a Controller object).

### 1.6 Input call chains

Startup:

    FUN_004ed680 (process init)
      -> FUN_00565fc0 (input init)
           -> FUN_00544a60 DirectInput8Create
           -> EnumDevices(4, cb, 0, 1)
           -> for each device: store COM ptr at B+0x00, type at B+0x30,
              build axis/id/name tables, Acquire
           -> reset smoothing globals (DAT_00794b74, ...)

    exit: FUN_005449f0 (DirectInput shutdown)

**Corrected in notes/18 §2: the main loop does call `0x566160(dt)` (device poll + controller tick) every >15 ms.** Original (wrong) text: per frame there is **no** dedicated engine input update in the main loop
`FUN_0047d760`; scripts drive polling: `Input.getAxis`/`Controller.user_GetAxisVal`
read device state on demand, `Input.checkHotkeys` is called by the game/UI layer,
and `Input.lastKey` drains the buffered keyboard queue. [V for the main loop not
calling input; I that the Game layer calls checkHotkeys each frame]

Event dispatch:

    script Input.checkHotkeys(inst, infocus)
      -> FUN_0047c640(ref, 0x1b, 0)        // GameRef context
      -> for each used slot: FUN_00565b60(axis) (>0.2) or FUN_00544900(0,0,id)
      -> FUN_004ed490(Hotkey, state)
      -> Game__queueEvent(handler, 0, 0x100000, hotkeyObj, 0)
      -> (later) script handler receives the queued event

`Game__queueEvent` is separate from the audio `Game__postEvent` at 0x4eb5c0.

Device selection: `FUN_005447e0` supplies the axis kind for `user_Add`;
`FUN_00544830` is shared by `getAxis`, the hotkey key test and the controller
value evaluation; `FUN_00544930` is only used by `lastKey`.

### 1.7 Corrections and additions (native glue pass, cards 020-026)

All [V] by decompiling the cited addresses.

- Controller block location: Controller natives resolve `this` -> handle -> RI
  (+0xc) -> typed object; the block is `C = *(typed + 0x4c) + 0x1c`, passed as
  ECX to 0x565aa0..0x566140. It is per Controller instance, not global.
- `Input.mapAxis`/`setAxisSmooth`/`checkHotkeys` get the same block through
  `Game(0x5fde90).getInfo(inst, 0x1b, 0)` (0x47c640 -> 0x47e450 -> typed
  vtbl+0x3c); no null check (null C would crash). `Input.setAxisSmooth` calls
  0x5660f0 with power fixed to 1.0.
- 0x5660f0 (`user_SetAxisSmooth`) field map: a->A+0x0c center_range,
  b->A+0x10 center_factor, c->A+0x14 opposite, d->A+0x18 same, e->A+0x20 power.
  The A+0x2c smoothing setter is still not located.
- 0x566030 reset: power default is **1.0** (not 0.0): A+0x0c=0.1, A+0x10..0x20=1.0,
  A+0x24..0x2c=0; mappings only get vaxis=0.
- Native return values: user_Del always 0; user_SetAxisSmooth always 1;
  user_Add 0 when unresolved.
- `createHotkey` 0x4de880 does no handler validation (the 0x1b getInfo is in
  `checkHotkeys`); `deleteHotkey` clears only slot+0 for the first slot whose
  +0x08 equals the Hotkey object; `flushHotkeys` 0x4de970 (raw bytes) zeroes +0x1c
  of used slots. checkHotkeys writes the script field `"state"` of every
  evaluated slot via 0x4ed490.
- `lastKey` 0x4de9f0 (no Ghidra function, raw bytes): returns the **packed**
  `(ansi << 16) | scan` value; `DAT_005dadb8` is a byte pointer into the ring
  0x5fe3c0..0x5fe3cf, wrapped by -0x10 at 0x5fe3d0.
- `axisName` always returns a String ("" for invalid ids); `getDeviceName`
  returns null for an invalid device (0x55b980 maps a NULL char* to null).
- MouseCursor natives read the object field `"cursor"`; getInfo 2 gives x,y,
  0x3b gives x,y,z; `getPos(Vector3)` leaves z untouched; a fresh Vector3 is
  allocated without a constructor (z = 0).

### 1.8 `java.io.FindFile` (0x4d9e90 first, 0x4d9fb0 next, 0x4d9e40 close) [V]

- `first(path, flags)` -> 0x5652c0: path copied to 256 bytes, `/` -> `\`;
  `FindFirstFileA`/`FindNextFileA` wrapper 0x545520/0x5455e0/0x5454d0 (64 OS
  slots at 0x799d90, is_dir = attr bit 4) collects **all** matches into one of
  32 list slots (0x7a59d0, records of 0x104 = name[256] + is_dir), closes the OS
  handle, then `qsort`s by 0x5280c0(a, b, 1) (ASCII case-folded, signed-char
  compare). Returns the slot or <0 (-2 no match, -1 no free slot).
- A packed/virtual-fs branch (0x527c40/0x527d80 over DAT_00773fb0) is guarded by
  `DAT_007a5b50`, which has three reads and no writes -> dead in 2.3.1.
- Filter in first/next: skip "." and ".."; files pass when `flags < 2`, dirs when
  `flags != 1` (0 FILES_DIRS, 1 FILES_ONLY, 2 DIRS_ONLY). Fields `handle` and
  `flags` live on the script object; `first` writes both (handle may be
  negative), `next` reads both. `first` does not free the slot on exhaustion;
  `close` sets handle = -1 and frees slot (0x565270).

---

## Part 2 — Audio

### 2.1 Backend

- Backend is **DirectSound8** (`DSOUND.dll` ordinal imports), plus **DirectShow**
  for compressed music streams. D3D audio is not used. [V]
- Init `FUN_00546da0` creates the DirectSound8 device, sets a primary buffer
  with `WAVEFORMATEX = { wFormatTag 1 (PCM), nChannels 2, nSamplesPerSec 22050,
  nAvgBytesPerSec 88200, nBlockAlign 4, wBitsPerSample 16 }`, flags 0x11
  (`DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRL3D`) and looping. [V]
- The 3D listener object is obtained by QueryInterface with IID at
  `0x005b6bf4`; listener writes go through `FUN_00547580`
  (`IDirectSound3DListener` SetPosition/SetOrientation). [V]
- Voice/mixer: `DAT_0079a108 > 3` and `DAT_0079a0f0 > 3` decide 3D-hardware and
  mix-hardware availability. Config keys `Sound_3D_HW` / `Sound_Mix_HW` are read
  at startup and stored by `FUN_00546ba0` into `DAT_0079a0c0` / `DAT_0079a0c4`;
  when absent the `> 3` defaults apply. A software mixer thread provides voices
  when mix hardware is absent. `has3DHardware` 0x4e4af0 and `hasMixHardware`
  0x4e4b00 are jumps to `FUN_00546be0` / `FUN_00546bf0`. [V]
- Shutdown `FUN_00546d00` (sound) is called at process exit alongside the
  DirectInput shutdown. [V]
- Channel volumes are globals, not per-object:
  `DAT_005dada0` = effects (channel 0), `DAT_005dada4` = music (1),
  `DAT_005dada8` = engine (2). `Sound.setVolume(channel, v)` writes the global;
  channel 1 additionally applies `FUN_00568ed0(v)` to the current music set. [V]

`java.sound.Sound` natives (2.3.1 table; music-track calls are new relative to
the 2.2.1 sources):

| native | behavior |
|---|---|
| 0x4e48f0 `changeMusicSet(int)` | maps 0..5 to set objects (0→0x5fe3d0, 1→0x5fe418, 2→0x5fe434, 3→0x5fe3ec, 4→0x602478, 5→0x602494), default null. If different from `DAT_005fe3b8`: stop old (`FUN_00569010`), store, start new (`FUN_00568f40`), apply music volume `FUN_00568ed0(DAT_005dada4)`. [V] |
| 0x4e49b0 `playTrack(int)` | `FUN_00568a50(index)` when a set is current. [V] |
| 0x4e49e0 `playTrack(String)` | `FUN_00568a90(string)` when a set is current. [V] |
| 0x4e4a10 `enableDebugDump(ResourceRef)` | no-op, returns 0. [V] |
| 0x4e4a30 `firstTrack()` | tail-jump 0x568b10: stop stream (0x5473b0), mode 5 → prev_mode, track = 0, `start_set` 0x568f40. [V] |
| 0x4e4a40 `getMode()` | returns current set +0x14, or -1 if no set. [V] |
| 0x4e4a60 `getNowPlaying()` | `FUN_00568b40` → current track index (+0x08); with no set it returns an uninitialised stack slot (`push ecx`/`mov eax,[ebp-4]`), same for `getTotalTracks`. [V] |
| 0x4e4a80 `getTotalTracks()` | `FUN_00568b50` → track count (+0x0c). [V] |
| 0x4e4aa0 `getVolume(channel)` | returns effects/music/engine global, 0 for other channels. [V] |
| 0x4e4af0 `has3DHardware()` | tail-jump 0x546be0. [V] |
| 0x4e4b00 `hasMixHardware()` | tail-jump 0x546bf0. [V] |
| 0x4e4b10 `nextTrack()` | tail-jump 0x568c70 (index +1). [V] |
| 0x4e4b20 `prevTrack()` | tail-jump 0x568ca0 (index -1). [V] |
| 0x4e4b30 `randomTrack()` | tail-jump 0x568cd0 (`GetTickCount() % count`). [V] |
| 0x4e4b40 `setMode(int)` | clamp < 6, store set+0x14, return it. [V] |
| 0x4e4b80 `setVolume(channel, v)` | writes channel global; channel 1 also `FUN_00568ed0(v)`. [V] |
| 0x4e4c00 `stopMusic()` | tail-jump 0x569090 → `FUN_005473b0`; stops the current track, set stays selected. [V] |

Hardware caps (verified 2026-10): `FUN_00546da0` sets `DSCAPS.dwSize = 0x60` at `0x79a0d0` and calls
`IDirectSound8::GetCaps` (vtable +0x10) right after device creation; on failure it releases the
device and returns -2. `DAT_0079a0f0` = DSCAPS+0x20 `dwFreeHwMixingAllBuffers`, `DAT_0079a108` =
DSCAPS+0x38 `dwFreeHw3DAllBuffers`. So `hasMixHardware()` = `dwFreeHwMixingAllBuffers > 3`,
`has3DHardware()` = `dwFreeHw3DAllBuffers > 3`; both 0 before init (BSS). `FUN_00546830` copies the
same two tests into the config globals `0x79a0c0`/`0x79a0c4` when audio is initialised. [V]

Music-set object fields (current set at `DAT_005fe3b8`; each set is 0x1c-0x40
bytes in static storage): [V for +0x08/+0x0c/+0x14, I for the rest]

| off | meaning |
|---|---|
| 0x04 | set volume |
| 0x08 | current track index |
| 0x0c | track count |
| 0x10 | track string table |
| 0x14 | mode (values 0..5; 5 appears special) |
| 0x18 | previous mode |

Music volume attenuation uses a 64-entry table at `0x5e0648` via
`FUN_00547010`. [! exact curve formula]

### 2.2 SFX events and the voice pool

#### `SfxRef.nplay` 0x4e4610

Java: `private native int nplay(Vector3 pos, float radius, float pitch, float
volume, int flags, int instance)`.

1. Resolve the `SfxRef` resource; on failure print and return -1.
2. If `pos != null`, load its x/y/z through `Instance__getFloatField`.
3. Call the shared event poster:

       Game__postEvent(pitch, DAT_005dada0 * volume, flags, posPtr,
                       instance, radius, 0, 0)

4. Return the instance id (or the value returned by postEvent). [V]

#### `Game__postEvent` 0x4eb5c0

`thiscall`, ECX = the SfxRef/Sound native object:

- Sound resource is at `this+0xc`; owner id at `this+8`.
- Range cull `FUN_00568940(pos, radius)` compares against the listener position
  globals `0x007a7598/759c/75a0` and culls beyond **250 m**. [V; exact compare
  geometry I]
- `Resource__getTyped(0xa0000000)` returns the sound asset: sound id at +0x60,
  base volume at +0x6c. Final volume = asset_volume * caller_volume; values
  <= 0.01 are dropped. [V]
- Builds the 0x3c-byte event record at **0x006024c8**:

      +0x00 int   flags        (| 4 when pos == null, i.e. 2D)
      +0x04 float volume
      +0x08 int   0            (unused/?)
      +0x0c float pitch
      +0x10       (unused/?)
      +0x14/18/1c float pos xyz
      +0x20/24/28 float optional vector 1
      +0x2c/30/34 float optional vector 2
      +0x38 float radius

- Calls `FUN_00568660(sound_id, instance, owner, &event)`; returns instance. [V]

#### Voice pool `FUN_00568660`

Static pool at **0x007a5b98**, **64 slots × 0x5c bytes**:

| off | meaning |
|---|---|
| 0x00 | state: 0 free, 1 new, 2 active, 3 loop-update |
| 0x04 | sound id |
| 0x08 | instance id |
| 0x0c | owner id |
| 0x10 | DirectSound voice handle |
| 0x14 | attenuation |
| 0x18 | copy of the 0x3c-byte event |

Allocation/update rules: [V]

- `instance == 0` allocates a free slot and sets state 1.
- otherwise find the slot matching `(instance, owner)` and update its event.
- 2D events (flags & 4) get attenuation 0; positional events get
  `clamp(dist^2 - radius^2, 16, 62500) / volume^2`. [V]
- The attenuation is **only a priority key**: `FUN_005690a0` qsorts the slot
  indices by it (`FUN_005689b0`, ascending) so the nearest/loudest events get the
  available voices. It never reaches a voice; the voice calls receive the event
  copy at slot+0x18 and recompute gain from scratch in `FUN_005476e0`. [V]

`SfxRef.stop(instance)` 0x4e47d0 → `FUN_004eba90(instance)`; the runtime stop
path is `FUN_00569020(instance, owner)` → `FUN_00546b00`.

#### Per-frame voice service `FUN_005690a0`

Called from the main loop after the listener update:

1. sort the 64 slots (qsort),
2. refill free slots up to the number of available voices
   (`FUN_00546c00`),
3. state 1 → create voice `FUN_00546a90`,
4. state 3 → update looping voice `FUN_00546b60`,
5. state 2 with DirectSound status flag bit 0 (finished) → stop, state reset
   (the code uses the constant `0x501502f9` in this reset path; its exact role
   is [?]).

#### Voice gain/3D parameters `FUN_005476e0(bus, voice, event)` [V]

Called by play `FUN_00546a90` → `FUN_005466d0` and update `FUN_00546b60` →
`FUN_00548630`, with the event copy at slot+0x18 (relative offsets: +0x04
volume, +0x08 2D pan, +0x0c pitch, +0x10 pos, +0x20 velocity = slot `vec1`,
+0x40 radius = slot+0x58). Per-bus state comes from `FUN_00546860` (bus create,
called by the sound resource `FUN_005260b0`): `DAT_0079d600[bus]` min distance
(floored at 0.001), `DAT_0079d800[bus]` max distance, `DAT_0079d580[bus]` 3D
flag = `~resource_flags & 1`. The sound resource parser `FUN_00525d90` reads
`sourcefile`/`volume`/`mindist`/`maxdist`/`instances`/`flags`, defaults
1.0 / **5.0 / 1000.0** / 4 / 0 (asset +0x6c/+0x70/+0x74/+0x64/+0x68).

- 2D bus or 2D event (flags & 4; `FUN_005466d0` also ORs 4 into the voice
  flags of a 2D bus): `SetPan(table(event+0x08))`, volume unchanged.
- Source point: radius > 0 → if `dist^2 >= r^2` the point on the sphere nearest
  the listener (`pos + r/dist * (listener - pos)`), else the listener itself.
- **No 3D hardware** (`DAT_0079a0c0 == 0`, software 3D):
  `d = clamp(|point - listener|, min, max)`,
  `lin = (max - d)/(max - min)` (1 when `max - min <= 0`),
  `gain = min(1, min / d * lin)`, `volume *= gain`. Pan from the listener basis
  (`atan2` of the up component, `SetPan` via the ±64 table `0x5e0748`); pitch
  doppler `pitch *= 1 - dot(v_src - v_listener, rel)/d * 0.01` when pitch > 0
  and d > 0.001.
- **3D hardware** (`DAT_0079a0c0 != 0`): the game computes no gain.
  `IDirectSound3DBuffer::SetPosition(point)` and `SetVelocity(vel)` (deferred,
  committed by `FUN_00546c70` each service). Min/max distances are set once at
  bus creation (`FUN_00546860`: `SetMinDistance(mindist)`, `SetMaxDistance(maxdist)`);
  the listener (`FUN_00547580`, `SetAllParameters`) uses distance, rolloff and
  doppler factors 1.0; 3D buffers are created with
  `DSBCAPS_MUTE3DATMAXDISTANCE` (`FUN_005463a0`, flags 0x200b8/0x600b0). For a
  2D event on a 3D buffer it sets `SetMode(DS3DMODE_DISABLE)`.
- Then `SetVolume(table0x5e0648[(uint)(volume * 64)])` (0 dB above 64); index 0
  returns 0 → play skips `Play`, update calls `Stop`. Frequency: pitch <= 0 →
  bus base rate + random ±0x800, else from pitch, clamped 100..200000.

`src/audio`: `SoftwareMixer` implements the software-3D gain (no pan/doppler,
no bus 2D flag yet); `Ds8Backend` follows the hardware-3D branch.

### 2.3 `SfxTable` (looping engine/part sounds)

#### Storage

SFX tables live in the car engine block:

    script object (Chassis/Part) + 0x4c -> car record
    car + 0x2050                        -> engine block e

    e + 0x0fc  SfxTable 0 (0x248 bytes)
    e + 0x344  SfxTable 1
    e + 0x58c  SfxTable 2

Each table is `0x248` bytes:

    +0x000  Entry entries[16]      // 0x24 bytes each
    +0x240  int   count
    +0x244  float volume multiplier

Entry (also used as an intrusive list node):

| off | meaning |
|---|---|
| 0x00 | next pointer |
| 0x04 | prev pointer |
| 0x08 | id = `*(resource+0x80)` (the RI id word; see 2.6) |
| 0x0c | SfxRef/resource pointer |
| 0x10 | inverse pitch (1.0 / pitch) |
| 0x14 | pitch min |
| 0x18 | pitch max |
| 0x1c | volume min |
| 0x20 | volume max |

Linking is done by `FUN_0040cac0` (`thiscall`, ECX = node, argument = resource):
unlink the node's old resource, then insert into the resource's +0x78 list and
set node+0x08 = *(resource+0x80), node+0x0c = resource. [V]

#### Natives

| native | behavior |
|---|---|
| 0x4b5980 `addItem(ResourceRef sfx, float pitch, float pmin, float pmax, float vmin, float vmax)` | rejects when count >= 16; links the entry with `FUN_0040cac0`; stores `1.0/pitch` at +0x10 and the ranges; increments count. [V] |
| 0x4b5a60 `clear()` | `FUN_00498280` unlinks all entries; count = 0. [V] |
| 0x4b5a90 `getItems()` | returns count. [V] |

#### Evaluator `FUN_004eb7a0`

`thiscall`, ECX = SfxTable; parameters: value (engine RPM-like) and a volume
scale. Algorithm: [V, formula details I]

- if count == 0, return.
- find the entry whose `[pmin, pmax]` contains value; interpolate
  `t = (value - pmin)/(pmax - pmin)`, `volume = lerp(vmin, vmax, t)`,
  pitch/rate = `value * inversePitch` (i.e. value/entry_pitch).
- adjacent ranges are cross-faded at their boundary so volume does not jump.
- if no range contains value, fall back to the nearest range with a fade based
  on distance.
- multiply by the table volume multiplier (+0x244) and the caller scale; clamp
  the value to 2500.
- the resulting volume/pitch are used to play the entry's resource through the
  same event path as `SfxRef.nplay` [I: exact bridge from evaluator to
  `Game__postEvent`].

#### Consumer: `type_car__vf13` 0x494910 (car vtable slot 13)

Per-frame engine/vehicle sound update:

- engine RPM = `|rpm * 9.549296| + 1`, scaled by `DAT_005fe10c`
  (9.549296 = 60/2pi, so the stored rpm is rad/s). [V]
- selects table 0/1/2 by engine state and calls `FUN_004eb7a0`; table counts are
  read at e+0x33c (table 0), e+0x584 (table 1), e+0x7cc (table 2). [V]
- starter sound node at e+0x810. [V location; node type I]
- horn: selector at car+0x1e00; checks node owner word at
  `e+0x840 + sel*0x10` (= node+0x08), uses pitch at `e+0x878 + sel*4` and
  volume `DAT_005dada0` (effects channel). [V]
- nitro node at e+0x818. [V location]
- exhaust pops use timers at car+0x224c / car+0x2248. [V]
- engine sounds use volume `DAT_005dada8` (engine channel). [V]

### 2.4 Chassis / Part SFX natives

| native | behavior |
|---|---|
| 0x4b3890 `Chassis.getSfxTable(int id)` | resolves the car/engine block and wraps the raw table pointer `e+0xfc + id*0x248` into a script `java.game.parts.SfxTable` object (`JVM__getClass`, `FUN_00549ee0`, `FUN_004ed320`). [V] |
| 0x4b4f40 `Chassis.setHornSFX(ResourceRef sfx, float pitch, int index)` | index < 4; links the intrusive node at `e+0x838 + index*0x10` through `FUN_0040cac0` and stores pitch at `e+0x878 + index*4`. [V] |
| 0x4b5130 `Chassis.setNitroSFX(ResourceRef sfx, float pitch)` | links the node at e+0x818; the pitch argument is ignored by the native. [V] |
| 0x4b4710 `Chassis.setSfxExhaustMinVol(...)` | not decompiled in this pass [?] |
| 0x4cdb80 `Part.setSfxLoopParams(float vol, float pitch)` | resolves the record through native object +0x4c, writes volume at +0x144 and pitch at +0x148, returns 1. The consumer of these two fields was not located [?]. |

Script-side flow (from `Chassis.java`, `SfxTable.java`, `Part.java`):

    Chassis.getSfxTable(i)                       // engine/parts table
      .addItem(sfx, pitch, pmin, pmax, vmin, vmax)
    Chassis.setHornSFX(hornSfx, pitch, hornIndex)     // 4 slots
    Chassis.setNitroSFX(nitroSfx, pitch)
    Part.setSfxLoopParams(vol, pitch)                 // hidden part feature

The `SfxTable` entries are the looping sources evaluated by
`type_car__vf13`; one-shot sounds go through `SfxRef.nplay` directly. [I for the
one-shot/loop split]

### 2.5 Audio call chains

Startup and shutdown:

    FUN_004ed680 (process init)
      -> D3D init FUN_00421280
      -> input init FUN_00565fc0
      -> sound init FUN_00546da0         // DirectSound8Create, primary buffer,
                                          // listener, mixer thread
      -> config Sound_3D_HW / Sound_Mix_HW
      -> FUN_00546ba0                     // capability flags -> 0x79a0c0/0c4

    exit: FUN_00546d00 (sound) + FUN_005449f0 (DirectInput)

Per frame (main loop `FUN_0047d760`):

    physicsTick / Game__physicsTick
    FUN_0047d070 (game/script update — calls Sound/SfxRef/Hotkey natives)
    build listener block from camera globals DAT_005fe130/138/13c/144
      and two basis vectors via FUN_00566800(1) / FUN_00566800(2)   [I: camera]
    FUN_00568bb0(listener) -> FUN_00546cd0 -> FUN_00547580
      (IDirectSound3DListener SetPosition / SetOrientation)
    FUN_005690a0 (voice pool service)
    render

Playback path:

    script SfxRef.nplay(pos, radius, pitch, volume, flags, instance)
      -> Game__postEvent(pitch, effects_volume*volume, flags, pos,
                         instance, radius, 0, 0)
      -> FUN_00568660 (allocate/update 64-slot voice pool)
      -> FUN_005690a0 (per-frame sort/refill)
      -> FUN_00546a90 (create) / FUN_00546b60 (loop update)
      -> DirectSound buffer; 3D params from IDirectSound3DListener

    script SfxRef.stop(instance)
      -> FUN_004eba90 -> FUN_00569020 -> FUN_00546b00 (stop)

Music path:

    script Sound.changeMusicSet(id) -> FUN_00569010 (stop) / FUN_00568f40
      (start) / FUN_00568ed0 (volume) on the selected static set object
    script Sound.playTrack(int|String) -> FUN_00568a50 / FUN_00568a90
    script Sound.nextTrack/prevTrack/randomTrack -> 0x568c70/0x568ca0/0x568cd0
    script Sound.stopMusic -> FUN_005473b0 (track stop only)
    compressed streams use DirectShow; others are read directly [I]

Engine sound path:

    type_car__vf13 (car vtable +13)
      -> FUN_004eb7a0(table0/1/2)          // RPM-driven looping engine sound
      -> entries link SfxRef resources via FUN_0040cac0
      -> horn (e+0x838..) / nitro (e+0x818) / starter (e+0x810)
         play through the SFX event path

---

### 2.6 Process-wide SFX service: owners, sound resources, buses (traced 2026-10-23) [V]

Names below are in `analysis/names.json`. All addresses checked by decompile;
ECX values marked "asm" were read from `procedure_assembly`.

#### Who owns what

There is no service object: everything is static storage.

| exe storage | what | src |
|---|---|---|
| `0x7a5b98` 64 x 0x5c | voice/event pool (2.2) | `audio::VoicePool::slots` |
| `0x7a7298` int[64] | qsort index array | `VoicePool::service` local `order` |
| `0x7a5b90` / `0x7a5b94` | pool counter (0) / listener id from `Sound__allocListener` 0x546c50 | - |
| `0x7a7598..0x7a75d0` | listener block (pos, vel, -fwd, -up; 3 floats each, 16-byte stride) | `VoicePool::listener` (pos only) |
| `0x79a180 + bus*0x20` | `IDirectSoundBuffer*[8]` per bus (buffer 0 = original, 1..7 duplicates) | `SoftwareMixer::samples_[bus]` |
| `0x79b180 + bus*0x20` | `IDirectSound3DBuffer*[8]` (null for 2D bus or no 3D HW) | Ds8Backend |
| `0x79d180[bus]` | base frequency (GetFrequency of buffer 0) | `MixSample::rate` |
| `0x79d380[bus]` | voice limit = min(instances, 8) | `SoftwareMixer::bus_limit` (today constant 8) |
| `0x79d580[bus]` (byte) | 3D flag = `~flags & 1` | not modelled yet |
| `0x79d600[bus]` / `0x79d800[bus]` | min dist (floored 0.001) / max dist | `min_distance_` / `max_distance_` |
| `0x7a8400[bus]` | source file name pointer (CompatibleSound+0x28) | - |
| `0x79da00[bus]` | cleared on release (other writer not traced) | - |
| `0x79a0be` (byte) | "sound initialised"; every back-end entry returns -1/0 when 0 | - |

Pool init `SfxPool__init` 0x568b60: `0x7a5b90 = 0; 0x7a5b94 = 0x546c50();`
then for i in 0..63 `slot.state = 0; slot.bus = 0; slot.attenuation = 0x501502f9
(1e10f); order[i] = i`. It is called only from 0x56946e, inside an
un-analysed function at **0x569420** that Ghidra did not define: the only
reference to 0x569420 is the data word at 0x593364 (a CRT static-initialiser
table). The bytes are `push 0; call [0x59332c]` followed by `call` to
0x52f990, 0x548a50, 0x545730, 0x543040, 0x527e50, 0x5679a0, 0x565530,
0x5697a0, 0x461910, **0x568b60**, then `push 0x5b6429; mov ecx,0x7a75e8;
call ...; ret`. So the pool is set up by C++ static init before WinMain,
independent of DirectSound init (`0x546da0` sets 0x79a0be). The import at
0x59332c was not resolved by name [UNRESOLVED raw: `6a00 ff152c335900`].

Per frame (`Game__mainLoop` 0x47d760; every loop iteration, *outside* the
15 ms control gate, after `0x47ba10(dt)` and `Game__flushDeferred`):

    listener = {0}; fwd = (0,0,1); up = (0,1,0)          // defaults
    if (DAT_005f1e44 > 0) {                               // a camera exists
        0x566800(2, &fwd) (reset to (0,0,1) if |v| < 0.1)
        0x566800(1, &up)  (reset to (0,1,0) if |v| < 0.1)
        pos = DAT_005fe130/134/138, vel = DAT_005fe13c/140/144
    }
    SfxPool__setListener 0x568bb0({pos, vel, -fwd, -up})  // copies to 0x7a7598.., then 0x546cd0
    SfxPool__service 0x5690a0()

With no camera the listener position is (0,0,0) (exe value, not invented).

#### Sound resource = `Resource_Sound` (RTTI), record type 6 (`res_sound`)

Shipped data: the record data of a type-6 record is the `.rsd` text, e.g.
`parts/engines/Baiern_Emer.rpk` record 0xa0 "0900":
`sourcefile parts\engines\Baiern_Emer\sounds\engine_IDL\0900RPM.wav\r\nvolume 1.000\r\ninstances 2.000\r\nmindist 2.000\r\nmaxdist 20.000\r\n`.
Records with size 0 are folders/definitions (`system.rpk` 0x9 `res_sound`,
Axis.rpk 0x2 "Axis"). The rprop schema in system.rdb lists exactly
sourcefile/volume/instances/mindist/maxdist/flags.

Layout: `RI+0x8c` data size, `+0x94` bit0 loaded, `+0x98` loaded level,
`+0x9c` requested level (1000.0 while a synchronous load runs), embedded
`Resource_Sound_Data` at **RI+0xb0** (vtable `Resource_Sound_Data`), whose
`+0x8` (RI+0xb8) is the owner id and `+0xc` (RI+0xbc) the `CompatibleSound*`.

`Resource_Sound__vf10` 0x525cf0 (load, asm):

    DAT_005db3e0 = (this+0x9c != 1000.0f /*0x59746c*/);   // 1 = async file read
    if (this+0x94 & 1) { this+0x98 = 1.0; return 1.0; }   // already loaded
    r = Resource_Sound_Data__parse(ECX = this+0xb0, data, this+0x8c);
    ResourceNode__setLoaded(1); this+0x98 = r; return r;

`Resource_Sound_Data__parse` 0x525d90 (ECX = data block D):

    if (data == 0) return 0.0;
    volume = 1.0; mindist = 5.0; maxdist = 1000.0; instances = 4; flags = 0;
    src[0] = 0; p = data;
    do {
        p += Str__readLine(p, line);      // 0x5281b0: copies through '\n' (kept) or NUL
        if (line[0]) {
            sscanf(line, "%s", key);      // fmt 0x5a9020; key NOT reset when this fails
            if      (!stricmp(key,"sourcefile")) sscanf(line,"%*s %s", src);
            else if (!stricmp(key,"volume"))     sscanf(line,"%*s %f",&volume);
            else if (!stricmp(key,"mindist"))    sscanf(line,"%*s %f",&mindist);
            else if (!stricmp(key,"maxdist"))    sscanf(line,"%*s %f",&maxdist);
            else if (!stricmp(key,"instances"))  sscanf(line,"%*s %d",&instances);
            else if (!stricmp(key,"flags"))      sscanf(line,"%*s %d",&flags);
        }
    } while (p - data < size);
    if (src[0] == 0) return 0.0;
    C = Compatible__create(5, DAT_005db3e0, src, 0, D+8, 0);    // 0x455e80
    D+0xc = C;
    if (C) { C+0x6c = volume; C+0x70 = mindist; C+0x74 = maxdist;
             C+0x64 = instances; C+0x68 = flags;
             if (DAT_005db3e0 == 0) Compatible__decodeSync(ECX = C); }  // 0x526030
    return 1.0;                // also when C == 0 (missing file): loaded but silent

`%d` on "2.000" gives 2, on "0.500" gives 0 (one shipped record: a bus with
voice limit 0 never plays). "flags 1.000" gives 1 → 2D bus.

`Compatible__create` 0x455e80 (type 5 = sound; types 0..4 are other
"Compatible" kinds incl. `CompatibleMesh_static`): `C = new 0x7c`
`CompatibleSound__ctor` 0x525b00 (defaults +0x6c 1.0, +0x70 5.0, +0x74 1000.0,
+0x64 4, +0x68 0, +0x60 bus = -1, +0x78 0), then `C+0x54 = type, +0x58 =
owner, +0x5c = 0, +0x38 flags = 0, +0x3c = 0`, `C+0x28 = strdup(sprintf("%s",
path))` (fmt 0x598fa0). No name cache: every Resource_Sound gets its own
CompatibleSound and bus.
- sync (`param_2 == 0`): `flags |= 0x20000000; C+0x44 = File__readAll(path,
  &C+0x40)` (0x565610: '/'→'\\', open/size/read whole file, returns size or
  -1/-2); size < 1 → delete C, return 0. (For type 5 the decode is left to
  0x526030.)
- async: `0x564d70(path, 0, Compatible__onAsyncRead 0x455db0, C, 0)`; on
  completion the callback stores data/size and queues C on the decode list
  0x5f00c8 (flags `|2`), or marks it failed (`|0x80000004`).

`Compatible__decodeSync` 0x526030: `flags |= 2; while (!(flags & 4)) {
DAT_005eff58 = C; C->vf1(); }` then frees the file bytes (C+0x40 = 0, C+0x44 = 0).
`CompatibleSound__vf1` 0x525fa0 calls `vf3` once (timed with 0x56a230/0x56a260
slot 0xb, `C+0x34 = ms*100`), unlinks C from its list and relinks it on 0x5f00c8
if bit 2 is still set.

`CompatibleSound__vf3` 0x5260b0 (upload):

    if (C+0x44 /*file size*/ > 0) {
        C+0x60 = Sound__createBus(C+0x40 data, C+0x44 size, C+0x64 instances,
                                  ~C+0x68 & 1 /*3D*/, C+0x70 min, C+0x74 max, C+0x28 name);
        C+0x78 = C+0x44; DAT_00773f18 += C+0x44;      // sound memory counter
    }
    C+0x38 = (C+0x38 & ~2) | 0xc; return 1;

`CompatibleSound__vf0` 0x525c40 (dtor): `if (bus >= 0) { Sound__releaseBus(bus);
DAT_00773f18 -= C+0x78; bus = -1; }`. Unload `Resource_Sound__vf7` 0x526070
(reached by ResourceRef.unload → 0x51a1b0 vf[+0x1c]): if loaded, delete
RI+0xbc, RI+0x98 = 0, setLoaded(0). Pool slots still holding that bus are not
touched.

#### Bus = one sound sample with up to 8 playback buffers

`Sound__createBus` 0x546860(data, size, instances, is3d, min, max, name):

    if (!0x79a0be) return -1;
    bus = first b in 0..127 with buffers[b][0] == 0; none → return -1;
    b0 = DSBuffer__createFromWav(data, is3d);        // 0x5463a0
    buffers[bus][0] = b0; if (!b0) return -2;
    b3d[bus][0] = (is3d && 0x79a0c0) ? QI(b0, IID 0x5b6be4) + SetMinDistance(min) + SetMaxDistance(max) : 0;
    if (min <= 0.001) min = 0.001;
    n = instances < 9 ? instances : 8;
    limit[bus] = n; mind[bus] = min; flag3d[bus] = is3d; maxd[bus] = max; name[bus] = name;
    for i in 1..n-1: buffers[bus][i] = DuplicateSoundBuffer(b0) (0 on failure; 3D iface as above);
    b0->GetFrequency(&freq[bus]);
    return bus;

`DSBuffer__createFromWav` 0x5463a0: `Wav__parse(data, &bytes, &wfx, &pcm)`; desc
`{0x24, flags, bytes, 0, wfx}`, flags `0xe8` (2D or no 3D HW) / `0x200b8`
(3D), replaced by `0x400e0` / `0x600b0` when `0x79a0c4` (mix HW) is set;
`CreateSoundBuffer`; `DSBuffer__fill` 0x5461e0 (Lock 0..bytes, memcpy both
parts, Unlock; fails when pcm/bytes is 0) else Release and return 0.

`Wav__parse` 0x5462a0 (no bounds checks in the exe):

    if (!p || p[0] != 'RIFF' || p[2] != 'WAVE') return -1;
    if (p[3] != 'fmt ') return -2;                  // fmt must be the first chunk (offset 12)
    *wfx = p + 20;                                  // WAVEFORMATEX as stored
    q = p + 20 + p[4];                              // skip fmt body
    while (q[0] != 'data') q += 8 + q[1];           // PAD/fact/LIST/smpl/DISP/JUNK seen
    *bytes = q[1]; *pcm = q + 8; return 0;

`Sound__releaseBus` 0x546b40 → `0x5474f0(bus)`: for each of the 8 buffers:
Stop, release the 3D iface when flag3d, Release, null; `0x79da00[bus] = 0`.
`Sound__busVoiceLimit` 0x546a70 = `limit[bus]`; `Sound__busAvailable` 0x546b80
= `buffers[bus][0] != 0`; `Sound__availableVoices` 0x546c00 = max(4,
(0x79a0c4 ? DSCAPS+0x24 : 0) + (0x79a0c0 ? DSCAPS+0x3c : 0)).

Shipped-data check (Python over the install, 2026-10-23): 157 `.rpk`, 1,765
type-6 records (174 empty, 1 without sourcefile); 623 referenced `.wav` files
exist, 967 are missing (896 from `EngineSoundPack.rpk`, partially installed).
All 623 pass the `Wav__parse` rule: PCM tag 1, 617 x 16-bit, 5 x 8-bit,
1 x 32-bit (`sound/wav/ambient/Madarak.wav`), mono/stereo, 7.2-48 kHz; chunk
sequences before `data`: none (546), PAD (43), fact (16), LIST (11), smpl (5),
DISP+LIST+PAD (1), JUNK (1).

#### Typed lookup from a script SfxRef

`Game__postEvent` 0x4eb5c0 and `Handle__stopSfx` 0x4eba90 run with ECX = a
0x10-byte ref node (`ResRefLink`): node+0x8 = id, node+0xc = RI.
`ResRefLink__set` 0x40cac0 writes node+8 = `*(RI+0x80)` (the RI's id word),
so the **owner passed to the pool is the resource id**, not `RI+0x80`
(src `audio/sfx.cpp` stores `resource + 0x80`: arithmetic on the id, a misread).

    RI = node+0xc; if (!RI) return -1;
    if (RI+0x7c != 1) RI->vf5(1.0);                    // not traced
    if (Resource__getTyped(ECX = RI, mask, a, b) < 0) return -1;   // 0 when loaded, 0x80000001 otherwise
    T = RI->vf3(1.0);  C = T+0xc;                      // T = Resource_Sound_Data (RI+0xb0)
    if (!T || !C || C+0x60 /*bus*/ < 0) return -1;
    vol = C+0x6c * volume; if (!(vol > 0.01)) return -1;
    ... event at 0x6024c8 (2.2) ...; SfxPool__post(bus, instance, node+8, &event); return instance;

(nplay: range cull `SfxPool__inRange` 0x568940 first, mask `0xa0000000`,
args `(0, 1.0)`; stop: mask `0x80000000`, args `(0, 0)`, then
`SfxPool__stop(instance, node+8)`.)

`Resource__getTyped` 0x51f4f0 → `ResourceNode__requestLoad` 0x518a80 when not
loaded (mask bit 0x40000000 clear in both callers):
- `(a, b) == (0, 0)` or `RI+0x82 == -1` → **synchronous**: read the record
  data (0x517b80/0x51e500), set `+0x9c = 1000.0`, call vf10 (+0x28), restore
  `+0x9c = 1.0` → the sound is parsed and decoded now. So `SfxRef.stop`
  loads an unloaded sound synchronously.
- otherwise (nplay's `(0, 1.0)` for a package record) → queues a streaming
  request (node from `+0x1664`, `0x51e5c0`, flag `RI+0x84 |= 0x2000000`) and
  returns; getTyped then returns 0x80000001, so **the first nplay of a sound
  that is not yet loaded returns -1 and plays nothing**. When the queue runs,
  vf10 sees `+0x9c == 1.0` → `DAT_005db3e0 = 1` → async file read too.
  [UNRESOLVED: when the streaming queue (0x51e5c0, drained via 0x517f60/
  0x518f80 in the main loop) completes relative to the next frame was not traced.]
- `ResourceRef.precache` → `Handle__precache` 0x4e9830 → `0x468b00(0xa0000001,
  1.0, 0, 1.0)` [0x468b00 not traced].

#### Engine SFX bridge (resolves Part 3 item 11)

`SfxTable__evaluate` 0x4eb7a0 calls the public `Game__postEvent` with
`ECX = entry` (asm 0x4eb96e `LEA ECX,[ESI-0x14]` with ESI = entry+0x14;
fallback 0x4eba39 `LEA ECX,[EBX+idx*0x24]`): an SfxTable entry *is* a
ResRefLink (+0 next, +4 prev, +8 id, +0xc RI). Arguments: pitch =
`entry+0x10 * value`, volume = `fade * scale * lerp(vmin, vmax, t)`, flags,
pos, instance, radius, vec1, vec2 passed through. The table scan stops at the
first entry whose +8 is 0.

#### src mapping

| exe | src |
|---|---|
| CompatibleSound +0x60 bus, +0x6c volume | `AudioAssets::Asset::bus/volume` |
| +0x70/+0x74 | `SoftwareMixer::set_bus_distances` |
| +0x64 instances | `bus_limit` (needs min(instances,8)) |
| +0x68 flags bit0 | bus 3D flag (missing) |
| node+8 owner | `Voice::owner`, `SfxItem::owner` (should equal resource id) |
| pool 0x7a5b98 / 0x568b60 | `VoicePool` ctor |
| 0x5690a0 per frame | `VoicePool::service` (not called by `FrameLoop` yet) |

Cards: tasks/081 (resource parser + WAV), 082 (bus allocation), 083 (service +
SfxRef natives), 084 (frame hook + owner fix).

---

## Part 3 — Open questions / uncertain items

1. `FUN_00543f70` (device refresh + negative-id reset) and the exact mouse
   button control-id encoding.
2. Controller object: exact `a..e` destination in `user_SetAxisSmooth`; setter
   and semantics of the power field (+0x20); precise force `c`/`f` semantics
   beyond the center-range halving.
3. `lastKey`: exact return value (ANSI char vs `(char<<16)|scan`) and the
   consumer of the 16-byte ring at 0x5fe3c0.
4. `createHotkey`: duplicate/occupied-slot handling; exact fields cleared by
   `deleteHotkey`; whether the edge detector also releases on key-up through the
   event mask.
5. `FUN_00566800(1|2)` object identity (likely camera basis vectors) and the
   exact camera-global to listener-field mapping.
6. Audio: DirectShow vs direct-file decision logic; mixer thread internals;
   (resolved: voice handle = `bus << 16 | buffer` (128 x 8); slot attenuation
   (+0x14) is only the qsort key and `0x501502f9` = 1.0e10f sorts freed slots
   last; voice gain is `FUN_005476e0`, see 2.2); music-set +0x04/+0x10/+0x18 semantics;
   the 64-entry attenuation curve at 0x5e0648.
7. `Part.setSfxLoopParams`: which record +0x144/+0x148 belongs to and what
   consumes those values (engine loop volume/pitch).
8. Starter node at e+0x810: whether it is a plain resource node or an SfxTable
   style entry, and whether `type_car__vf13` uses `FUN_004eb7a0` for it.
9. `Chassis.setSfxExhaustMinVol` 0x4b4710 not decompiled in this pass.
10. Music-track Java signatures for the 2.3.1-only natives (playTrack,
    firstTrack, getNowPlaying, ...) are inferred from native argument decoding;
    the 2.2.1 source does not declare them.
11. (resolved, 2.6) `FUN_004eb7a0` calls the public `Game__postEvent` with ECX = the entry.

Also note for future work: notes/00-overview.md still says `FUN_005449f0` is
sound teardown. The correct mapping is `FUN_004ed680` init → `FUN_00565fc0`
input / `FUN_00546da0` sound, and exit → `FUN_00546d00` sound /
`FUN_005449f0` DirectInput.
