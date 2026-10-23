---
id: 087
status: merged
branch: card/087
depends: [085, 086]
---
# game/script: bind the ground's traffic graph (getInfo 0x39) + setTrafficCrossStopOffset

## Goal
Give `GroundRef` natives the exe's traffic-manager lookup `Game__getInfo(self, 0x39)`:
when the handle's resource is a typeof-1 record whose `gametype` is the `native ground`
type, parse its `params` and load the named `.trc` (card 086) once; return the graph or
null. Port `setTrafficCrossStopOffset` (0x4dd7f0) on top of it as the first user. All
other GroundRef natives keep their card-085 no-service bodies. Trace: notes/19 §1.

## Exe evidence (Claude-traced; asm checked where noted)
Shipped data (maps/city.rdb, record 0x00000001, typeof 1):

    gametype 0x00010035
    params 0x00000010,0,0,0,0,0,0,maps\city\meshes\city.scm,maps\city\ai\city.trc
`0x00010035` -> external link 1 (`system\`) record 0x35, rsd `native ground`.

### 0x47c640 Game__getInfo(handle, id, out) — ECX = Game 0x5fde90 (asm: 0x4dd8de `MOV ECX, 0x5fde90`)
    if (handle->id(+8) == 0) return 0;
    RI = handle->+0xc; t = RI ? RI->typeof(+0x7c) : 0;
    if (t == 1) {
        if (RI) { if (RI->+0x7c != 1) RI->vf[0x14](1.0f);            // make resident
                  if (Resource__getTyped(RI, 0xa0000000, 0, 0) >= 0 && (typed = RI->vf[0xc](1.0f)))
                      return GameType__getInfo(typed, id, out); }   // 0x47e450
    } else if (t == 8) { ... typed(0x80000000)->+0xc ->vf[0x3c] ... }  // jump table not recovered
    else { log "GetInfo: Unexpected TypeOf!"; return out; }
    return 0;
### 0x47e450 GameType__getInfo(typed, id, out)
    if (typed+0x4c == 0) { if (typed+0x50) return 0x47dd50(typed, "getInfo", 1, 2, id, 0x4f); }   // script getInfo
    else { obj = (typed+0x44 resource)->typed(0x80000000)->+0xc; return obj->vf[0x3c](typed+0x4c, id, out); }
    return 0;
### 0x4bb570 type_ground__vf15 getInfo(id, out)
    case 0x39: return this+0x88;   // Traffic* (0 when no .trc)
    case 0x3d: return this+0x8c;   // Pedestrians*
### 0x4bade0 type_ground__vf3 create(owner, params) — traffic part only
    trc[0] = 0;
    Str__scan(params, "%x,%f,%f,%f,%f,%f,%f,%s,%s", &id, &x,&y,&z, &a,&b,&c, scm, trc);   // 0x569380 -> 0x527140
    ...
    if (trc[0] == 0) { +0x88 = 0; +0x8c = 0; }
    else {
        buf = 0; n = File__readAll(trc, &buf);                  // 0x565610: '/' -> '\\', <0 = missing
        if (n < 0) +0x88 = 0;
        else { t = new Traffic (0x3144; ctor 0x416000 sets +0x3140 = 0); Traffic__load(t, buf);
               free(buf); Traffic__init(t, *g_physWorld) (0x419da0); 0x47d670(this, 0x1c); +0x88 = t; }
        +0x8c = new Pedestrians(*g_physWorld);                  // 0x41b570 (not modelled)
    }
`Str__vscan` 0x527140 rules needed here: whitespace (9..13, ' ') in the format skips
whitespace in the input; a literal must match or the scan stops; `%x` skips whitespace
then reads hex (0x527490, `0x` prefix accepted); `%f` skips whitespace then reads a float
(0x527530, untraced); `%s` skips whitespace, then copies until whitespace, NUL, 0x400
chars, or the **next format character** (here ','), and NUL-terminates. A failed
conversion returns early and later outputs keep their initial value.
### 0x4dd7f0 GroundRef.setTrafficCrossStopOffset(F)V
    getArgs(ctx, &self, &f);
    h = instanceToHandle(self);                 // not null-checked
    t = Game__getInfo(h, 0x39, 0);
    if (t) t+0x3140 = f;                        // Traffic cross stop offset

## Changes
1. `src/game/runtime.h`: `#include "game/traffic.h"`. In `Resource`, next to the other
   typed-data stand-ins:
   ```cpp
   // type_ground (vtable 0x59e464) typed data +0x88: the Traffic graph. INVENTED: kept
   // on the record node and created on first lookup; the exe creates it in
   // type_ground vf3 0x4bade0 when the instance is made resident.
   bool ground_checked = false;
   std::shared_ptr<traffic::Graph> traffic;
   ```
   In `Runtime` (public):
   ```cpp
   // 0x47c640 Game__getInfo(h, 0x39) -> 0x4bb570: the ground's traffic graph or null.
   traffic::Graph *ground_traffic(Handle *h);
   ```
2. `src/game/runtime.cpp`:
   - `ground_traffic`: `if (!h || h->id == 0 || !h->target) return nullptr;` `r = h->target;`
     `if (r->type != 1) return nullptr;` (shortcut: the typeof-8 jump-table branch is not
     recovered); first call sets `ground_checked` and runs `load_ground(r)`; return
     `r->traffic.get()`.
   - `static void load_ground(Runtime &rt, Resource *r)`: needs `r->record` and
     `r->package`; read the record bytes (`r->package->rpk.data(*r->record)`,
     `data_size`) line by line like `ResourceManager::refs` (CR stripped, first token);
     take `gametype <hex>` and the rest of the `params` line. Resolve the type:
     `Resource *ty = rt.resolve(gametype, r->package)`; it must have a record whose
     `rt.resources().refs(*ty->package, *ty->record)` contains a `DataRef::Native` with
     `native == "ground"`; otherwise return (shortcut: a scripted type's `getInfo`
     (0x47dd50) is not modelled, result stays null).
   - params: a local `scan_ground_params(const std::string &s, std::string *trc)` that
     follows the 0x527140 rules above for exactly this format: hex, 6 floats
     (`strtof`; INVENTED stand-in for 0x527530), two `%s` fields split at ','. Stop at the
     first mismatch leaving `trc` empty.
   - `trc` empty -> return. `path = rt.resources().resolve_path(trc)`; empty or
     `fs::read_file` fails -> return (`File__readAll` < 0). Then
     `auto g = std::make_shared<traffic::Graph>(); size_t used;` `if (!g->load(...))` log
     `"ground: <trc>: <err>"` to stderr and return (INVENTED: the exe has no failure path);
     `r->traffic = g`. `Traffic__init` 0x419da0 and the 0x47d670 registration are not
     ported (shortcut, untraced).
3. `src/script/natives.cpp`: replace the card-085 body of `gr_set_traffic_cross_stop_offset`:
   `want(...,2)`, `get_f(a[1])`; `traffic::Graph *t = game_rt()->ground_traffic(value_handle(a[0]))`
   (null handle -> null, INVENTED: the exe does not check); `if (t) t->cross_stop_offset = f;`
   null result. Keep the `// 0x4dd7f0` comment, drop the SERVICE MISSING line.

## Files
- edit: `src/game/runtime.h`, `src/game/runtime.cpp`, `src/script/natives.cpp` (one function)
- read only: `src/resource/manager.h` (`refs`, `resolve_path`, `DataRef`), `src/platform/fs.h`,
  `src/game/traffic.h` (card 086)

## Done when
- `// 0xADDR` comments on `ground_traffic`, `load_ground`, the scan and the native; lazy
  creation, the float reader and the load-failure log are `INVENTED:`; the scripted-type
  getInfo, typeof-8 branch and Traffic__init are `shortcut:`
- `make -C src clean check` green; no unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
