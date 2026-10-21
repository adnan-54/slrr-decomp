---
id: 040
status: implemented
branch: card/040
depends: [004, 009, 022, 023, 024, 026]
---
# events: per-instance event mask + notification list, GameType mask/notification natives

## Goal
Give every game instance the two pieces of event state the exe keeps on its typed
object: the event mask (`typed+0xa0`) and the notification list (`typed+0x5c`).
Make `GameType.setEventMask`, `clearEventMask`, both `addNotification` overloads
and `remNotification` non-stubs. Dispatch itself is card 041; this card only
stores state. (Depends on the earlier natives.cpp cards only because they edit the
same file.)

## Exe evidence (cleaned pseudocode, renamed fields)
`h = handle(this)` is our `value_handle(a[0])`, `typed` is our `Runtime::Resource`
(`value_resource(...)`).

### 0x4dc780 GameType.setEventMask(int m) -> 0x4ebd90
    typed = handle(this)->RI->typed;   // a null handle crashes in the exe
    typed->event_mask |= m;            // typed+0xa0

### 0x4dc4d0 GameType.clearEventMask(int m) -> 0x4e8820
    typed->event_mask &= ~m;

### 0x4e7480 Notification ctor (0x44 bytes)
    +0x24 watcher ref node (id at +0x2c)   = watcher
    +0x34 mask   = etype      // bit31 one-shot, bit30 "no forward when a sender is given"
    +0x38 alias  = ealias     // low 28 bits replace the event code; 0x10000000 = use custom message
    +0x3c msg    = custmsg    // char* stored as given (may be null)
    +0x40 method = custmethod // copied; null when absent

### 0x4da280 GameType.addNotification(GameRef ref, int etype, int ealias, String msg)
### 0x4da140 GameType.addNotification(GameRef ref, int etype, int ealias, String msg, String method)
    if (ref == null) return;
    me = handle(this); if (!me) { log "vm_gameref_addnotif: scripted resource ..."; return; }
    // 0x4e78e0, ECX = ref:  the list lives on the WATCHED object (ref)
    t = ref->RI->typed; if (!t) return;
    for n in t->notifications:                 // head first
        if (n.watcher == me && n.mask == etype) { unlink n; break; }
    t->notifications.push_front(Notification(me, etype, ealias, msg, method /*null for the 4-arg form*/));

### 0x4db6c0 GameType.remNotification(GameRef ref, int etype) -> 0x4e92d0
    if (ref == null) return;
    me = handle(this); if (!me) { log ...; return; }
    t = ref->RI->typed; if (!t) return;
    remove the first n with n.watcher == me && n.mask == etype

## Changes
1. `src/game/runtime.h`, inside `struct Resource` after `uint32_t flags`:
   ```cpp
   // Notification (0x4e7480, 0x44 bytes) on the watched instance's list.
   struct Notification {
       Resource *watcher = nullptr;   // +0x24 ref node
       uint32_t mask = 0;             // +0x34 etype
       uint32_t alias = 0;            // +0x38 ealias
       bool has_message = false;      // +0x3c non-null
       std::string message;
       bool has_method = false;       // +0x40 non-null
       std::string method;
   };
   uint32_t event_mask = 0;                  // typed+0xa0
   std::vector<Notification> notifications;  // typed+0x5c, index 0 = list head
   ```
   INVENTED: initial mask 0 (the typed-object ctor value was not traced; Osd sets
   its own mask with `setEventMask(EVENT_CURSOR|EVENT_HOTKEY)`).
2. `src/script/natives.cpp`: five natives in a new `// ---- java.lang.GameType events ----`
   section placed right before `gt_create_native_instance`
   (`grep -n 'bool gt_create_native_instance' src/script/natives.cpp`). Pattern:
   `need_rt(...)`, `receiver_object(...)`, `want(...)`, `get_i(...)`; receiver
   resource `value_resource(a[0])`, ref resource `value_resource(a[1])`.
   - setEventMask/clearEventMask: INVENTED: a null receiver resource is ignored (exe crashes).
   - addNotification: `a[4]` (and `a[5]` for the 5-arg form) is `Value::Str` →
     `has_message/has_method = true` + text; `Value::Null` → false.
     Watcher resource null (exe logs "scripted resource not found") → return true with null out.
     Insert with `notifications.insert(notifications.begin(), n)` after erasing the old match.
   - Each function gets a `// 0xADDR GameType.name` comment; all return `Value::make_null()`.
   - Flip the five registry rows
     (`grep -n '"java.lang.GameType", "\(setEventMask\|clearEventMask\|addNotification\|remNotification\)"' src/script/natives.cpp`)
     from `nullptr, true` to `<fn>, false`.

## Files
- edit: src/game/runtime.h (struct Resource), src/script/natives.cpp (5 natives + 5 rows)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
