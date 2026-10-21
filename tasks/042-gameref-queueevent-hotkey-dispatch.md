---
id: 042
status: implemented
branch: card/042
depends: [041, 023]
---
# events: GameRef.queueEvent native (0x4db590) + hotkey events through queue_event

## Goal
Make `GameRef.queueEvent(ResourceRef ro, int type, String param)` a non-stub on top
of `Runtime::queue_event` (card 041), and replace card 023's shortcut so
`Input.checkHotkeys` really delivers `handleEvent(Hotkey)` to the handler.

## Exe evidence (cleaned pseudocode, renamed fields)
### 0x4db590 GameRef.queueEvent(ResourceRef ro, int type, String param)
    getArgs(frame, &this, &ro, &type, &param);
    h = instanceToHandle(this);            // our value_resource(a[0])
    if (h) { Game__queueEvent(h, ro /* sender, may be null */, type, param, 0); return; }   // 0x47d550
    log "vm_gameref_queueevent: scripted resource <id> not found in file \"..\" at line N"; // non-fatal

### 0x4de750 Input.checkHotkeys (tail, see card 023)
    if (count > 0)
        Game__queueEvent(slot[first].handler, 0, 0x100000, slot[first].hotkey, 0);
    // 0x100000 path in 0x47c960 calls handler.script.handleEvent(hotkey) (one argument)

## Changes (src/script/natives.cpp only)
1. New native `gr_queue_event` (`// 0x4db590 GameRef.queueEvent`) next to the other
   GameRef natives (`grep -n 'bool gr_get_flags' src/script/natives.cpp`, add after the
   GameRef block): `need_rt`, `receiver_object`, `want(..., 4)`, `get_i(a[2])`;
   `target = value_resource(a[0])`; null → `*out = Value::make_null(); return true;`
   (exe only logs). `sender = value_resource(a[1])` (null ok).
   `param = a[3].kind == Value::Str ? a[3] : Value::make_null()`.
   `game_rt()->queue_event(target, sender, (uint32_t)type, param);`
   Flip the row `{"java.util.resource.GameRef", "queueEvent", "(Ljava.util.resource.ResourceRef;ILjava.lang.String;)V", nullptr, true}`
   to `gr_queue_event, false`.
2. In the `checkHotkeys` native from card 023 (`grep -n 'checkHotkeys' src/script/natives.cpp`):
   after `hotkeys().check(...)`, drain the queue:
   ```cpp
   std::vector<input::QueuedEvent> q;
   q.swap(input::hotkeys().queue);
   for (const input::QueuedEvent &ev : q)   // 0x4de750 tail -> 0x47d550
       if (Runtime *rt = game_rt())
           rt->queue_event(value_resource(Value::make_obj((Object *)ev.handler)), nullptr,
                           0x100000, ev.hotkey ? Value::make_obj((Object *)ev.hotkey)
                                               : Value::make_null());
   ```
   and delete card 023's `// shortcut: queued hotkey events are never dispatched ...` comment.
   (`value_resource` of a null/unbound object returns null; `queue_event` ignores a null target.)

## Files
- edit: src/script/natives.cpp
- read only: src/input/hotkeys.h (QueuedEvent), src/game/runtime.h (queue_event)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
