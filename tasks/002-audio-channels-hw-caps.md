---
id: 002
status: todo
branch:
depends: []
---
# audio: process-wide channel volumes + DirectSound hardware caps

## Goal
`Sound.setVolume/getVolume/has3DHardware/hasMixHardware` read process globals.
Add a global `ChannelVolumes` instance (the exe's `DAT_005dada0/a4/a8`) and a
global copy of the two DirectSound caps counters the exe tests, filled by
`Ds8Backend::init` via `IDirectSound8::GetCaps` exactly where the exe does it.
No natives in this card (cards 003/004 wire them).

## Exe evidence (cleaned pseudocode, renamed fields)
### Channel globals
`DAT_005dada0` effects (channel 0), `DAT_005dada4` music (1), `DAT_005dada8`
engine (2); all 1.0f in the shipped image. `audio::ChannelVolumes` already
models them (defaults 1.0f, `operator[]`); only a single shared instance is missing.

### 0x546da0 sound init (excerpt, verified)
```c
// after DirectSoundCreate8 succeeded, device in DAT_007a8608
DSCAPS caps_0x79a0d0;             // global, BSS (all zero before init)
caps_0x79a0d0.dwSize = 0x60;      // sizeof(DSCAPS)
if (FAILED(device->GetCaps(&caps_0x79a0d0))) {   // vtable +0x10
    device->Release();            // vtable +0x08
    return -2;                    // init fails
}
if (FAILED(device->SetCooperativeLevel(hwnd, 2 /*DSSCL_PRIORITY*/))) { // vtable +0x18
    device->Release();
    return -3;
}
...
```
### 0x546be0 has3DHardware / 0x546bf0 hasMixHardware
```c
bool has_3d_hw()  { return 3 < DAT_0079a108; }  // caps+0x38 dwFreeHw3DAllBuffers
bool has_mix_hw() { return 3 < DAT_0079a0f0; }  // caps+0x20 dwFreeHwMixingAllBuffers
```
(unsigned DWORDs in the exe; compare as `uint32_t > 3`). Before init both are 0
→ both functions return false.

## Files
- edit: `src/audio/audio.h` — in `namespace audio` after `struct ChannelVolumes` add:
  ```cpp
  // DAT_005dada0/a4/a8: one process-wide instance.
  ChannelVolumes &channel_volumes();
  // DSCAPS at 0x79a0d0 (filled by GetCaps in 0x546da0): the two counters the exe tests.
  struct HardwareCaps {
      uint32_t free_hw_mixing_all = 0;  // DSCAPS+0x20, DAT_0079a0f0
      uint32_t free_hw_3d_all = 0;      // DSCAPS+0x38, DAT_0079a108
  };
  HardwareCaps &hardware_caps();
  bool has_3d_hardware();   // 0x546be0
  bool has_mix_hardware();  // 0x546bf0
  ```
- edit: `src/audio/audio.cpp` — function-local statics for the two accessors;
  `has_3d_hardware()` = `hardware_caps().free_hw_3d_all > 3`,
  `has_mix_hardware()` = `hardware_caps().free_hw_mixing_all > 3`.
- edit: `src/audio/dsound.cpp` (`Ds8Backend::init`) — between the
  `DirectSoundCreate8` check and `SetCooperativeLevel`, add
  `DSCAPS caps = {}; caps.dwSize = sizeof caps;` → on `FAILED(impl_->device->GetCaps(&caps))`
  call `shutdown(); return false;`; on success copy `caps.dwFreeHwMixingAllBuffers`
  and `caps.dwFreeHw3DAllBuffers` into `hardware_caps()`. Cite `// 0x546da0 GetCaps`.
  (This file is `#ifdef _WIN32`; it is not compiled by `make check` on Linux —
  keep the edit minimal and syntactically obvious.)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
