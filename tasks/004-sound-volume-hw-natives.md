---
id: 004
status: implemented
branch: card/004
depends: [001, 002, 003]
---
# natives: Sound.setVolume/getVolume/has3DHardware/hasMixHardware/enableDebugDump

## Goal
Replace the last 5 `java.sound.Sound` stubs in `src/script/natives.cpp`.
Static natives: `a[0]` is the first Java argument. Uses `audio::channel_volumes()`,
`audio::has_3d_hardware()`, `audio::has_mix_hardware()` (card 002) and
`audio::music_system()` (card 001). Depends on 003 only because it edits the
same section of the same file.

## Exe evidence (cleaned pseudocode, renamed fields)
`fx = DAT_005dada0`, `music = DAT_005dada4`, `engine = DAT_005dada8`
(= `channel_volumes().effects/.music/.engine`), `cur = DAT_005fe3b8`.

### 0x4e4b80 setVolume(int channel, float v)
```c
if (channel == 0) fx = v;
else if (channel == 1) { music = v; if (cur) set_volume(cur, v); /* 0x568ed0, result dropped */ }
else if (channel == 2) engine = v;
// other channels: nothing
```
Port: channel 1 → `channel_volumes().music = v; music_system().set_channel_volume(v);`
(`MusicSystem::change_set` reads `channel_volume()` as its `DAT_005dada4`; keep both
equal) then `if (music_system().current()) music_system().set_volume(v);`.
No clamping. Returns void (`Value::make_null()`).

### 0x4e4aa0 getVolume(int channel) → float
```c
switch (channel) { case 0: return fx; case 1: return music; case 2: return engine; default: return 0.0f; }
```
= `channel_volumes()[channel]` (const overload already returns 0 for others).

### 0x4e4af0 has3DHardware() → int : `jmp 0x546be0` → `return 3 < DAT_0079a108;` (0/1)
### 0x4e4b00 hasMixHardware() → int : `jmp 0x546bf0` → `return 3 < DAT_0079a0f0;` (0/1)
Return `Value::make_int(audio::has_3d_hardware() ? 1 : 0)` etc. Without a
DirectSound device (Linux, or init failed) the caps are 0 → both return 0,
which is what the exe returns before/without a successful init.

### 0x4e4a10 enableDebugDump(ResourceRef display) → int
```c
Native__getArgs(args, 0, &display);  // argument read and ignored
return 0;
```
Return `Value::make_int(0)`; accept any argument (including null).

## Files
- edit: `src/script/natives.cpp`
  - add `#include "audio/audio.h"` (music.h was added by card 003).
  - add the 5 functions to the `// ---- java.sound.Sound ----` section from card 003
    (`grep -n 'java.sound.Sound' src/script/natives.cpp`), same signature pattern,
    using `want`, `get_i`, `get_f`.
  - change the 5 registry rows from `nullptr, true` to `<fn>, false`.

## Done when
- each native has a `// 0xADDR Sound.name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
