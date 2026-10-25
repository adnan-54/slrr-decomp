---
id: 203
status: todo
branch:
depends: []
---
# GameLogic.kismajomCheck: cheat words against the lastKey ring

## Goal
Replace the explicit stub `java.game.GameLogic.kismajomCheck([Ljava.lang.String;)I`
(src/script/natives.cpp registry row, `nullptr, true`). Caller: sl/Scripts/game/Garage.java:1926
(`Input.flushKeys(); int code = GameLogic.kismajomCheck(GameLogic.kismajom);` with the five
words of GameLogic.java:269-274, each char stored +1). The exe compares the words against the
16-byte ring of recently typed ANSI keys that `Input.lastKey` 0x4de9f0 fills; src already has
that ring: `input::hotkeys().last_ring[16]` / `last_pos` (src/input/hotkeys.h, filled by
`HotkeyTable::last_key`). RE: notes/23 §1.

## Exe evidence (cleaned)
`PTR_DAT_005dadb8` = ring write cursor (initial 0x5fe3c0 = &ring[0]); ring = 0x5fe3c0..0x5fe3cf.
In src: `cursor = last_pos`, `ring = last_ring`.
### 0x4debd0 GameLogic.kismajomCheck(String[] words)
```
n = intField(words, "length")          // 0x4ece80; null/missing -> 0
loop {
  if (n == 0) return -1;
  n = n - 1;
  s = (char*)stringOf(words[n])         // 0x4ecac0 + 0x4ecc70
  len = strlen(s);
  p = cursor;                           // pointer into ring
  k = len;
  do {
    i = k;
    if (i == 0) break;                  // every char matched
    c = s[--len_pos];                   // walks s from its LAST char backwards
    p = (p - 1 >= &ring[0]) ? p - 1 : p + 15;   // step back, wrap to ring[15]
    k = i - 1;
  } while ((int)(signed char)c - 1 == (int)(signed char)*p);
  if (i - 1 < 0) {                      // i == 0: full match
    q = (cursor - 1 >= &ring[0]) ? cursor - 1 : cursor + 15;
    *q = 0;                             // clear the newest ring byte (fires once)
    return n;
  }
}
```
Notes: words are tried from the LAST index down; the ring is walked newest-first; an empty
word matches immediately (returns its index and still clears the newest byte); a word longer
than 16 wraps around the ring (compare keeps going, same formula).

## Files
- edit: src/script/natives.cpp: add `gl_kismajom_check` near the Input natives (~line 5571,
  `in_last_key`), registry row `kismajomCheck` -> `gl_kismajom_check, false`.
- read only: src/input/hotkeys.h (`last_ring`, `last_pos`), src/input/device.h (`input::hotkeys()`).

Port sketch (static native, args a[0] = the array):
```cpp
// 0x4debd0 GameLogic.kismajomCheck
if (!want("GameLogic.kismajomCheck", a, 1, err)) return false;
input::HotkeyTable &h = input::hotkeys();
int n = (a[0].kind == Value::Array && a[0].arr) ? (int)a[0].arr->elems.size() : 0;
while (n != 0) {
    n--;
    const Value &e = a[0].arr->elems[(size_t)n];
    // INVENTED: the exe dereferences a non-String/null element (strlen on null); treat as no match.
    if (e.kind != Value::Str) continue;
    int p = h.last_pos, len = (int)e.s.size(), i = len;
    bool ok = true;
    for (; i > 0; i--) {
        p = p - 1 >= 0 ? p - 1 : p + 15;
        if ((int)(signed char)e.s[(size_t)(i - 1)] - 1 != (int)(signed char)h.last_ring[p]) { ok = false; break; }
    }
    if (ok) {
        int q = h.last_pos - 1 >= 0 ? h.last_pos - 1 : h.last_pos + 15;
        h.last_ring[q] = 0;
        *out = Value::make_int(n);
        return true;
    }
}
*out = Value::make_int(-1);
return true;
```

## Done when
- `// 0x4debd0` comment; non-exe logic marked `INVENTED:`
- `make -C src clean check` green (natives check: stub count drops by 1); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
