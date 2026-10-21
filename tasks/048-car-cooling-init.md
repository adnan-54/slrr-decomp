---
id: 048
status: implemented
branch: card/048
depends: [047]
---
# vehicle: initial engine heat capacity / cooling coefficient (replace INVENTED 0)

Status: **draft — needs Claude review.** No `src/` change is included in this card
until the review confirms the two conclusions below.

## Goal
`CarData::heat_capacity` (car+0x1ebc) and `CarData::cooling_coeff` (car+0x1ec8)
currently start at `0.0f`, marked `INVENTED initial 0` (card 047). Replace those
starts with the constants `Vehicle__createCar 0x4992e0` actually writes, so a
vehicle has the exe's initial thermal state before its first
`Vehicle__refreshParameters` (0x4aebf0) / `Vehicle__update` (0x49c8e0) tick.

The four functions named in the task were decompiled with
`procedure_pseudo_code`. Their address names come from `analysis/names.json`:
`0x4992e0 Vehicle__createCar`, `0x49c8e0 Vehicle__update`,
`0x49ab20 type_car__vf1`; `0x48f020` has no name in the map.

## Finding summary (read this before the quotes)
1. **`0x4992e0 Vehicle__createCar` is the real writer of the car's four thermal
   starts.** It allocates the 0x2284-byte `Part`/`Vehicle` instance and writes
   `+0x1ebc = 320000.0f`, `+0x1ec0 = 20.0f`, `+0x1ec4 = 20.0f`,
   `+0x1ec8 = 9.375e-05f`. This is the evidence for card 048.
2. **`0x49ab20 type_car__vf1` is not a `Vehicle` ctor.** It is the vehicle-manager/
   car-type default initialiser (`M+0x2ebc` default engine block is set here; notes/09
   §"Engine-block template writers"). Its `in_ECX+0x1ec4/0x1ec8/0x1ecc` writes are
   part of a 4×0x48-byte repeated block in the *manager* object, not `CarData`
   fields. Do **not** copy `-2.4` into `cooling_coeff`. The byte-search hit for
   displacement 0x1ec8 at 0x49b181 is therefore a same-offset different-class hit.
3. **`0x48f020` (unnamed)** parses one text record and writes
   `param_2+0x1ec4/+0x1ec8/+0x1ecc/+0x1ef0`. Which class `param_2` is (Vehicle or
   the manager/type object) is **not established** from the four addresses
   available here — it shares the offset cluster with (2), so it is plausibly the
   same type object, but that is a guess. It does not change the `createCar`
   constants, which are direct and unconditional.
4. **`0x49c8e0 Vehicle__update`** is the per-tick consumer/overwriter, not an
   initialiser: it adds heat to `+0x1ec0` using `+0x1ebc` as divisor and
   recomputes `+0x1ec8 = cooling-rate / +0x1ebc` every engine-on tick. Its
   `+0x1ec8` initial value still matters for ticks skipped by the `+0x70 & 0x400`
   no-thermal flag, so the `createCar` start is not dead data.

## Exe evidence (verbatim Ghidra pseudocode; constants in hex and decimal)

### 0x4992e0 `Vehicle__createCar` (entry signature + the thermal block)
```
uint * FUN_004992e0(int param_1,int *param_2)
...
  local_14 = (uint *)FUN_00567a20(0x2284);          // 0x2284-byte Part/Vehicle instance
...
    uVar7 = FUN_00490100();                         // Vehicle__ctor
...
  puVar8 = (uint *)FUN_004c1f90(param_1,param_2,uVar7);   // Part__initInstance
...
  puVar8[0x546] = 0;
  puVar8[0x7b1] = 0x41a00000;
  puVar8[0x7b0] = 0x41a00000;
  puVar8[0x7af] = 0x489c4000;
  puVar8[0x7b2] = 0x38c49ba6;
  puVar8[0x7a7] = 0;
```
`puVar8` is declared `uint *` (4-byte stride), so:
| pseudocode | car offset | hex | decimal |
|---|---:|---|---:|
| `puVar8[0x7af] = 0x489c4000` | **+0x1ebc** heat_capacity | `0x489c4000` | **320000.0** |
| `puVar8[0x7b0] = 0x41a00000` | +0x1ec0 engine_temp | `0x41a00000` | **20.0** |
| `puVar8[0x7b1] = 0x41a00000` | +0x1ec4 ambient | `0x41a00000` | **20.0** |
| `puVar8[0x7b2] = 0x38c49ba6` | **+0x1ec8** cooling_coeff | `0x38c49ba6` | **9.375e-05** |
| `puVar8[0x7a7] = 0` | +0x1e9c engine_damage | `0` | 0.0 |

