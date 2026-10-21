---
id: 020
status: todo
branch:
depends: []
---
# input: process-wide device/hotkey tables + Input device natives

## Goal
The exe keeps one global device table (0x78d0f0, count 0x78d0ec) and one global
hotkey table (0x5fe478). Add process-wide accessors for both (plus the installed
backend) to `src/input/device.*`, then make these `java.io.Input` natives
non-stubs: `getAxis`, `getDeviceName`, `activeAxis`, `axisName`, `lastKey`.
All device logic is already ported (`DeviceTable::get_axis/active_axis/axis_name/
device_name`, `HotkeyTable::last_key`); this card is glue only.

## Exe evidence (cleaned pseudocode, renamed fields)
All five are static natives: `Native__getArgs(frame, 0 /*no this*/, &args...)`.

### 0x4de990 Input.getAxis(int device, int axis) -> float
    return DeviceTable_getAxis(device, axis);          // 0x544830 = DeviceTable::get_axis

### 0x4de9c0 Input.getDeviceName(int i) -> String
    p = 0x565fb0(i)  // tail-call of 0x5448a0:
                     //   if (0 <= i && i < count) return record[i].device_name; else return NULL
    return newString(p)   // 0x55b980: NULL pointer -> returns null (0), else a java String

### 0x4de6d0 Input.activeAxis(int device) -> int
    return DeviceTable_activeAxis(device);             // 0x544670 = DeviceTable::active_axis

### 0x4de700 Input.axisName(int device, int axis) -> String
    char buf[256];
    DeviceTable_axisName(device, axis, buf);  // 0x544750: buf[0]=0 first; invalid -> stays ""
    return newString(buf);                    // never null: invalid ids give ""

### 0x4de9f0 Input.lastKey() -> int   (raw bytes, no Ghidra function)
    eax = 0x544930();                   // buffered keyboard read: (ansi << 16) | scan, 0 = none
    dl = (char)(eax >> 16);
    if (dl != 0) {
        *ring_ptr++ = dl;               // ring_ptr DAT_005dadb8 points into 0x5fe3c0..0x5fe3cf
        if (ring_ptr == 0x5fe3d0) ring_ptr -= 0x10;
    }
    return eax;                         // the PACKED value, not just the char
`HotkeyTable::last_key(Backend&)` already does exactly this (ring = `last_ring`).

## Files
- edit: src/input/device.h, src/input/device.cpp — add (namespace `input`, free functions):
  ```cpp
  class HotkeyTable;
  // Process-wide tables, like the exe globals 0x78d0f0 (devices) / 0x5fe478 (hotkeys).
  DeviceTable &devices();
  HotkeyTable &hotkeys();
  // Null until installed; install_backend runs devices().init(*b) (exe: 0x565fc0 at startup).
  Backend *backend();
  void install_backend(Backend *b);
  ```
  Implement with function-local statics in device.cpp (include "input/hotkeys.h").
  INVENTED: who calls `install_backend` (boot/main loop) is out of scope; with no
  backend the device table is empty (count 0), which every reader already handles.
- edit: src/script/natives.cpp — add `#include "input/device.h"` and `#include "input/hotkeys.h"`,
  five native functions (comment each `// 0xADDR Input.name`) and switch their
  rows from `nullptr, true` to `<fn>, false`:
  - `getAxis (II)F` -> `Value::make_float(input::devices().get_axis(d, i))`
  - `getDeviceName (I)Ljava.lang.String;` -> `device_name(i)` null -> `Value::make_null()`, else `make_str`
  - `activeAxis (I)I` -> `make_int(active_axis(d))`
  - `axisName (II)Ljava.lang.String;` -> always `make_str(axis_name(d, i))`
  - `lastKey ()I` -> `backend()` null -> `make_int(0)` (INVENTED: no keyboard); else
    `make_int((int32_t)input::hotkeys().last_key(*backend()))`
  Use the existing `want`/`get_i` helpers for argument checks.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green (the natives-vs-exe table check must still pass); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
