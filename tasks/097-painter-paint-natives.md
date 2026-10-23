---
id: 097
status: merged
branch: card/097
depends: []
---
# script: Painter.xPaint port; paintPart/doPaint blocked on a scene-pick service

## Goal
Replace the three `java.game.Painter` stub rows in `src/script/natives.cpp`
(lines 7273-7275).

- `xPaint` (0x4e07c0, 53 bytes) is fully portable: it queues event `0x10` with
  the constant string `bigdecal 0,0 0,0,0 0,0,0 0 0 1 0` on the GameRef
  argument's resource, with no other work.
- `paintPart` (0x4e0800) and `doPaint` (0x4e0560) both do everything through
  0x4eb000, a render-scene pick that src does not have. There is no partial
  behaviour to port: the pick's output (hit surface + two sampled decal points)
  is the event target and the `%f` payload. Per the 085/091/093 no-service
  pattern, src validates the argument count and returns `void`. The complete
  post-pick body (guards, formats, hit-record outputs) is recorded below so a
  later pick-service card can fill it in without re-tracing. This is a
  deliberate divergence, not equivalent behaviour: the exe emits `paint`/
  `bigdecal` events whenever the pick hits. Upgrade when 0x4eb000 exists.

## Exe evidence (cleaned pseudocode; getArgs mapping from instruction listings)
`getArgs` 0x4b1af0: out0 = last VM slot (receiver; for the static `doPaint` the
exe still supplies one unused slot), then the declared arguments in order.
The decompiled call expressions mislabel these slots; the push order in
`read_function_instructions` is authoritative. `0x47c640` = `Game__getInfo`
(ECX = Game 0x5fde90); id 2 = `GII_POS`, id 7 = `GII_CATEGORY`; the category
values 9/5 are `GIR_CAT_PART`/`GIR_CAT_VEHICLE` per the shipped
`2.2.1 sources/system/Scripts/lang/src/GameType.java`, and matching the
`cat == GIR_CAT_PART || cat == GIR_CAT_VEHICLE` test in Painter.java.
`0x47d550` = queueEvent(ECX = Game, target ref node, sender, code, param,
method). Strings confirmed by `read_bytes`.

### 0x4e07c0 Painter.xPaint(GameRef part)V
    getArgs(ctx, &self, &part);                 // self [-0x8] unused
    queueEvent(part, 0, 0x10, "bigdecal 0,0 0,0,0 0,0,0 0 0 1 0", 0);
    // no null check; 0x5a56a0 is the constant

### 0x4e0800 Painter.paintPart(GameRef cursor, int color)V
    getArgs(ctx, &self, &cursor[-0x20], &color[-0x24]);
    Game__getInfo(cursor, 2, &pos[-0x38]);       // GII_POS, x/y/z
    FUN_00571110();                              // no args, purpose untraced
    ok = FUN_004eb000(&poscopy[-0x44], &surface[-0x1c], &link[-0x2c],
                      &hit3[-0x58], 0, &hitI[-0x4c], 0, 0, 0, 0);
    if (!ok) return;                             // EAX = 0 (void)
    if (Game__getInfo(surface, 7, 0) != 9 && != 5) return;
    sprintf(buf[-0x98], "paint 0 0 %d 0 -1", color);        // 0x5a527c
    queueEvent(surface, 0, 0x10, buf, 0);
    // release link 0x501ca0 and the surface ref-node list; EAX = 0

### 0x4e0560 Painter.doPaint(GameRef cursor, int color, int brush, int temp, float rot, float size, int flip)V
    getArgs(ctx, &cls[-0x6c] /*unused*/, &cursor[-0x48], &color[-0x34],
            &brush[-0x20], &temp[-0x28], &rot[-0x2c], &size[-0x30], &flip[-0x24]);
    hTmp[-0x44] = {0,0,0,0};
    Handle__setId(&hTmp, brush);                 // 0x51f9b0 -> 0x5183a0 find-by-id
    // hTmp.id ([-0x3c]) = brush when the id resolves, else 0; node released below
    Game__getInfo(cursor, 2, &pos[-0x7c]);
    ok = FUN_004eb000(&pos[-0x7c], &surface[-0x1c], &link[-0x50],
                      &hit3[-0x88], 0, &hitI[-0x70], 0,
                      &ptA[-0x68], &ptB[-0x5c], 0);
    if (!ok) return;
    if (Game__getInfo(surface, 7, 0) != 9 && != 5) return;
    sprintf(buf, "bigdecal %d,%d %.3f,%.3f,%.3f %.3f,%.3f,%.3f %.2f %.2f %d %d",  // 0x5a5570
            color, hTmp.id,
            ptA.x, ptA.y, ptA.z, ptB.x, ptB.y, ptB.z,
            size, rot, temp, flip);              // note size before rot
    queueEvent(surface, 0, 0x10, buf, 0);
    // release hTmp/link/ref nodes; EAX = 0

Argument order/format check from the sprintf pushes: color and brush id are the
two `%d`; the first six `%.3f` are the two 3-float pick outputs (param_8/9);
then `size` (arg6) and `rot` (arg5); the last `%d` pair is `temp` (arg4) then
`flip` (arg7).

