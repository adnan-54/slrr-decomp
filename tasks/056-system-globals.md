---
id: 056
status: implemented
branch: card/056
depends: []
---
# script: System.exit/setMeasure/syncGameTime/setLdPriority globals

## Goal
Replace four `java.lang.System` engine stubs with the exe's behaviour. Each one
only writes engine globals; no service is needed. Store the globals as new fields
of `struct Scheduler` in `src/script/natives.cpp` (it already holds `warp`, the
0x5fde90+0x27c global), each field with a comment naming its exe address. Nothing
reads them yet; that is fine.
Out of scope (stay stubs): `info` 0x4e55d0, `getConfigOptions` 0x4e55c0
(-> 0x47c240 config reload), `isLoading` 0x4e5690 / `isLoadingReset` 0x4e56a0
(resource manager 0x5db008 +0x16d854 / 0x5191b0).

## Exe evidence (Claude, from decompile + raw bytes)
`Native__getArgs 0x4b1af0(args, 0, &x)` reads the first declared arg (our `a[0]`,
use the existing `get_f`/`get_i` helpers like `system_time_warp`).

### 0x4e5550 System.exit()V  (raw bytes `c7 05 74 0c 5f 00 01 00 00 00 c3`)
```
*(int*)0x5f0c74 = 1;     // quit request flag
```
-> field `int quit_request = 0;` set to 1. Return void (`Value::make_null()`).

### 0x4e57b0 System.setMeasure(F)V
```
float m = 1000.0;          // default if no arg
getArgs(args, 0, &m);
g_5da524 = m;                       // measure
g_5fe378 = m * 0.00027777778f;      // m / 3600   (const shown by decompiler)
g_5fe374 = 10.0f / m;
```
-> fields `measure` (init: leave 0 / INVENTED initial value not traced),
`measure_per_hour`, `measure_inv10`. Use the literal constants above.

### 0x4e5820 System.syncGameTime(F)V
```
float t; getArgs(args, 0, &t);
g_6024c0 = (int)t % 86400;           // 0x15180, C truncation + C remainder
if ((unsigned)(g_6024c0 - 3600) > 0x10b2f) || (g_5da080 = 0, (unsigned)(g_6024c0 - 18000) < 0x1c20))
    g_5da080 = 1;
```
i.e. `night = 0; if ((uint32_t)(s - 3600) > 68399u || (uint32_t)(s - 18000) < 7200u) night = 1;`
(note: when the first test is true the flag is set to 1 without the reset; same result).
-> fields `int day_seconds` (0x6024c0), `int time_flag_5da080`. Keep the
unsigned comparisons exactly (negative seconds wrap to large values).

### 0x4e56b0 System.setLdPriority(I)V
```
int p; getArgs(args, 0, &p);
if (p == 0) { g_5da464 = 0x3dcccccd /*0.1f*/; g_5fe3bc = 0; }
else        { g_5da464 = 0xbf800000 /*-1.0f*/; g_5fe3bc = 1; }
```
-> fields `float load_slice = 0` (0x5da464), `int load_priority = 0` (0x5fe3bc).

## Files
- edit: src/script/natives.cpp (struct Scheduler fields; four new native functions
  next to `system_time_warp`; replace the four `nullptr, true` rows of
  `java.lang.System` exit()V, setMeasure, syncGameTime, setLdPriority).

## Done when
- each ported function has a `// 0xADDR System.name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch card/056
