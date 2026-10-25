---
id: 192
status: implemented
branch: card/192
depends: [191]
---
# vmboot: route the dialog / garage / dealer / generator clicks and the finish Enter through the exe input paths

## Goal
vmboot still drives five steps with INVENTED direct `osd_command(...)` calls (vmboot.cpp:
`answer_dialogs` ~507, Garage cmd 122 ~676, CarMarket buy/back ~710 + browse-right ~690,
Garage cmd 133 ~735, Generator 0x10E ~766) and the race finish key (~3388, osdCommand(0x01)
on the fWindow). Card 191 made the main-menu click real (`menu_select`: grid scan with
`rt->screen_pick`, `move x,y` to the native cursor, mouse button 1 held through the synthetic
backend, LCLICK -> Group.handleEvent -> `g.click()` -> osdCommand). Generalise that into one
helper and use it for every Osd button; replace the finish-window osdCommand with a real Enter
key hold (the main menu already does this: `g_input->devices[0].raw.keys[0x1c]`, kPress..kRelease).
No engine code changes except one comment in cursor.cpp. Background: notes/18 s7 (line 223), s8 (line 265).

## Exe / script evidence
- Click path is the exe's: cursor tick 0x4b8430 (src/game/cursor.cpp), pick 0x4eb000
  (`Runtime::screen_pick`). The pick only runs while the cursor has `+0x50 & 1` (MouseCursor
  `enable`); Dialog.show() calls `Input.cursor.enable(1)` (Dialog.java:171-172).
- Gadget fields (Gadget.java:25-26): `int cmd; Object handler;` handler defaults to
  `osd.globalHandler` (Osd.java:85,1052).
- Targets (script facts):
  | step | receiver (handler) | gadget cmd | source |
  |---|---|---|---|
  | dialog OK | the waiting Dialog object | 0 (button index; "YES;NO" YES = 0) | Dialog.java:149-155 `osd.createButton(style,x,y,text,i)` |
  | used dealer | Garage state | 122 CMD_BUYCARSUSED | Garage.java:87 |
  | market browse right | CarMarket state | 58 AXIS_MENU_RIGHT | CarMarket.java:428 |
  | market buy / back | CarMarket state | 34 AXIS_SELECT / 35 AXIS_CANCEL | CarMarket.java:421,430 |
  | event list | Garage state | 133 CMD_EVENTLIST | Garage.java:538 |
  | race | EventList.eGen (Generator) | 0x10E CMD_RACE | EventList.java:345 |
- Dialogs also put a fullscreen shield button cmd 9999999 behind their buttons
  (Dialog.java:131-132): the grid scan must match cmd AND handler, never "first hit".
- Finish window: finishWindowCircuitRacing binds Enter to OSD command 0x01
  (CircuitRacing.java:1364); hotkeys are sampled by the 50 ms Frontend HotkeyWatcher, scan 0x1c
  = ControlSet MENUSET AXIS_SELECT (RCDIK_ENTER), 0x544830 reads the raw key byte's high bit
  (vmboot comment above kPress).
- cursor.cpp:113 `shortcut: right-click's camera/car hand-off (0x4a2b40/0x4af9a0) is not ported`
  is wrong: that code is dead in the exe (notes/18 s8: transforms an uninitialised stack temp
  that is never read; RCLICK strings use the +0x128 copy). Replace the comment with:
  `// 0x4b98c4..0x4b9950: RCLICK getPhysBody + 0x4a2b40/0x4af9a0 transform a dead temp; no port.`

## Steps
1. Split `menu_select` into `bool find_gadget_pos(Boot&, Object *handler, int cmd, float *x,
   float *y, Runtime::Resource **body)` (64x64 grid scan as now, match `gadget.cmd == cmd` and
   `gadget.handler == handler`; `handler == nullptr` = any handler, which menu_select passes so
   its cmd-0xB007-only match and its check stay unchanged).
2. Add a click queue driven from the main frame loop so fixed frames (`i == kGarageCmd`, ...)
   are never skipped: `struct PendingClick { Object *handler; int cmd; const char *what;
   int phase; uint32_t body; };` `std::deque<PendingClick>`. Each frame, before
   `loop.frame`: if the head has phase 0: resolve with find_gadget_pos; on success queue
   `move x,y` (as menu_select) and set phase 1; on failure print
   `  click <what>: no grid point picks cmd <cmd>; falling back to osdCommand` and call the old
   `osd_command(...)` (keeps the flow; INVENTED fallback). Phases 1..10: mouse button 1 held
   (`g_input->devices[1].raw.mouse_buttons[0] = 0x80`), 11..20 released; after phase 20 print
   `  click <what>: cursor LCLICK body 0x%x %s` (ok if `game::cursor_last_click_body() == body`)
   and pop. INVENTED: the press/hold timing (10+10 frames, as menu_select).
3. Replace the six `osd_command` stand-ins with `clicks.push_back({...})` (keep their printouts
   and the `*_from`/`*_at_*` bookkeeping; `dealer_started` becomes "click queued or fallback
   started"). answer_dialogs: keep the NoYes text print and the StringRequester input fill
   (still INVENTED: typing), then enqueue cmd 0 on the dialog instead of osd_command. Buttonless
   dialogs (no gadget with cmd 0, e.g. SimpleLoadingDialog) take the fallback path.
4. Finish window (~3388): instead of `osd_command(fwv.obj, 0x01, ...)`, hold
   `raw.keys[0x1c] = 0x80` for 60 frames then 0 (1 s hold like kPress..kRelease), via `frames(1)`.
5. Checks: one `c.check` per step that DID go through the cursor on your run
   ("<what> clicked through the native cursor"); steps that fell back only print. List both
   groups in your report.
6. cursor.cpp: replace the shortcut comment (see evidence).

## Files
- edit: src/tools/vmboot.cpp (menu_select ~264, answer_dialogs ~507, game_init_check loop
  ~650-790, finish window ~3365-3400), src/game/cursor.cpp (comment at :113 only)
- read only: src/game/cursor.h, src/game/runtime.h (screen_pick, PickHit)

## Done when
- existing checks unchanged in meaning; the flow still reaches Track and the race finish in
  `--race` (`make -C src clean check` green, both vmboot modes)
- each routed step prints its cursor/fallback result; INVENTED markers on the press timing,
  the fallback, and the StringRequester typing
- card frontmatter: status implemented, branch name; report lists cursor vs fallback per step
