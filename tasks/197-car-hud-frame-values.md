---
id: 197
status: implemented
branch: card/197
depends: [196]
---
# car HUD: per-frame gauge values (type_car__vf13 HUD block)

## Goal
Each frame, compute the driver slot's HUD gauge state (pin angle, text string, lamp on/off) from
the gauges of card 196, as type_car__vf13 0x494910 does in its block 0x4954bc..0x495945 through
`gauge_update` 0x4b1d50. src keeps the result as plain state per gauge (no render objects yet).
Full RE: notes/21-hud-osd.md §4.

## Exe evidence
### Gate (asm 0x4954bc..0x495510)
```
if car+0x2054 != 0: skip                     // traffic/bot id (src: no such field -> always 0)
for s in 0 .. M+0x17ec-1:                    // control slots, R_s = car+0x150 + s*0x4a8
  if R_s+8 (player id) == 0 or R_s+0x58 (HUD viewport id) == 0 or s != M+0x17f0: continue
  rpm = |car+0x1e80 * 9.549296|              // [0x59ab98], ANDPS abs
  v   = |root active-state vel|              // root = car+0x13ec (car_root)
  ... gauge updates below, in this order
```
### 0x4b1d50 gauge_update(G, elem, parent, x)   (ECX = G)
```
if G.ref id == 0: return                       // G+0x24
clamp(y): out = G.min; if (G.min <= y) { out = G.max; if (y <= G.max) out = y }   // NaN -> min
kind 1 (pin):  a = clamp(G.scale*x + G.offset); elem (created if needed) bone00 pose:
               pos = G.pos, ori = (G.ori[0], G.ori[1], G.ori[2] + a)
kind 2 (text): a = clamp(G.scale*x + G.offset); elem text = sprintf(G.fmt, (double)a)   // 0x4eb370 -> 0x5277d0
kind 5 (lamp): if x <= 0.01: elem deleted (if any), return
               else elem (created if needed) at pos = G.pos, ori = G.ori
other kinds: nothing
```
### Value sources, in exe order (asm 0x495516..0x495945; indices = card 196 table)
```
gear  G9 : 0x4eb370(elem, G9.fmt, (int)(char)gear_chars[car+0x1e5c gear])   // direct sprintf, no ref test, no clamp
rpm   G7 : rpm
speed G8 : v / measure_per_hour        (0x5fe378)
mileage G12 : car+0x2250 mileage * measure_inv10   (0x5fe374)
nos   G10: car+0x1f5c nitro / e+0xa4c tank_nitro, only if tank_nitro > 0
turbo G11: boostGauge(rpm) 0x4f5ab0, only if dyno && dyno+0xdc > 0 && dyno+0xe8 > 0 && dyno+0xec > 0
rpmpin G2: rpm
spdpin G3: v * 3.6                     (0x59ab88)
nospin G4: nitro / tank_nitro, only if tank_nitro > 0
abs   G13: (car+0x70 & 0x1000000) ? 1 : 0
asr   G14: (car+0x70 & 0x2000000) ? 1 : 0
hbrake G15: car+0x1df4 handbrake input
emergency G16: (car+0x1ec0 engine_temp > 100 || rpm > e+0x1c rpm_limit || rpm > e+0x18 max_rpm) ? 1 : 0
if R_s+0x38 != 0 (cockpit view):  G0 <- rpm, G1 <- v * 3.6   (parent = the car's render instance)
else: delete the G0/G1 elements
(then damage colours 0x4b0f80/0x4b1cd0: not traced, not in this card)
```
### boostGauge 0x4f5ab0 (notes/10-engine-dyno.md:86-93)
```
g as in the turbo block of the dyno cycle (dyno.cpp:350-358, same fields)
b = min(p_turbo_max * g, p_turbo_waste)
return (sqrt(b + 1) - 1) / (sqrt(min(p_turbo_max, p_turbo_waste)) - 1)   // sqrt(P), not sqrt(P+1): exe quirk
```

## src mapping
- `struct HudGauge { bool live = false; float angle = 0; std::string text; }` and
  `HudGauge hud[17]` on `game::CarData` (index = card 196 gauge index).
- "elem created" = `live = true`; "deleted" = `live = false`; pin: `angle = a`; text: `text`.
- R_s+8 = `slot_player[s] != nullptr`; `s == driver_slot`. R_s+0x58 and R_s+0x38 are not modelled:
  `// INVENTED: HUD viewport taken as present; cockpit view absent (G0/G1 stay not live)`.
- rpm/v/fields: `rpm_smoothed`, `root_body->vel`, `mileage`, `nitro`, `tank_nitro`, `flags`,
  `handbrake_input`, `engine_temp`, `rpm_limit`, `max_rpm`, `gear`, `dyno` (car.h:159-240).
- dyno offsets: +0xdc turbo_mul, +0xe8 turbo_range, +0xec p_turbo_max (dyno.h:47-48). Add
  `float EngineModel::boost_gauge(float rpm) const` in dyno.cpp next to the turbo block.
- Call site: new `game::hud_cars(float measure_per_hour, float measure_inv10)` over all car
  payloads, called once at the end of `advance_sim` (natives.cpp ~7661) with
  `sched(vm).measure_per_hour / measure_inv10`.
  `// INVENTED: vf13 runs in the render pass per drawn car; src calls it once per advance_sim.`

## Files
- edit: src/game/car.h, src/game/car.cpp (hud_cars, gauge_update), src/vehicle/dyno.cpp (+ .h
  declaration), src/script/natives.cpp (one call)

## Done when
- `// 0x4b1d50`, `// 0x494910 HUD block`, `// 0x4f5ab0` comments; INVENTED as listed
- `make -C src clean check` green in both vmboot modes; vmboot race numbers unchanged (HUD is
  read-only on the simulation)
- card frontmatter: status implemented, branch name