### 0x4eb000 scene pick (helper, not ported — this is the missing service)
Input: world position (only x/y used; z ignored). Walks the render-scene tile
list (`0x421220` first tile, `0x421260` first child, `0x4210d0` next child):
tile fields +0xd4/+0xd8 are the centre, +0xdc/+0xe0 the half size; rejects the
tile unless the normalised u,v are in [-1,1], negates v. For each child it
builds two points on the child (`0x421e20(child, out3, uv)`, scaled ×0.1) and
casts the segment with `0x4fe1a0(points, child+0xac0, &t)`; `0x4fe1a0` clears
the global hit record, calls the real cast `0x4fddf0` and returns
`t < 1.0`. On a hit it binds the surface ref node to global `DAT_005dae78`
(0x40cac0), links the temp node into `DAT_005dae64`, and copies the hit record
(`DAT_005dae44/48/4c`, `50/54/58`, `5c`, `40`, `60`) plus the two sampled
points to the outputs; returns 1. No hit: outputs stay zeroed (`param_6` = -1)
and it returns 0.

## Changes
1. `src/script/natives.cpp`, directly after `sfxref_stop` (closes at line 4651,
   before the `java.io.Controller` section):
   ```cpp
   // ---- java.game.Painter -----------------------------------------------------

   // 0x4e07c0 Painter.xPaint(GameRef part)V: queue the constant decal flush on
   // the part. The exe has no null check and would fault in 0x47d550 on an
   // unbound ref node; src's queue_event ignores a null target.
   bool painter_x_paint(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("Painter.xPaint", err) || !want("Painter.xPaint", a, 2, err))
           return false;
       *out = Value::make_null();
       game_rt()->queue_event(value_resource(a[1]), nullptr, 0x10,
                              Value::make_str("bigdecal 0,0 0,0,0 0,0,0 0 0 1 0"));
       return true;
   }

   // 0x4e0800 Painter.paintPart(GameRef cursor, int color)V
   // SERVICE MISSING: 0x4eb000 (render-scene pick) does not exist in src. The
   // exe returns here when the pick misses, then requires the hit surface's
   // GII_CATEGORY (0x47c640 id 7) to be 9 (GIR_CAT_PART) or 5 (GIR_CAT_VEHICLE),
   // and queues "paint 0 0 %d 0 -1" (0x5a527c) with `color` on that surface
   // (0x47d550, code 0x10). Args are copied unchecked by getArgs (0x4b1af0).
   bool painter_paint_part(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Painter.paintPart", a, 3, err)) return false;
       *out = Value::make_null();
       return true;
   }

   // 0x4e0560 Painter.doPaint(GameRef cursor, int color, int brush, int temp,
   //                          float rot, float size, int flip)V
   // SERVICE MISSING: same 0x4eb000 gate. After a hit the exe queues
   // "bigdecal %d,%d %.3f,%.3f,%.3f %.3f,%.3f,%.3f %.2f %.2f %d %d"
   // (0x5a5570) with color, brush id, the two sampled 3D points, size, rot,
   // temp and flip. brush id = `brush` if the id resolves (0x51f9b0 ->
   // 0x5183a0) else 0; the temporary node has no other effect.
   // doPaint is `native static` (shipped Painter.java:349), so src's
   // run_native passes no receiver: 7 arguments (vm.cpp:1608).
   bool painter_do_paint(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("Painter.doPaint", a, 7, err)) return false;
       *out = Value::make_null();
       return true;
   }
   ```
2. Registry rows 7273-7275: replace `nullptr` with the matching function and
   set the trailing `engine` bool to `false` (the field is documentation only;
   `install` at natives.cpp:7300 never reads it).
   ```cpp
   {"java.game.Painter", "paintPart", "(Ljava.util.resource.GameRef;I)V", painter_paint_part, false},
   {"java.game.Painter", "doPaint", "(Ljava.util.resource.GameRef;IIIFFI)V", painter_do_paint, false},
   {"java.game.Painter", "xPaint", "(Ljava.util.resource.GameRef;)V", painter_x_paint, false},
   ```

## Files
- edit: src/script/natives.cpp (three functions after `sfxref_stop`; rows 7273-7275)
- read only: src/script/vm.cpp (`run_native` static receiver convention, line 1608)
- read only: src/game/runtime.h (`queue_event`, `Handle`/`value_resource` mapping)

## Done when
- each of the three functions has its `// 0xADDR` comment; the missing pick is
  marked `SERVICE MISSING:` with the 0x4eb000 outputs and format strings above
- the three registry rows point at the functions and end in `false`
- `make -C src clean check` green; no new unit tests or hand-made expected
  values (the constant xPaint string is taken verbatim from exe 0x5a56a0)
- card frontmatter: status implemented, branch name

## UNRESOLVED
- `FUN_00571110()` in paintPart takes no arguments and is not traced; it may
  have scene/frame side effects. It is skipped by the no-service body.
- 0x4eb000's scene iteration (0x421220/0x421260/0x4210d0), the child-to-world
  mapper 0x421e20, and the tile layout (+0xd4/+0xd8 centre, +0xdc/+0xe0 half
  size) are read from decompiled code only; the cast 0x4fe1a0 / 0x4fddf0 and
  globals DAT_005dae40..78 are not traced.
- `Game__getInfo` id 7 (`GII_CATEGORY`) is a type-8 jump-table arm that is not
  recovered; the accepted values 9/5 come from the shipped GameType.java and
  Painter.java, not from the exe's table.
- 0x51f9b0's find-by-id path (0x5183a0) is understood only as
  ResourceManager find; the doPaint brush id has no observable effect because
  the temporary node is released before return. A future port must reproduce
  "brush if resolvable else 0".
- The exe VM supplies one extra (unused) getArgs slot for the static doPaint;
  src's `run_native` passes no receiver for statics, hence `want(..., 7)`.
- xPaint's null/unbound GameRef diverges: the exe faults in 0x47d550, src
  no-ops. Recorded as a shortcut, not equivalent behaviour.