No condition guards these stores: they are straight-line code in `createCar`,
after the instance is constructed. `+0x1ecc` is **not** written in this block
(the next store is `[0x7a7]`). Consistency check: 30 / 320000 = 9.375e-05, so
the cooling start is the same "cooling rate / heat capacity" form that
`Vehicle__update` uses at line 284 below.

### 0x49ab20 `type_car__vf1` (manager default init; same displacement, different object)
Straight-line constructor code, no guard. The block below is the 4th repetition
of a 4-element pattern at stride 0x48; the first three are identical at
`+0x1dec/+0x1df0/+0x1df4`, `+0x1e34/…`, `+0x1e7c/…`:
```
  *(undefined4 *)(in_ECX + 0x1edc) = 5;
  *(undefined4 *)(in_ECX + 0x1ef0) = 0xffff0000;
  *(undefined4 *)(in_ECX + 0x1ec4) = 0x404f5c29;
  *(undefined4 *)(in_ECX + 0x1ec8) = 0xc019999a;
  *(undefined4 *)(in_ECX + 0x1ecc) = 0xc0651eb8;
  FUN_00569b20();
```
- `+0x1ec4 = 0x404f5c29` = **3.24**
- `+0x1ec8 = 0xc019999a` = **-2.4**
- `+0x1ecc = 0xc0651eb8` = **-3.58** (not -14.32: exponent field is 0x80)
- `+0x1ef0 = 0xffff0000` and `+0x1edc = 5` belong to the same repeated record.

This function is the car-type manager vtable slot 1 (notes/09 §"Engine-block
template writers": it initialises the manager's default engine block at
`M+0x2ebc`, allocates it with `Mem__alloc(0xc34)` / `FUN_004904c0`, and also
seeds `M+0x1fb4` wheel count, `M+0x2c38`, etc.). The `+0x1ec4/8/c` triple is a
3-float field of its 4×0x48 record array; it is not car thermal state.

### 0x48f020 (unnamed text-record parser)
Full decompilation:
```
void FUN_0048f020(char *param_1,int param_2)

{
  char cVar1;
  int iVar2;
  undefined4 uVar3;
  undefined4 uVar4;
  undefined4 uVar5;
  
  iVar2 = param_2;
  if (*(int *)(param_2 + 0x120) == 0) {
    param_2 = 0;
    do {
      cVar1 = *param_1;
      param_1 = param_1 + 1;
    } while (cVar1 != '\0');
    iVar2 = FUN_00569380(param_1,"%d %f %f %f %d %8s %f %f %f %f",&param_2,iVar2 + 0x1ec4,
                         iVar2 + 0x1ec8,iVar2 + 0x1ecc,iVar2 + 0x1ef0);
    if (0 < iVar2) {
      uVar5 = 0;
      uVar4 = 0xe;
      uVar3 = FUN_0051e860(param_2);
      FUN_0051fa40(uVar3,uVar4,uVar5);
    }
  }
  return;
}
```
- Guard: `*(int *)(param_2 + 0x120) == 0` (record not loaded yet).
- `FUN_00569380` is the text scan helper used by the car-type cfg parser
  (notes/09 §"Car cfg parser"; compare helper `0x57c11e`).
- Writes visible to the decompiler: first `%f` → `+0x1ec4`, second `%f` →
  `+0x1ec8`, third `%f` → `+0x1ecc`, fifth conversion (`%d`) → `+0x1ef0`.
  The `%8s` and the last four `%f` arguments are not shown (callee variadic
  signature unknown), so their destinations are **not** claimed here.
- On ≥1 converted field the parsed first `%d` is used as a resource id and
  requested with `FUN_0051fa40(id, 0xe, 0)`.
