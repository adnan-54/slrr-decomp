---
id: 052
status: draft
branch:
depends: []
---
# car: SfxTable item natives, Vehicle.getHorn and getSpeedSquare

## Goal
Replace five engine stubs with the exe's behaviour: the three
`java.game.parts.SfxTable` item natives (`getItems`, `clear`, `addItem`),
`java.game.Vehicle.getHorn` and `java.game.Vehicle.getSpeedSquare`. The SfxTable
natives need the wrapper's raw table pointer (in the exe the `Native` handle
payload). Add that payload layer now (mirroring `bind_wheel`/`wheel_data`), but
note that **nothing produces a wrapper yet**: `Chassis.getSfxTable 0x4b3890` is
still an engine stub, so the resolver is always null. For that missing service
the exe's own null-handle values apply and must be returned:
`getItems -> 0`, `clear -> do nothing`, `addItem -> do nothing`.
`Vehicle.getHorn` reads the horn record, which is now owned by `CarData`
(`engine_sfx.horn_resource`); its only writer, `Chassis.setHornSFX 0x4b4f40`,
is still a stub, so it stays 0 — exactly the exe value for an unset horn (owner
word 0). `getSpeedSquare` needs no new service: our runtime already models
GetInfo id 3 as `Runtime::Resource::vel` (same source as `GameRef.getVel`
0x4db050, `src/script/natives.cpp:1263`).

## Exe evidence (cleaned pseudocode, renamed fields)

Common prologue: `Native__getArgs 0x4b1af0` writes the receiver, then the
declared args in order, to stack slots. `FUN_004ecc70(self, DAT_007a2620)`
resolves the script instance to its native handle payload (notes/06); its class
argument is not needed in our model, where the payload is looked up by wrapper
object. `FUN_0040cac0` is the ref-node bind (`Runtime::bind` equivalent, notes/14
§2.3).

### 0x4b5a90 SfxTable.getItems
```
h = instanceToHandle(self, DAT_007a2620);   // 0x4ecc70
if (h) return *(int *)(h + 0x240);          // item count
return 0;
```

### 0x4b5a60 SfxTable.clear
```
h = instanceToHandle(self, DAT_007a2620);
if (h) SfxTable__clear(h);                  // 0x498280, ECX = h
```
### 0x498280 SfxTable__clear(h) (helper; already implemented as `audio::SfxTable::clear`)
```
if (h->count (+0x240) > 0) {
    for (i = 0; i < count; i++) {
        item = h + i*0x24;
        if (item->resource (+0xc) != 0) {
            // unlink the intrusive node: exe +0x0 = prev, +0x4 = next
            if (item->prev (+0x0) == 0) *(item->resource + 0x78) = item->next (+0x4);
            else *(item->prev + 4) = item->next;
            if (item->next != 0) *item->next = item->prev;
            item->resource = 0; item->prev = 0; item->next = 0;
        }
        item->owner (+0x8) = 0;
    }
}
h->count = 0;
```
Our `SfxItem` names +0x00 `next` and +0x04 `prev` as table indices (src comment),
not the exe's `prev`/`next` pointers; the existing `clear()` already zeroes all
entries and the count, so keep it.

### 0x4b5980 SfxTable.addItem(ResourceRef sfx, F pitch, F pmin, F pmax, F vmin, F vmax)
```
getArgs(self, ref, pitch, pmin, pmax, vmin, vmax);   // 0x4b1af0
h = instanceToHandle(self, DAT_007a2620);            // 0x4ecc70
if (h != 0 && h->count (+0x240) < 0x10) {
    i = h->count++;
    item = h + i*0x24;
    bind(item /*ECX, elided*/, *(ref + 0xc));        // 0x40cac0: link + addref the SfxRef RI
    item->inv_pitch (+0x10) = 1.0 / pitch;
    item->pmin (+0x14) = pmin;
    item->pmax (+0x18) = pmax;
    item->vmin (+0x1c) = vmin;
    item->vmax (+0x20) = vmax;
}
```
Matches the existing `audio::SfxTable::add_item` (count guard, `1.0f / pitch`,
the four range params, `owner = resource + 0x80`).

