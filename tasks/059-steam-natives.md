---
id: 059
status: implemented
branch: card/059
depends: []
---
# java.util.Steam: initAPI / neutralize / getPackage / setAchievement / resetAchievements

## Goal
Turn the five stubbed `java.util.Steam` natives into working handlers. The
engine has **no Steam service**: four of them are thin wrappers around
steam_api.dll (init, shutdown, SetAchievement+StoreStats, ResetAllStats), so
they become their exe failure path (defined values below), and `getPackage` is
a pure DLC-id -> directory mapping that needs no Steam call at all. No car or
VM state is read or written.

## Exe evidence (cleaned pseudocode, resolved constants)

Argument binding: the exe's `CallInfo` unpacker 0x4b1af0 (see card 058) hands
the natives their arguments in declared order; our `Vm` already does this, so
it is not ported. The exe logs failures through 0x569320 (append to
DAT_007a2838) + 0x5695c0 (flush); the message includes the script file
(`*(*CallInfo + 0x74)`) and line (`*(*CallInfo + 0x70)`, formatted by 0x528030
with `DAT_005a6828`), which our `Vm::NativeFn` cannot see.

### 0x4e4c80 Steam.initAPI()I
```
c = steam_init();                    // 0x564af0
if (c == 0)
    log("vm_steam_init_api: Failed to initialize Steam API in file \"<file>\" at line <line>");
return c != 0;                       // 1 on success, 0 on the failure path
```
### 0x4e4d40 Steam.neutralize()V  (Claude gate: raw bytes `c6 05 df 75 7a 00 01 c3`)
```
*(uint8_t*)0x7a75df = 1;      // global "neutralized" flag; return void
```
Readers (not traced, car/part code): 0x49446b, 0x494646, 0x49a4f8. Port: a
file-scope `bool steam_neutralized = false;` in natives.cpp set to true, with a
comment naming 0x7a75df and those readers. Nothing in src reads it yet.
### 0x4e4c10 Steam.getPackage(I)Ljava.lang.String;
```
id = arg0;
s = package_dir(id);                 // 0x564ac0, never returns NULL
if (s != NULL) {                     // always
    DAT_007a75e4 = 1;                // Steam mesh-version remap flag (src: ParseOptions)
    if (steam_apps_check(id) != 0)   // 0x564b10, result unused for the value
        s = package_dir(id);         // same pure mapping again
}
push_java_string(s);                 // 0x55b980; our port returns Value::make_str(s)
```
`0x564ac0 package_dir(int id)`:
```
if (id < 0)          return "Error! Invalid Steam package ID!";
if (id == 0x7d5dc)   return "dlc\\redline_cars_remake_pack\\";   // 0x7d5dc = 514524
return "Invalid DLC ID!";
```
### 0x4e4e00 Steam.setAchievement(Ljava.lang.String;)V
```
name = arg0;
ok = steam_set_achievement(name);    // 0x564b50
if (ok == 0)
    log("vm_steam_set_achievement: Failed to set Steam achievement in file \"<file>\" at line <line>");
return;                              // void
```
### 0x4e4d50 Steam.resetAchievements()V
```
ok = steam_reset_achievements();     // 0x564b30 (Ghidra types it void, caller reads a char)
if (ok == 0)
    log("vm_steam_reset_achievements: Failed to reset Steam achievements in file \"<file>\" at line <line>");
return;                              // void
```

### Steam helpers (for the absent-service values only; not ported)
```
0x564af0 steam_init()       = (SteamInternal_SteamAPI_Init("SteamUtils010", 0) == 0)
0x564b30 steam_reset()      = UserStats(STEAMUSERSTATS_INTERFACE_VERSION013)->vtable[0x50](1)
                              // ResetAllStats(true); raw: returns void, caller tests it
0x564b50 steam_set_ach(n)   = UserStats()->vtable[0x18](n)   // SetAchievement
                              then UserStats()->vtable[0x24]() // StoreStats
                              returns the SetAchievement result
0x564b10 steam_apps_check(i)= SteamApps(STEAMAPPS_INTERFACE_VERSION008)->vtable[0x4c](i)
0x55b980 push_java_string(s)= creates/interns a java.lang.String from the char*
```

## Service gap and absent-service results
`src/` has no Steam service (no steam_api.dll linkage, no `java.util.Steam`
backing object). Replace each call with its exe **failure** result; the
replacement is `INVENTED:` as a whole.

| native | exe call | result when the call fails (the only case in this engine) |
|---|---|---|
| `initAPI()I` | 0x564af0 | return **0** and write the init-failure log line |
| `neutralize()V` | none (pure flag write) | set `steam_neutralized = true`, void |
| `getPackage(I)Str` | pure 0x564ac0 | still returns the mapping below; no Steam call needed |
| `setAchievement(String)V` | 0x564b50 | void; write the set-achievement failure log line |
| `resetAchievements()V` | 0x564b30 | void; write the reset-achievements failure log line |

Logging: use `std::fprintf(stderr, "%s\n", message)` (same channel as
`system_log`, 0x4e5700). `INVENTED:` the exe's ` in file "<file>" at line <n>`
suffix is dropped because `Vm::NativeFn` has no call-site info; the message
prefixes `DAT_005a5ca4` / `DAT_005a5fa0` / `DAT_005a609c` are not modelled.
Do **not** write to `error.log` and do not call `std::exit`.

## Files
- edit: src/script/natives.cpp — add `steam_init_api`, `steam_neutralize`,
  `steam_get_package`, `steam_set_achievement`, `steam_reset_achievements` and
  the `steam_failure` log helper near `system_log` (0x4e5700, ~line 507); flip
  the five registry rows at 4807-4811 from `nullptr, true` to `<handler>, false`.
- read only: src/mesh/scx.h (`ParseOptions::steam_version_remap` is already
  `true`, matching the exe's `DAT_007a75e4 = 1`; no new field or setter).

## Implementation notes
- `getPackage`: `want("Steam.getPackage", a, 1)` then `get_i`; on a
  non-int arg return false like the other handlers. Return exactly the strings
  above; `"dlc\\redline_cars_remake_pack\\"` is the C++ literal for the runtime
  value `dlc\redline_cars_remake_pack\` (keep the trailing backslash).
  `0x7d5dc` = 514524. The exe's `DAT_007a75e4 = 1` needs no code (see Files).
- `setAchievement`: validate one String arg; it is not used further.
- void handlers set `*out = Value::make_null()`; `initAPI` sets
  `Value::make_int(0)`.
- Each handler/helper gets a `// 0xADDR Name` comment; the no-service
  replacement gets a one-line `INVENTED:` comment.
- Keep the exact class/method/sig strings of the existing rows.

## UNRESOLVED
- **Log prefix literals** `DAT_005a5ca4`, `DAT_005a5fa0`, `DAT_005a609c` (first
  0x569320 append before the message) were not read from the image; not needed
  for the stderr replacement.
- **0x564b10** `SteamApps->vtable[0x4c](id)` result: it only gates a second call
  to the same pure mapping 0x564ac0, so the returned String is unaffected. Not
  needed for the port.
- **init semantics if a real Steam service is ever added:** helper raw line
  `iVar1 = SteamInternal_SteamAPI_Init("SteamUtils010",0); return iVar1 == 0;`
  while the caller treats 0 as failure. The absent-service port returns 0
  either way; do not "fix" it now.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
