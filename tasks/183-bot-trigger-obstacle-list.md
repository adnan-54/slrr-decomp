---
id: 183
status: todo
branch:
depends: [181, 182]
---
# `native bot` bot_trigger sensor: create (0x483bd0), follow/resize (0x47f980 tail), vf14 0x20/0x40 obstacle list

## Why (notes/19 s8.2)
Each bot owns a `bot_trigger` sphere sensor around its car. Its trigger contacts (event 0x20 on / 0x40 off) keep
the bot's obstacle list (`cBodyList` records, list at `+0x26ac` DoubleList, head read through `+0x26b4`, append
at `+0x26d4/+0x26dc`). Card 185 walks that list in 0x483bd0 and feeds the solver (card 181). This card builds the
sensor and the list (records with body + type; their sub-records come in card 184).

## Exe evidence
Bot type manager ctor 0x484990: `DAT_005fe214 = ResRefLink(system.rpk, 0x34)` (the `native trigger` type).
```
0x483bd0 head (after the solver reset of card 181):
  if (+0x2700 == 0) {                      // +0x26f8 ResourceRef {prev,next,target(+0x2700),id}: no live trigger
     s = sprintf("%.2f,%.2f,%.2f,0,0,0,sphere,%.2f", S.pos.x, S.pos.y, S.pos.z, 100.0)
     GameObject__createScripted 0x523c80(&ref, DAT_005fe214, s, "bot_trigger")   // ECX = bot: child of the bot
     +0x26f8 = ref; +0x2708 = 0
  }

0x47f980 tail (right after 0x484b90, every tick while a controlled car exists):
  rmax = (mode == 2 || mode == 4) ? 55.0 : 10.0
  r = S.speed * 2.5 + 10.0; if (r < 10) r = 10; else r = min(r, rmax)
  ang = +0x21e0 (steer axis, after the slew) * 0.005905512        // = 0.75/127 rad
  c = cos(ang); s = sin(ang); e = r - 10
  x = S.pos.x + e*c*S.fwd.x + e*s*S.fwd.z
  z = S.pos.z + e*c*S.fwd.z - e*s*S.fwd.x
  str = sprintf "fffffff" (0x56a000) of (x, S.pos.y, z, 0, 0, 0, r)
  0x4eb3f0(+0x26f8, str)                   // -> type_trigger vf7 0x4d4af0 (card 182 move_trigger)
  if (+0x2708 == 0) { 0x501da0(+0x26f8) /*deferred "touched" list, untraced*/; +0x2708 = 0 } else +0x2708--
  (the rest of the tail is the debug HUD, skipped)
```
(S.fwd is the snapshot's flattened unit forward; in mode 3 rmax = 10, so the sphere is r 10 at the car.)
```
type_BOT vf14 0x480850 code 0x20 (param = trigger record, +0x50 = other root body B):
  rec = new cBodyList (0x38, vtable cBodyList; dtor 0x47f930 frees the sub-record chain at +0x30)
  rec.body (+0x24) = B     (0x481ca0: ref-counted link into B's ref list at +0x28)
  inv = B.def+0x2c (inverse mass)
  rec.type (+0x2c) = inv < 1e-5 ? 1 /*static*/ : inv < 0.01 ? 2 /*heavy dynamic: cars*/ : 0
  root = B; while (root+0x58 & 4) root = root+0x20
  if (0x468b00(0xa0000000) on root's ref returns t && (t+0xc & 0x8000)) rec.type = 0     // untraced owner flag
  if (root.id (+0x40) == controlled[active] id) { delete rec; return 1 }                // own car
  build sub-records from B's shape (card 184)
  append rec at the list tail
code 0x40 (param = record, +0x50 = B):
  for each rec: if (rec.body == 0 || rec.body == B) delete rec
0x483bd0 walk (card 185) also deletes records whose body is gone (rec.body == 0).
```
Delivery: the trigger record events go to the sensor's owner, the bot_trigger instance (0x502d60 -> queueEvent);
`type_trigger` vf14 0x4d4ab0 only counts contacts (+0x34 ++/--). How the bot (its parent) gets 0x20/0x40 was not
traced (event forwarding to the parent in Game__dispatchEvent 0x47c760 / notifications); src: INVENTED direct
routing below.

## Files
- edit: src/game/car.h — `BotData`: `Runtime::Resource *trigger = nullptr; // +0x26f8/+0x2700`,
  `int trigger_wait = 0; // +0x2708`, `struct BotObstacle { RigidBody *body; int type; /* sub-records: card 184 */ };`
  `std::vector<BotObstacle> obstacles; // +0x26ac list, append order`
- edit: src/game/car.cpp — `bot_steer`: create the trigger (Runtime::instantiate of the system.rpk 0x34 type, the
  same type resolve path GameRef(map, system:0x34, ...) uses, parent = `b.self`, alias "bot_trigger", params as
  above); `control_bots`: the tail move after the mode switch + slew; new
  `void bot_trigger_event(Runtime::Resource *bot, RigidBody *other, uint32_t code)` (0x20 add / 0x40 remove)
- edit: src/game/runtime.cpp — in the `trigger_event` callback: when the target resource's parent is a native bot
  and the target is its `trigger`, also call `game::bot_trigger_event(parent, other, code)`
  (`INVENTED:` routing; keep the existing script queue untouched)

## Done when
- `// 0x483bd0`, `// 0x47f980 tail`, `// 0x480850 0x20/0x40` comments; `INVENTED:` on the routing and the skipped
  0x468b00/0x501da0 parts
- `make -C src clean check` green; vmboot --race: each bot has one bot_trigger child and records appear when cars
  are near (a diagnostic print is fine, no hand-made expected numbers); no new unit tests
- card frontmatter: status implemented, branch card/183
