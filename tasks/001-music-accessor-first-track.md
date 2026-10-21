---
id: 001
status: todo
branch:
depends: []
---
# audio: process-wide MusicSystem + firstTrack (0x568b10)

## Goal
The `java.sound.Sound` natives need one music state machine shared by the whole
process (the exe keeps the six sets and the current-set pointer `DAT_005fe3b8`
as globals). Add a global accessor for `audio::MusicSystem` and the one missing
set command, `firstTrack` (0x568b10). Everything else of the music state machine
already exists in `src/audio/music.*` — do not change it.

## Exe evidence (cleaned pseudocode, renamed fields)
Set fields: `+0x08 track`, `+0x14 mode`, `+0x18 prev_mode` (= `MusicSet::track/mode/prev_mode`).
`FUN_005473b0` = `MusicSystem::stop_stream()`, `FUN_00568f40` = `MusicSystem::start_set(set)`.

### 0x568b10 MusicSet::first (ECX = current set)
```c
int first(MusicSet *set) {
    stop_stream();                 // FUN_005473b0
    if (set->mode == 5)
        set->mode = set->prev_mode;
    set->track = 0;
    start_set(*set);               // FUN_00568f40 (result ignored)
    return set->track;
}
```
The native (0x4e4a30) only calls it when a set is current: `if (DAT_005fe3b8) jmp 0x568b10`.

### Globals
`DAT_005fe3b8` (current set) and the six sets are static in the exe: one instance
for the whole process.

## Files
- edit: `src/audio/music.h` — add to `struct MusicSystem` (public, next to `next()`):
  `int first();   // 0x568b10` ; after the struct, in `namespace audio`, declare
  `MusicSystem &music_system();  // process-wide sets + DAT_005fe3b8`.
- edit: `src/audio/music.cpp` — implement `MusicSystem::first()` exactly like
  `MusicSystem::next()` but with `set->track = 0;` instead of `set->track++`
  (returns -1 when no set is current, like the other commands); implement
  `MusicSystem &music_system() { static MusicSystem m; return m; }`.

## Done when
- `first()` carries a `// 0x568b10` comment; the accessor a one-line comment
  citing `DAT_005fe3b8`.
- `make -C src clean check` green; no new unit tests or hand-made expected values.
- card frontmatter: status implemented, branch name.