- Unresolved: the string pointer is advanced past the terminating NUL before
  the call; either the decompiler mis-rendered `FUN_00569380`'s arguments or the
  helper does not take the input string in arg 1. **Not guessed.**

### 0x49c8e0 `Vehicle__update` (per-tick consumer; dt = param_2, car = param_1)
```
  263      if ((*(uint *)(param_1 + 0x70) & 0x400) == 0) {
  264        if (0.0 < fVar20 * param_2) {
  265          *(float *)(param_1 + 0x1ec0) =
  266               (fVar20 * param_2) / *(float *)(param_1 + 0x1ebc) + *(float *)(param_1 + 0x1ec0);
  267        }
  268        if ((*(float *)(param_1 + 0x1ec0) <= 90.0) || (1.0 <= *(float *)(param_1 + 0x1eac))) {
  269          fVar18 = *(float *)(iVar15 + 0xa5c);
  270        }
  271        else {
  ...          // radiator-damage/health path: c = health·(|v|²·e+0xa64 + e+0xa60) + e+0xa5c
  283        }
  284        *(float *)(param_1 + 0x1ec8) = fVar18 / *(float *)(param_1 + 0x1ebc);
  285        if (100.0 < *(float *)(param_1 + 0x1ec0)) {
  286          fVar18 = *(float *)(param_1 + 0x1ec0) - 100.0;
  287          FUN_004afb50(fVar18 * fVar18 * param_2 * DAT_005dacd4);
  288        }
  ...
  326      }
  ...
  328    fVar18 = *(float *)(param_1 + 0x1ec0) - *(float *)(param_1 + 0x1ec4);
  329    *(float *)(param_1 + 0x1ec0) =
  330         *(float *)(param_1 + 0x1ec0) -
  331         ABS(fVar18) * *(float *)(param_1 + 0x1ec8) * param_2 * fVar18;
```
- `+0x1ebc` is only read here, as the heat-capacity divisor.
- `+0x1ec0` (temp) is increased by `heat·dt / +0x1ebc` only when
  `(flags & 0x400) == 0` and `heat·dt > 0` (`fVar20` is the combustion heat,
  notes/09 §1 "Thermal").
- `+0x1ec8` is recomputed each engine-on tick as `c / +0x1ebc`, `c` from
  `e+0xa5c` (60-a60-a64 cooling config) with the radiator-damage path; then the
  Newton cooling at 328..331 uses `+0x1ec8` and ambient `+0x1ec4`.
- Consequence for card 048: the `createCar` start of `+0x1ec8` is the value in
  effect for any tick that skips the `0x400` branch, so 9.375e-05 is the correct
  start, not 0.

## Changes
1. `src/game/car.h`, `struct CarData` (engine/cooling fields):
   ```cpp
   float engine_temp = 20.0f;             // +0x1ec0 (unchanged; 0x4992e0 writes 0x41a00000)
   float engine_temp_ambient = 20.0f;     // +0x1ec4  0x4992e0 writes 0x41a00000
   float heat_capacity = 320000.0f;       // +0x1ebc  0x4992e0 writes 0x489c4000
   float cooling_coeff = 9.375e-05f;      // +0x1ec8  0x4992e0 writes 0x38c49ba6
   ```
   (The existing `engine_temp = 20.0f` is confirmed, not INVENTED; add the
   missing ambient field so all four createCar stores are represented.)
2. `src/game/car.cpp`: no edit needed — `CarData::CarData() = default;`, the
   starts are the in-class initialisers.
3. Do **not** take `3.24 / -2.4 / -3.58` from `type_car__vf1 0x49ab20`; those are
   manager-record fields at the same displacements. Do not model `+0x1ecc` from
   card 048 (no `createCar` writer found; its meaning is not established).
4. Leave the `0x48f020` parser out of this card until a reviewer identifies
   `param_2`; it would only affect the same thermal fields through a text record,
   not the engine spawn defaults.

## Files
- edit: src/game/car.h
- src/game/car.cpp (no change required; verify `CarData::CarData() = default`)

## Done when
- `heat_capacity`/`cooling_coeff` no longer say `INVENTED initial 0`, citations
  point at `0x4992e0` hex constants.
- `make -C src clean check` green; no new unit tests or hand-made expected values.
- card frontmatter status changed to implemented after Claude review.
