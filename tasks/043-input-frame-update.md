---
id: 043
status: merged
branch: card/043
depends: [020]
---
# input: per-frame input update 0x566160 (poll + controller registry + tick + force feedback)

## Goal
The exe's main loop calls `0x566160(dt)` every frame (>15 ms gate, card 044). It
sets the frame clock, polls every device, bumps the frame stamp and runs every
controller's force pass plus a joystick force-feedback pass. Port it as
`input::update(float dt)`, add the process-wide controller list the exe keeps
(0x566010 registers each Controller), and add a no-op force-feedback hook on the
backend. `begin_frame`, `DeviceTable::poll` and `Controller::tick` already exist
and are reused unchanged.

## Exe evidence (cleaned pseudocode, renamed fields)
Controller block `C`: `+0` mapping count, `+4` AxisRecord[76] (stride 0x30:
`+0 value, +4 kind, ... +0x24 force_c, +0x28 force_f, +0x2c smooth`),
`+0xe44` Mapping[152] (stride 0x20: `+0 vaxis, +4 device, +8 index`), `+0x2144` next.

### 0x566010 Controller register (raw bytes, called from the Controller type ctor)
    C->next = g_controllers;  g_controllers = C;  g_controller_count++;   // newest first
    return 1;
    // there is no unregister anywhere in the exe

### 0x566160 input frame(float dt)
    frame_ms = dt * 1000.0f;                   // DAT_007a5b54
    poll_ms  = DeviceTable_poll();              // 0x544ba0 = DeviceTable::poll
    stamp++;                                    // DAT_007a5b60
    for (C = g_controllers; C; C = C->next) {
        <force pass over 76 records, kind == 0>       // == existing Controller::tick()
        for (i = 0; i < 152; i++) {
            m = &C->mappings[i];
            if (m->index < 1 && C->axes[m->vaxis].kind != 0 && device_type(m->device) == 3)
                0x544b40(m->device, m->index, -C->axes[m->vaxis].force_c, C->axes[m->vaxis].force_f);
        }
    }

### 0x544b40 force feedback (device, index, a, b)
    if (device < 0 || device >= device_count) return -1;
    if (ff_enabled /* DAT_0078d0e0 */) 0x5434e0(&record[device], b, 0, a, 0);   // DirectInput effect, not decoded
    return 0;    // index is unused

## Changes
1. `src/input/backend.h`, class `Backend`, after `read_key`:
   ```cpp
   // 0x544b40 -> 0x5434e0: joystick force-feedback effect (a = -force_c, b = force_f).
   // shortcut: no-op everywhere; the DirectInput effect (0x5434e0) is not decoded yet.
   virtual void force_feedback(int device, float a, float b) { (void)device; (void)a; (void)b; }
   ```
2. `src/input/controller.h`:
   - in `namespace input`: `// 0x566160: frame clock, device poll, every controller's tick + FF pass.`
     `void update(float dt_seconds);`
   - in `class Controller` public: `void force_feedback_pass(Backend &backend);  // 0x566160 inner loop`
     (forward-declare `class Backend;` next to `class DeviceTable;`).
   - in `class Controller` private: `Controller *next_ = nullptr;  // +0x2144` and
     `friend void update(float);`.
3. `src/input/controller.cpp` (`#include "input/device.h"` is already there; device.h has
   `devices()`/`backend()` from card 020):
   - file-static `Controller *g_controllers = nullptr; int g_controller_count = 0;` in the
     anonymous namespace (DAT_007a5b68 / DAT_007a5b64).
   - constructor: after member init, `next_ = g_controllers; g_controllers = this; g_controller_count++;`
     with `// 0x566010 register (newest first; the exe never unregisters)`. Add a destructor
     that unlinks `this` from the list, marked `// INVENTED: the exe never destroys controller blocks.`
     (declare `~Controller();` and `Controller(const Controller &) = delete;` /
     `Controller &operator=(const Controller &) = delete;` in the header: the list holds `this`).
   - `force_feedback_pass`: loop over `mappings_[0..kMappingCount)` exactly as above, using
     `devices_.type(m.device) == kJoystick` and `axes_[m.vaxis]`; guard
     `m.vaxis` to `0..kAxisCount-1` with `// INVENTED: bounds guard (exe indexes unchecked)`.
     Call `backend.force_feedback(m.device, -a.force_c, a.force_f)` only when
     `0 <= m.device && m.device < devices_.count()` (0x544b40 range check).
   - `update(dt)`: `begin_frame(dt);` then `if (Backend *b = backend()) devices().poll(*b);`
     then `for (Controller *c = g_controllers; c; c = c->next_) { c->tick(); if (Backend *b = backend()) c->force_feedback_pass(*b); }`.
     Comment `// 0x566160`. Note: `begin_frame` already does `frame_ms = dt*1000` and `stamp++`;
     the exe polls between those two writes, which no reader can observe.

## Files
- edit: src/input/backend.h, src/input/controller.h, src/input/controller.cpp
- read only: src/input/device.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green (also `make -C src -f Makefile.win` compiles if the mingw toolchain is present); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
