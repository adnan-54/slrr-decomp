---
id: 113
status: merged
branch: card/113
depends: [111]
---
# runtime: game root ref (DAT_005fde50) as default parent of GameRef.create

## Goal
Shipped `GameLogic.initVehicleTypes` (sl/Scripts/game/GameLogic.java:602) does
`new GameRef().create(null, ct[i], null, "VehicleType")`. Our `gr_create` returns null
for a null parent, so boot stops with `initVehicleTypes line 605: null receiver`.
The exe uses the global game root ref instead (Claude, decompiled 0x4da620):

## Exe evidence
### 0x4da620 GameRef.create (vm_gameref_create) — relevant part
```
args -> receiver, parent (ResourceRef*), type, params, alias      (0x4b1af0)
h = handle(receiver)                    // 0x4ecc70; none -> log "scripted resource ... not found", return 0
p = parent ? parent : &DAT_005fde50     // game root ResourceRef
if (p->id /* p[3] */ != 0) {
    inst = 0x523c80(p, type, params, alias)   // create scripted instance under p
    bind(h, inst)                             // 0x40cac0
    ... return script object of inst (+0x50) or 0
}
return 0
```
Boot's GameInit creation (0x47d760, src/app/boot.cpp `game_init`, card 107) also passes
`&DAT_005fde50` as the parent; today src passes nullptr there with an INVENTED comment.

## Changes
1. `src/game/runtime.h/.cpp`: add `Resource *Runtime::game_root()` that lazily creates
   ONE instance node (use `instantiate(nullptr, nullptr, "<name>", "", false, err)` or the
   same fields by hand) and returns it on every later call; non-zero id. Comment
   `// DAT_005fde50 game root ref`; the node's name/type are INVENTED (not traced).
2. `src/script/natives.cpp` `gr_create` (0x4da620) and `gr_create_native` (0x4da7e0):
   when `value_resource(a[1])` is null, use `rt->game_root()` (keep the `parent->id == 0`
   and `!type` early returns for a non-null parent as today). Fix the misleading comment
   above gr_create ("type defaults to DAT_005fde50" -> parent defaults to it).
   Only do this for gr_create_native if its existing comment/evidence cites the same
   pattern; otherwise leave it and say so in the report.
3. `src/app/boot.cpp` game_init: parent = `runtime_->game_root()`; drop that INVENTED note.

## Done when
- `make -C src clean check` green; no new tests.
- Report the new `tools/vmboot-capped.sh --selfcheck | tail -3` stop line.
