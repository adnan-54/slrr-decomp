---
id: 200
status: todo
branch:
depends: [196]
---
# cfg scan: exe %d / %x integer readers for osd_* ids/colours and type_ground params

## Goal
Replace the `strtoul(..., 0)` stand-in in `fill_osd` (src/game/car.cpp:261 and :279, marked
`INVENTED: 0x569380 integer conversion not traced`) with the exe's own `%d` reader of
Str__vscan 0x527140, and make src/game/runtime.cpp `scan_hex` require the `0x` prefix like the exe
hex reader 0x527490. Every shipped `osd_*` value is `0x%08X`, so results on the corpus do not change;
this removes the INVENTED marker and matches edge cases (octal, '+', "0x" alone, failed id).
RE: notes/22 §1.

## Exe evidence (cleaned)
### 0x527140 `%d` / `%u` / `%x` dispatch
```
%d, %u:  skip ws (9..13, ' ');  if in[0]=='0' && (in[1]=='x'||in[1]=='X'): ok = hex(&in, out)
                                else ok = dec(&in, unsigned = (conv=='u'), out)
%x:      skip ws; ok = hex(&in, out)
if !ok: return count            // the scan stops; later fields keep their old values
count++
```
### 0x527490 hex(&p, out)
```
if !(p[0]=='0' && (p[1]=='x'||p[1]=='X')): return 0       // no prefix -> fail
p += 2
if !(isdigit(*p) || 'a'<=*p<='f' || 'A'<=*p<='F'): return 0   // "0x" alone -> fail
v = 0 (uint32, wraps)
while hex digit: v = v*16 + digit; p++
*out = v; advance cursor to p; return 1
```
### 0x527420 dec(&p, unsigned, out)
```
neg = (*p == '-'); if neg: p++                             // '+' is not accepted
if !('0' <= *p <= '9'): return 0
v = 0 (int32, wraps; leading '0' stays decimal)
while '0' <= *p <= '9': v = v*10 + (*p - '0'); p++
*out = unsigned ? v : (neg ? -v : v); advance cursor; return 1
```
### osd_* handlers (notes/21 §2): format `"%d %f %f %f ..."`, ref set only when the scan count > 0
(osd_gear: > 1); colour is the `%d` after pos.

## Port
- src/game/car.cpp: add `static bool vscan_int(const std::string &tok, int32_t *out, bool *whole)`
  implementing the dispatch above for `%d` (ws skip, `0x` -> hex, else dec); `*whole` = the reader
  consumed the whole token. In `fill_osd`:
  - id: if `!vscan_int(...)` -> `return` (count 0: no ref, nothing else changes). Keep the gear rule
    (ref only when the scan got past the id: next token exists and parses). If `!whole`, the exe's
    next conversion sees the junk and fails: set the id/ref as now, then `return`.
  - colour: same; on failure `return` before touching the colour or later fields.
  - delete the `INVENTED: 0x569380 ...` comment, add `// 0x527140 %d -> 0x527490 / 0x527420`.
- src/game/runtime.cpp `scan_hex` (line 81): after ws skip, return false unless `s[p]=='0'` and
  `s[p+1]` is `x`/`X` (0x527490); keep the rest. Update its comment ("0x prefix required").

## Files
- edit: src/game/car.cpp (fill_osd + helper), src/game/runtime.cpp (scan_hex)

## Done when
- `// 0x527490` / `// 0x527420` comments on the ported readers; no INVENTED left for the int scan
- `make -C src clean check` green (cfg corpus + vmboot unchanged)
- no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
