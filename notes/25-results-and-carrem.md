# 25 — Race results/rewards dialog and the traffic `car_rem` branch (RE 2026-10-25)

## 1. Results/rewards dialog: script-only, nothing exe-side to port
The post-race dialog is `finishWindowCircuitRacing extends GameType` (CircuitRacing.java:1249-1720,
one `finishWindow*` class per Gamemode, Gamemode.java:15) plus `endRaceWindow` (Gamemode.java:1339).
Everything it does lives in script:
- layout: `osd.createText/createRectangle`, `Text.changeText/changeColor`, fade animations
  (`setupAnimation/restartAnimation/fadeIn/fadeOut/a_speed` are script members of the osd Text
  wrapper, not natives: no row in analysis/natives.tsv);
- results: `getRanks`, `timeToString`, `bot_laps`; prize text `game.gmcEvent.getPrizeCash(finishPos)`
  / `getPrizeName()`; reward `gmcEvent.aquirePrize(finishPos)` (CareerEvent), career stats
  `updateCareerStats/updateEventStatus`, prize type checks in Gamemode.java:1148-1175;
- flow: hotkey Enter -> `osdCommand(0x01)` -> `hide()`, `fwThread.methodStatus(2)` handshake, then
  `Track.getOut` -> EventList.
The natives it reaches (Text.create/changeColor/setPos, Osd, Input.createHotkey, Thread.methodStatus,
sound loop) are already implemented: `vmboot --race` runs the whole flow (card 172) with 0 failed
threads, and an unimplemented native would fail its thread by name (natives.cpp install()).
Conclusion: no card. Prize amounts/rules are game rules in .java; the exe must not know them.

## 2. `car_add` / `car_rem`: two exe-side producers
### 2.1 The type-record `notify` flag (M+0x2ec0 bit 2)
- cfg keyword table 0x5da880: `{0x59d5f8 "notify", 0x48fb10}`.
  `0x48fb10(ctx, M)`: `if (M+0x120 == 0) M+0x2ec0 |= 2;` (M+0x120 is taken as 0 in src, same
  UNRESOLVED as `controller`/`driver`, src/game/car.cpp fill_car).
- type_car vf1 0x49ab20 defaults: `M+0x2ec0 = 0` (0x49b47f); bit 0 is another keyword (0x49bde0).
- type_car getInfo 0x4a15c0 case 0x45: `return (M+0x2ec0 >> 1) & 1` (the traffic code copies it to
  traffic-car +0xdd, 0x4dd090).
- Shipped data: exactly ONE cfg has `notify`: `cars/misc/Police_V3/scripts/_main_.cfg` line 2
  (the police car, City.java `GRI_POLICECAR = cars.misc.Police:0x0006r`). No traffic cfg has it.

### 2.2 Vehicle__createCar 0x4992e0 tail (after 0x4c2b10 and 0x49a8e0, before the parent +0x4000 flag)
```
T = new typed car; id = T+8 (instance id); parent = T+0xc (parent instance handle)
if ((M+0x2ec0 & 2) && (id & 0xffff0000) != 0xffff0000 && parent != 0)
    sprintf(buf, "car_add %d %d", id, 0);
    Game__queueEvent(parent+0xf0, 0, 0x10 /*EVENT_COMMAND*/, buf, 0);   // 0x47d550
```
### 2.3 type_car vf4 0x49beb0 head (first thing the dtor does)
```
if ((M+0x2ec0 & 2) && (T+8) >> 16 != 0xffff && T+0xc != 0)
    sprintf(buf, "car_rem %d %d", T+8, T+0x2054);
    Game__queueEvent((T+0xc)+0xf0, 0, 0x10, buf, 0);
```
T+0x2054 = the traffic car that drives this instance (set by getInfo 0x38 case of 0x4a15c0, from
`addTrafficCar` 0x4dd090; 0 when no traffic car took it). src has no traffic cars -> always 0.
Polarity note: the gate is `!= 0xffff` (game instances, not 0xffff script resources). The comment at
src/game/car.cpp instance_dtor ("id>>16 == 0xffff") has it backwards.

### 2.4 Traffic manager producers (NOT portable yet)
Lane traffic cars send to the Traffic sink `Traffic+0x3130` (DAT_005e652c+0x3130), only when the car's
+0xdd notify byte is set: `car_rem <inst id> <trafficcar*>` from 0x40ebb0 (lane update, car leaves
the visible range), 0x410680 (same, single car) and ctCross__halt 0x40de70 (halted cars); `car_add`
from 0x40cd24/0x40f433/0x40f64c/0x40fa92. They need the traffic car pool (notes/19 §2, not carded).

### 2.5 Consumers (script)
City.java:3082-3180 `handleEvent(GameRef, EVENT_COMMAND, String)`: `car_add id trafficId` ->
police marker (`GRI_POLICECAR`) or opponent tracker; `car_rem id` -> remove marker/tracker,
`corpse 0 0`. HotActionCop.java:895 same. In src today nothing creates a `notify` car (police is only
spawned through `addTrafficP`, City.java:818), so the effect is invisible in vmboot; card 209 ports
2.2/2.3 so the messages exist once City/traffic runs.

## 3. Card
- 209 `notify` keyword + car_add/car_rem messages (0x48fb10, 0x4992e0 tail, 0x49beb0 head).
