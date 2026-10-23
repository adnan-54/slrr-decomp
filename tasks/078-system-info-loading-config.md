---
id: 078
status: merged
branch: card/078
depends: []
---
# script: System.info / isLoading / isLoadingReset / getConfigOptions

## Goal
Replace the four remaining `java.lang.System` stubs (card 056 left them out). `info(I)String`
becomes a real decimal-string native. `getConfigOptions()V`, `isLoadingReset()V` and
`isLoading()I` are engine hooks into the resource streamer and the gameplay-config store;
src has neither service, so they become explicit no-ops / `0` with comments naming the exe
addresses and the exact exe behaviour they stand in for. No consumer exists in src.

## Exe evidence (Claude, decompile + raw bytes)
Our `vm` argument is the exe's JavaMachine object: in every branch below `[CallInfo+0]` is
fed to `JVM__newString` (0x55b980) as `this`, and the 0x553980 constructor is the
JavaMachine ctor. `getArgs` = `FUN_004b1af0`; `%d` formatter = `FUN_00528030`.

### 0x4e55c0 System.getConfigOptions()V
Raw bytes `b9 90 de 5f 00 e9 76 6c f9 ff`:
```
ecx = 0x5fde90;            // static engine/config object
jmp 0x47c240;              // Config__loadGameplaySettings (void, no args)
```
`0x47c240` (evidence: decompiled) reloads gameplay/graphics settings through
`FUN_0047c110(key)` (int), `FUN_0047c080(key)` (float) and `FUN_0047cff0(key, default)`
(has-key check) into ~40 globals:
`headlight_rays, flares, shadow_size, shadows, shadow_detail, texture_size, object_detail,
object_detail_amp, texture_format, video_gamma (1/x), particle_density, skidmark_max,
texture_save_quality (*100, int), external_damage, internal_damage, deformation,
mem_vertex_max/min, mem_texture_max/min, mem_instance_max/min, mem_sound_max/min (only when
the key exists), resource_loadrate (-> 0x5db02c), ForceFeedBack, FFB_strength (*10000),
FFB_strength_emulated (*1e-6), engine_inertia_factor, wheel_gndfeedback_factor,
wheel_brake_factor (each only when the key exists), MouseHelp, steerhelp_turn,
esp_steer_scale, esp_angvel_scale, esp_yaw_allowed, esp_brake_level, esp_power_level,
head_move_steer/vel/acc (only when the key exists), leaveGearOnRelease`; finally
`FUN_00422230()` re-creates two gfx resources sized by `shadow_size`.
No ConfigData store and none of those globals exist in src. Exe return value: void.

### 0x4e5690 System.isLoading()I
Raw bytes `a1 08 b0 5d 00 8b 80 54 d8 16 00 c3`:
```
return *(int *)(*(int **)0x5db008 + 0x16d854);
```
`0x5db008` is the resource-manager pointer (notes/18, card 069). `+0x16d854` is the
priority/loading flag for resource type 0; its only readers are this native and the flush
loop 0x5176a0 (which sets the neighbouring flags +0x16d858/5c/60/64, not +0x16d854).
src's ResourceManager loads synchronously and has no such flag. Exe value with no
streamer: int 0 (BSS-initialised; nothing in src can set it).

### 0x4e56a0 System.isLoadingReset()V
Raw bytes `8b 0d 08 b0 5d 00 e9 05 3b 03 00`:
```
ecx = *(int **)0x5db008;   // resource manager
jmp 0x5191b0;              // ecx is unused by the target
```
`0x5191b0` (decoded from bytes `a1 2cb05d00` + 32×`a3 <off>3a7700` + `c1 e0 05` +
`a3 1c3a7700` + `c3`):
```
int rate = *(int *)0x5db02c;                 // config "resource_loadrate" (set at 0x47c411)
for (i = 0; i < 32; i++) *(int *)(0x773a20 + 4*i) = rate;   // per-type load budgets
*(int *)0x773a1c = rate << 5;                // total budget
```
It resets the streamer's per-frame load budget; it does not clear `+0x16d854`. No
streamer in src, so no-op. Exe return value: void.

