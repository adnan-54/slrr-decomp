---
id: 003
status: todo
branch:
depends: [001]
---
# natives: java.sound.Sound music-track natives

## Goal
Replace 12 explicit stubs (`nullptr, true` rows) in `src/script/natives.cpp` with
real natives that drive `audio::music_system()` (card 001):
`changeMusicSet, nextTrack, prevTrack, firstTrack, randomTrack, playTrack(I),
playTrack(String), getTotalTracks, getNowPlaying, stopMusic, getMode, setMode`.
All are **static** natives: `a[0]` is the first Java argument (no receiver).

## Exe evidence (cleaned pseudocode, renamed fields)
`cur` = `DAT_005fe3b8` = `music_system().current()`. Native args come from
`Native__getArgs 0x4b1af0`. Set offsets: `+0x08 track`, `+0x0c count`, `+0x14 mode`.

### 0x4e48f0 changeMusicSet(int id)
```c
set = (id 0..5) ? &sets[id] : NULL;   // 0→0x5fe3d0 1→0x5fe418 2→0x5fe434 3→0x5fe3ec 4→0x602478 5→0x602494
if (set != cur) {
    if (cur) stop(cur);               // 0x569010
    cur = set;
    if (set) { start_set(set); set_volume(set, DAT_005dada4 /*music channel*/); }
}
```
= existing `MusicSystem::change_set(id)` (it uses `channel_volume()` for
`DAT_005dada4`; card 004 keeps that in sync). Returns void.

### tail jumps (return void to the script)
```
0x4e4b10 nextTrack()   : if (cur) 0x568c70  -> music_system().next()
0x4e4b20 prevTrack()   : if (cur) 0x568ca0  -> music_system().prev()
0x4e4a30 firstTrack()  : if (cur) 0x568b10  -> music_system().first()   (card 001)
0x4e4b30 randomTrack() : if (cur) 0x568cd0  -> music_system().random()
0x4e4c00 stopMusic()   : if (cur) 0x569090 = jmp 0x5473b0 (stop stream, set stays current)
                         -> music_system().stop()
```
Each `MusicSystem` method already returns -1/does nothing when no set is current.

### 0x4e49b0 playTrack(int i) / 0x4e49e0 playTrack(String s)
```c
if (cur) 0x568a50(i);   // music_system().play_track(int)
if (cur) 0x568a90(s);   // music_system().play_track(const std::string&)
```
A null String argument: INVENTED: treat as "" (the exe would pass a null char*).

### 0x4e4a40 getMode() : `return cur ? cur->mode : -1;`  -> `music_system().mode()`
### 0x4e4a60 getNowPlaying() : `if (cur) return cur->track;` (0x568b40)
### 0x4e4a80 getTotalTracks() : `if (cur) return cur->count;` (0x568b50)
With no current set both return an **uninitialised stack slot** in the exe.
Use `music_system().track()` / `.count()` (they return -1) and mark
`// INVENTED: exe returns an uninitialised value when no set is current`.

### 0x4e4b40 setMode(int m)
```c
if (!cur) return m;
if ((unsigned)m < 6) cur->mode = m;
return cur->mode;
```
= existing `MusicSystem::set_mode(m)`; return its result as int.

## Files
- edit: `src/script/natives.cpp`
  - add `#include "audio/music.h"` next to the other project includes.
  - add one small function per native in a new section
    `// ---- java.sound.Sound ----` placed just before the registry table
    (locate it with `grep -n 'java.sound.Sound' src/script/natives.cpp`).
    Signature pattern (see `math_sqrt` with `grep -n -A8 '^bool math_sqrt'`):
    `bool snd_next_track(Vm &, const Args &, Value *out, std::string *)`;
    use the existing helpers `want`, `get_i`, `get_s` for argument checks;
    void natives set `*out = Value::make_null();`, int natives `Value::make_int(...)`.
  - change the 12 registry rows from `nullptr, true` to `<fn>, false`.
  Do not read the whole file; use grep to find the spots.

## Done when
- each native function has a `// 0xADDR Sound.name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green (it includes the natives-vs-exe table check);
  no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