### 0x4b2ae0 Vehicle.getHorn
```
h = instanceToHandle(self, DAT_007a2620);    // 0x4b1af0 + 0x4ecc70
if (h == 0) return 0;
RI = h->0xc;                                 // RI, the typed resource instance
if (RI == 0) return 0;
if (RI->0x7c != 1) RI->vf5(1.0f);            // make resident
if (Resource__getTyped(RI, 0x80000000, 0, 0) < 0) return 0;   // 0x51f4f0, ECX = RI
T = RI->vf3(1.0f);                           // typed vehicle object
if (T == 0) return 0;
P = T->0x4c;                                 // car payload (our CarData)
e = *(P + 0x2050);                           // engine block (our CarData::engine_sfx)
return *(int *)(e + 0x840 + *(int *)(P + 0x1e00) * 0x10) != 0;  // e+0x840+id*0x10
```
`P+0x1e00` is `horn_id` (already `CarData::horn_id`). The horn records are
`e+0x838 + id*0x10` nodes; the checked word is node+0x08 (owner =
resource+0x80) = `e+0x840 + id*0x10` (notes/13 §9, §12.1; notes/14 §2.3).
Our `audio::EngineSfx::horn_resource[id]` stores that id/owner word and the
evaluator already tests it (`src/audio/sfx.cpp:271`).

### 0x4e6960 Vehicle.getSpeedSquare
```
h = instanceToHandle(self, DAT_007a2620);
if (h == 0) {
    log("vm_vehicle_getspeedsquare: scripted resource %x not found in file \"%s\" at line %d");
    return 0;
}
FUN_0047c640(h, 3, &v);                      // GetInfo id 3 = GII_VEL (= 0x4db050 GameRef.getVel)
return v[0]*v[0] + v[1]*v[1] + v[2]*v[2];    // locals -0x14/-0x10/-0xc = v[0..2]
```
`FUN_0047c640`'s result is not checked (the out floats are left as-is on
failure). Our model of info id 3 is `Runtime::Resource::vel`; when the receiver
has no resource the port returns 0 (the log text is not reproduced).

## Changes

1. `src/game/car.h`
   - `#include "audio/sfx.h"`.
   - In `struct CarData`, with the engine-block `e` fields: `audio::EngineSfx engine_sfx;`
     with a comment that the exe's `e = *(car+0x2050)` is aggregate-modelled in
     `CarData` (like `exhaust_min_vol`/`heat_capacity`).
   - Next to `bind_wheel`/`wheel_data`: declare
     `void bind_sfx_table(Object *wrapper, audio::SfxTable *t);` and
     `audio::SfxTable *sfx_table(Object *wrapper);` (the `Native` raw-payload
     layer; no producer yet, see Goal).
2. `src/game/car.cpp`
   - `std::map<Object *, audio::SfxTable *> g_sfx_tables;` next to `g_wheels`;
     `bind_sfx_table`/`sfx_table` mirroring `bind_wheel`/`wheel_data`;
     `g_sfx_tables.clear();` in `reset()`.