### 0x4e55d0 System.info(I)Ljava.lang.String;
```
int n; getArgs(args, 0, &n);              // missing arg -> uninitialised n in the exe
switch (n) {
case 0: v = JVM_list_count(vm + 0x18);  break;  // T_List<Thread>, ctor 0x553980
case 1: v = JVM_list_count(vm + 0x1c);  break;  // T_List<Thread>
case 2: v = JVM_list_count(vm + 0x10);  break;  // T_List<Instance> (string intern list)
case 3: v = *(int *)(vm + 0x24);        break;
case 4: v = *(int *)(vm + 0x28);        break;
case 5: v = *(int *)0x7a383c;           break;  // == 0
default: goto out;                              // jumps to JVM__newString with an
}                                               // UNINITIALISED stack buffer
snprintf(buf, 0x100, "%d", v);                  // 0x528030 -> 0x5277d0
out:
return JVM__newString(buf);                     // 0x55b980
```
- `JVM_list_count` 0x4d8760: `n = 0; for (node = *(void **)(list + 8); node; node =
  *(void **)(node + 4)) n++;` (T_List's head at +8, next at +4).
- All six format strings are `"%d"` (read at 0x5a4078, 0x5a4098, 0x5a40bc, 0x5a40e4,
  0x5a4150, 0x5a4190).
- VM layout from the JavaMachine ctor 0x553980: `+0x10 = T_List<Instance>`, `+0x18` and
  `+0x1c = T_List<Thread>`, `+0x24 = 0`, `+0x28 = 0` (both also reset to 0 there).
- `*(int *)0x7a383c` is 0 in the file image; no writer was traced.
- The `switch` is unsigned (`ja`), so negative arguments also take the default branch.
- No shipped script calls `System.info` (grep of the `bin/**/*.java` sources), so the two
  missing services are unobservable at script level.

## Files
- edit: `src/script/natives.cpp`

## Implementation
Add the four functions next to the other `java.lang.System` natives (`system_mem_usage`,
around line 860) and replace the four `nullptr, true` registry rows at ~5795/5800-5802.

1. `system_get_config_options` (0x4e55c0): `*out = Value::make_null(); return true;` with
   `// shortcut: no ConfigData store / renderer settings service in src; exe reloads
   gameplay settings into engine globals at 0x47c240.`
2. `system_is_loading` (0x4e5690): `*out = Value::make_int(0);` with the raw byte
   sequence and `// shortcut: no async resource loader in src; the exe reads the
   per-type loading flag at [[0x5db008]+0x16d854], 0 until the streamer sets it.`
3. `system_is_loading_reset` (0x4e56a0): `*out = Value::make_null(); return true;` with a
   comment giving the 0x5191b0 loop above and `// shortcut: ...`.
4. `system_info` (0x4e55d0): `want("System.info", a, 1, err)` + `get_i`; for n in 0..5
   produce 0 through `snprintf(buf, sizeof buf, "%d", (int)0)` + `Value::make_str(buf)`
   (keep the `%d` shape); n outside 0..5 -> `Value::make_str("")`.
   Comments per case: cases 0-2 `// shortcut: no <list> service in src; exe counts
   T_List<Thread>/<Instance> at VM+0x18/+0x1c/+0x10`; cases 3-4
   `// UNRESOLVED: VM+0x24 / VM+0x28 writers not traced (ctor 0x553980 only zeroes them)
   — exe raw: uVar1 = *(undefined4 *)(*param_1 + 0x24);` (and +0x28); case 5
   `// 0x7a383c is 0 in the image`; default `// INVENTED: exe builds a String from an
   uninitialised stack buffer; return "".`

## Done when
- each ported function has a `// 0xADDR System.name` comment; untraced logic is `INVENTED:` /
  `UNRESOLVED:` as above; no engine behaviour "improved"
- `make -C src clean check` green; no new unit tests or hand-made expected values (there
  is no shipped corpus fact that exercises these natives)
- card frontmatter: status implemented, branch name

## Gate note (Claude)
Accepted as service-less. Follow-up: src does track script threads (Scheduler) and instances, so info(0..2) could return real counts once the exact lists at VM+0x10/+0x18/+0x1c are mapped; not in this card.
