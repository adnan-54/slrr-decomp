---
id: 169
status: implemented
branch: card/169
depends: [170]
---
# vmboot spline driver (INVENTED harness): the player car drives itself to checkpoint 0

## Why
The career race (CircuitRacing on Norisring) runs and its triggers fire (card 168), but no harness drives the
car along the track; the native bot driver (0x4803a0 / 0x480850 / 0x482630 / 0x486020, notes/19 §8) is too big
for now. Stopgap: a **vmboot-only** driver (`INVENTED:`, nothing in src/ engine code) presses the existing keys
to follow the AI spline the script hands to the bots. Needs card 170 (steering reaches the tyres).
Script does: checkpoint order (CircuitRacing.handleCheckpoint). Exe provides (ported): triggers, controls.

## Start from
Branch card/169 commit 3908067 (first draft, blocked): it already has the spline load from
`track_data.splinesAI.elementAt(0)` (live script object, backslashes -> `/`, path used as-is, cwd = game dir),
the reversed closed x/z polyline, the per-frame driver, `left_yaw_sign` and the prints. Rebase it on main after
card 170 and change only what is below.

## Facts (2026-10-24)
- First run on card/169: cp0 (48.8, -5.7, -542.4) lies **before** the hairpin apex (z -571) when driving south
  from the grid. The existing INVENTED forced-drag run (Card 161 check 4, vmboot.cpp ~2239) slides the car past
  cp0, so `nextTrigger[0]` is already 1 after it; after the 900-frame auto drive (card 168 check) it is still 0.
- Spline loader 0x482630: lines `x y z<TAB>dx dy dz<TAB>width`; bots drive decreasing index (0x486020).
- Keys: Up 200, Down 208, Left 203 (steer +1), Right 205 (steer -1).
- Success: `handleCheckpoint` (CircuitRacing.java:507-520) does `nextTrigger[0]++` for the player car on cp0.

## What to change (vmboot only)
1. Move the spline driver block to run **right before the Card 161 check 4 forced-drag block** (after the card
   168 checks and the card 160 rev-limiter block), starting with `restore_spawn()`, `car_command "reset"`, settle.
2. Read `nextTrigger[0]` at block start into `nt0`. Driver loop as in the draft (max 4000 frames, stop when
   `nextTrigger[0] > nt0`), release all keys at the end.
3. If the reversed polyline direction at the spawn projection points against the car forward (dot < 0), fail
   the check with that message (do not flip silently).

## vmboot checks
1. `left_yaw_sign` precondition from the draft (|mean yaw| > 0.05 rad/s) passes.
2. **Hard check**: `nt0 == 0` **and** `nextTrigger[0] == 1` at block end, i.e. the block itself observed the
   0 -> 1 transition. It must fail if `nextTrigger[0]` is already 1 at block start.
3. All existing checks pass (forced drag, card 168 records, V-Clip overrun 0), 0 failed threads.

## Done when
`make -C src -j4 check` green; `ulimit -v 4000000; timeout 280 src/build/vmboot --selfcheck` PASSED with checks
1-3. Result: frames used, nt0, final `nextTrigger[0]`, `rpTotal[0]`, `current_lap`, final pos, closest cp0
distance, max distance from the polyline.

## Rules
Stop and report after 2 failed vmboot runs (status `blocked`, numbers in Result). No tuning beyond those runs, no
unit tests, no map names in code, every harness rule `INVENTED:`. No engine/src changes: an engine bug = report.

## Not this card
Native bot, cp1..cp3, finish window, Garage return.