3. `src/script/natives.cpp`
   - `#include "audio/sfx.h"` (audio/audio.h is already included).
   - Add near `pt_set_sfx_loop_params`: `sfx_table_of(const Args&)` (the
     wrapper-object payload lookup) plus
   ```cpp
   // 0x4b5a90 java.game.parts.SfxTable.getItems
   bool sfx_get_items(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("SfxTable.getItems", a, 1, err)) return false;
       audio::SfxTable *t = sfx_table_of(a);
       *out = Value::make_int(t ? t->items_count() : 0);
       return true;
   }
   // 0x4b5a60 java.game.parts.SfxTable.clear (0x498280 helper)
   bool sfx_clear(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("SfxTable.clear", a, 1, err)) return false;
       if (audio::SfxTable *t = sfx_table_of(a)) t->clear();
       *out = Value::make_null();
       return true;
   }
   // 0x4b5980 java.game.parts.SfxTable.addItem
   bool sfx_add_item(Vm &, const Args &a, Value *out, std::string *err)
   {
       float pitch, pmin, pmax, vmin, vmax;
       if (!want("SfxTable.addItem", a, 7, err) ||
           !get_f("SfxTable.addItem", a[2], &pitch, err) ||
           !get_f("SfxTable.addItem", a[3], &pmin, err) ||
           !get_f("SfxTable.addItem", a[4], &pmax, err) ||
           !get_f("SfxTable.addItem", a[5], &vmin, err) ||
           !get_f("SfxTable.addItem", a[6], &vmax, err))
           return false;
       if (audio::SfxTable *t = sfx_table_of(a)) {
           // INVENTED: the exe links the SfxRef RI (0x40cac0) and stores its
           // pointer; audio::SfxTable keeps an int32 key -> use the resource id.
           Runtime::Resource *r = value_resource(a[1]);
           t->add_item(r ? (int32_t)r->id : 0, pitch, pmin, pmax, vmin, vmax);
       }
       *out = Value::make_null();
       return true;
   }
   ```
   - Add near `ve_has_crime`:
   ```cpp
   // 0x4b2ae0 java.game.Vehicle.getHorn
   bool veh_get_horn(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Vehicle.getHorn", a, 1, err)) return false;
       int32_t v = 0;
       if (game::CarData *c = car_of(a)) {
           // INVENTED: the exe indexes e+0x840+horn_id*0x10 unchecked; clamp to
           // the four horn records.
           int32_t id = c->horn_id;
           if (id >= 0 && id < 4)
               v = c->engine_sfx.horn_resource[id] != 0 ? 1 : 0;
       }
       *out = Value::make_int(v);
       return true;
   }
   // 0x4e6960 java.game.Vehicle.getSpeedSquare
   bool veh_get_speed_square(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Vehicle.getSpeedSquare", a, 1, err)) return false;
       Runtime::Resource *r = value_resource(a[0]);
       float sq = 0.0f;
       if (r)
           sq = r->vel[0] * r->vel[0] + r->vel[1] * r->vel[1] +
                r->vel[2] * r->vel[2];
       *out = Value::make_float(sq);
       return true;
   }
   ```
   - Registry rows: `getHorn` → `veh_get_horn`; `getItems` → `sfx_get_items`;
     `clear` → `sfx_clear`; `addItem` → `sfx_add_item`; `getSpeedSquare` →
     `veh_get_speed_square` (all `false` for the last field, signatures unchanged).

## UNRESOLVED (raw lines / open points)
- 0x4b5980: `*(float *)(iVar2 + 0x10) = 1.0 / local_c;` — the decompiler shows a
  double literal; the operand width (fdiv with a float 1.0) was not byte-read.
  Keep the existing float `1.0f / pitch`.
- 0x4b5980: `FUN_0040cac0(*(undefined4 *)(local_20 + 0xc));` prints only the
  argument; the node in ECX is elided. Node identity is taken from notes/14 §2.3
  ([V]), not re-verified here.
- The resource lifetime side of `addItem` (addref via 0x40cac0) and of `clear`
  (unlink/release) is not modelled: `audio::SfxTable` stores an int32 key and no
  ref. `shortcut:` this in the code.
- No SfxTable wrapper can exist until `Chassis.getSfxTable 0x4b3890` is ported;
  the three natives therefore always take the exe's null-handle path. Same for
  `getHorn`: `Chassis.setHornSFX 0x4b4f40` is still a stub, so the horn record is
  always 0 (the exe value when no horn sound was set).

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
