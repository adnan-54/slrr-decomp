---
id: 074
status: implemented
branch: card/074
depends: [073]
---
# game/script: Game deferred unload/delete queues + drain (ResourceRef.unload/destroy, Viewport.destroy)

## Goal
Replace the "apply now" shortcuts in `rr_unload`, `rr_destroy`, `vp_destroy`
(`src/script/natives.cpp`) with the exe's deferred lists on the Game object (0x5fde90):
`ResourceRef.unload` on a game instance (type 1) and every `destroy` append a node to a
queue, and `Game__flushDeferred` 0x47d070 drains them once per control step. The drain
sends `GameRef.EVENT_DELETE` (2) to game instances, then unloads, and for delete
destroys the resource: **every handle bound to it is zeroed** (id 0, target null), its
children are destroyed recursively and it leaves its parent. Evidence: notes/18 §4, §4.1-4.3.

Depends on 073 only because both edit the same placeholder line in `FrameLoop::frame`
(`src/app/boot.cpp`). Timers (073) need **no** extra code here: their `Handle` is bound
with `Runtime::bind`, so zeroing `r->handles` (step D3 below) zeroes it, and
`run_timers` then erases it (id 0) exactly like the exe (0x51f970 + 0x47b700).

## Exe evidence (Claude-traced, asm checked where ECX matters)

`R+0x7c` is the resource **type** (src `Resource::type`), `R+0x78` the handle list
(src `Resource::handles`), handle `{+0 link, +4 next, +8 id, +0xc res}` = src `Handle`.

### 0x4ec800 Handle__unload (ECX = h) — from ResourceRef.unload 0x4e4500
    if (h->id && h->res) { if (h->res->type == 1) Game__queueUnload(h); else h->res->vf[+0x1c](1); }

### 0x4e9450 Handle__queueDelete (ECX = h) — from ResourceRef.destroy 0x4e32b0, Viewport.destroy 0x4e6c90
    if (h->id && h->res) Game__queueDelete(h);

### 0x47e340 Game__queueUnload(h) / 0x47b970 Game__queueDelete(h)
    // queueDelete only: if (h->res && h->res->type == 1) fetch typed data (result unused)
    n = new ResourceRefDLNode(h);   // 0x47b090: own handle, ResRefLink__set(n->handle, h->res)
                                    // -> n->handle.id = R's id (R+0x80), not h->id
    append n at the list tail; count++;

### 0x47d070 Game__flushDeferred (ECX = Game)
    do {
      work = 0;
      while (unload list not empty) {
        n = first;                                  // stays in the list while processed
        if (n->handle.id) {
          R = n->handle.res;
          if (R) { if (R->type == 1) Game__dispatchEvent(&n->handle, 0, 2, 0, 0);  // 0x47c760
                   R->vf[+0x1c](1); }               // unload
          unlink n->handle from R; n->handle.id = 0;
        }
        count--; delete n (unlinks it from the list); work++;
      }
      while (delete list not empty) {               // same, plus destroy
        ... if (R) { if (R->type == 1) dispatch(&n->handle, 0, 2, 0, 0);
                     R->vf[+0x1c](1); R->vf0(1); }  // vf0 zeroes n->handle (D3), so the
        ... unlink is then a no-op
      }
      while (generic list +0x1cc not empty) { unlink; vf0(1); work++; }   // no producer traced
    } while (work);
    if (Game+0x234) 0x559f60();                      // not traced
Event 2 = `GameRef.EVENT_DELETE`; script param is a null String (0x47c960 default branch).

### Unload `vf[+0x1c](1)` per kind src models
- **type 1, 0x51a1b0** (only the parts src has state for):
  U1. each child with `type == 1`: unload(child) first (exe passes `param|2`).
  U2. script object `+0x158` (typed+0x50): unrooted by 0x54c600 (no Java call), `= 0`.
      `0x477320` called next is an empty function: no event/handler from unload itself.
  U3. delete every notification of the instance (`+0x164` list = typed+0x5c).
  U4. each child: destroy it if `child.id>>16 == R.id>>16 || child.id>>16 == 0xffff`
      (same package or runtime-created; with param 1 the 0x200-flag exception never applies).
- **type 3, 0x51c630**: releases render data (meshes, buffers, owned text string only when
  `+0x16c & 1`); children untouched. src has none of that state -> no-op.
- **type 0x12, 0x526ef0**: deletes the nodes of list `R+0xd8` (class UNRESOLVED) -> no-op
  in src (`vp_destroy` already deactivates via 0x420b20 before queueing).
- other types: no src state -> no-op.

### Destroy `vf0(1)` common part (0x519500 / 0x51b5a0 / 0x526bd0 -> 0x51ebe0 -> 0x51eca0 -> 0x569890 -> 0x569b70)
  D1. the class dtor runs its unload again (idempotent after the first call).
  D2. `ResourceManager__freeId` 0x5173e0: runtime ids are recycled (src: not modelled).
  D3. `ResourceNode__clearRefs` 0x51f970 (ECX = R+0x74, asm 0x51ed2a): for every handle on
      `R+0x78`: `link = next = id = res = 0`; list head = 0.
  D4. `TreeNode__dtor` 0x569890: `while (first child) child->vf0(1)` — every child, recursive,
      no event, no package filter.
  D5. `TListNode__dtor` 0x569b70: R leaves its parent's child list.
  D6. memory freed (Game: pool).

## Changes
1. `src/game/runtime.h`, public section of `Runtime`, after `queue_event` (073 adds the timer
   API in the same place; keep both):
   ```cpp
   // 0x47e340 / 0x47b970: append a ResourceRefDLNode (0x47b090) bound to h->target.
   // Callers check h->id && h->target first (0x4ec800 / 0x4e9450).
   void queue_unload(Handle *h);
   void queue_delete(Handle *h);
   // 0x47d070 Game__flushDeferred: drain both lists until a pass does nothing.
   void flush_deferred();
   // vf[+0x1c](1) for the kinds src models (0x51a1b0 type 1; others no-op).
   void unload(Resource *r);
   ```
   private:
   ```cpp
   void destroy(Resource *r);   // vf0(1) common part, D1..D5
   std::list<std::unique_ptr<Handle>> unload_q_, delete_q_;   // Game+0xe8 / +0x134
   ```
   (`#include <list>`; unique_ptr keeps the Handle address stable for bind.)
   `~Runtime`: `unlink` every queued handle before the resources go away (INVENTED cleanup,
   same as 073's timers).
2. `src/game/runtime.cpp`:
   - `queue_unload` / `queue_delete` (0x47e340 / 0x47b970): `auto n = std::make_unique<Handle>();
     bind(n.get(), h->target);` (`bind(Handle*, Resource*)` sets `id = r->id`, like 0x40cac0),
     `push_back`. The queueDelete typed-data fetch has no src effect: skip with a comment.
   - `flush_deferred` (0x47d070): pseudocode above, with
     `std::list::front()` processed **before** `pop_front()` (script code run by
     `queue_event` may append; `std::list` keeps the node valid). Per node:
     `Handle *h = q.front().get(); if (h->id) { Resource *r = h->target; if (r) {
     if (r->type == 1) queue_event(r, nullptr, 2, Value::make_null()); unload(r);
     [delete list: destroy(r);] } unlink(h); } q.pop_front(); work++;`
     Note `destroy` already zeroed `h` (D3) so `unlink(h)` must tolerate `target == nullptr`
     (it does: it only resets fields). Generic list +0x1cc and `Game+0x234 -> 0x559f60`:
     `shortcut:` comments.
   - `unload(r)` (0x51a1b0 subset for `r->type == 1`, else return):
     U1 `for (Resource *c : std::vector<Resource *>(r->children)) if (c->type == 1) unload(c);`
     U2 `r->script = nullptr;` (`shortcut:` src VM has no root list; 0x54c600 unroot not modelled)
     U3 `r->notifications.clear();`
     U4 copy `r->children`, then for each `c`: `if (c->instance || (c->package && c->package
     == r->package)) destroy(c);` — comment: exe test `id>>16` equal or `0xffff`; src
     instance ids are 0x80000000+ (no 0xffff prefix), so `instance` stands for "runtime-created".
     Untraced/unmodelled parts (`+0x154` native instance dtor, "Undefined GameType Destructor"
     log, arrays `+0x11c/+0x124/+0x13c/+0x150`, handles `+0x128/+0x140`, loaded bit 0x51f790):
     one `shortcut:` comment.
   - `destroy(r)`:
     D1 `unload(r);`
     D3 `for (Handle *h = r->handles; h;) { Handle *nx = h->next; h->target = nullptr; h->id = 0;
        h->prev = h->next = nullptr; h = nx; } r->handles = nullptr; r->refs = 0;`
        then, because src `Notification::watcher` is a raw `Resource*` where the exe has a
        handle bound to the watcher (zeroed by D3): for every node `x` in `records_` and
        `instances_`, every `n` in `x->notifications` with `n.watcher == r` -> `n.watcher =
        nullptr` (queue_event already drops null watchers, its INVENTED comment about "no
        destroy hook" can now cite 0x51f970).
     D4 `while (!r->children.empty()) destroy(r->children.front());` (D5 of the child removes it).
     D5 `reparent(r, nullptr);` (removes r from the parent's `children`, fixes `next_sibling`).
     D2/D6: `shortcut:` the node stays allocated in `instances_`/`records_` (side tables keyed
     by `Resource*` — `src/game/car.cpp` `g_parts`/`g_cars`, `natives.cpp` controller map —
     would dangle) and ids are not recycled; upgrade when those tables get a destroy hook.
3. `src/script/natives.cpp`:
   - `rr_unload` (0x4e4500 -> 0x4ec800): keep the log; `Runtime::Handle *h = value_handle(a[0]);
     if (h && h->id && h->target) { if (h->target->type == 1) game_rt()->queue_unload(h);
     else game_rt()->unload(h->target); }`. Replace the shortcut comment.
   - `rr_destroy` (0x4e32b0 -> 0x4e9450): `if (h && h->id && h->target)
     game_rt()->queue_delete(h);` — no `detach` (the script's handle stays, it is zeroed at
     the drain). Replace the shortcut comment.
   - `vp_destroy` (0x4e6c90): keep the 0x420b20 deactivate line; replace `detach(o)` with
     `if (h && h->id && h->target) game_rt()->queue_delete(h);  // 0x4e9450`; drop the
     "no frame-end delete queue" shortcut and the "UNRESOLVED: FUN_0047b090" line (0x47b090 is
     `ResourceRefDLNode__ctor`, traced).
   - `Camera.destroy` 0x4d8fc0 stays an engine stub (no camera typed data in src).
4. `src/app/boot.cpp` `FrameLoop::frame` (after 073 merged): before the `run_timers` call add
   `if (Runtime *rt = Runtime::active()) rt->flush_deferred();  // 0x47d070 (end of sim step 0x47e050)`.
   The exe runs it after every physics sub-step and before `Game__runTimers`.

## Files
- edit: `src/game/runtime.h`, `src/game/runtime.cpp` (queues, flush, unload, destroy, dtor)
- edit: `src/script/natives.cpp` (`rr_unload`, `rr_destroy`, `vp_destroy`)
- edit: `src/app/boot.cpp` (one line in `FrameLoop::frame`)
- read only: `src/game/runtime.cpp` (`unlink`, `bind`, `reparent`, `queue_event`),
  `tasks/073-gametype-timers.md` (same list/unique_ptr pattern)

## UNRESOLVED / not in this card
- 0x559f60 (`Game+0x234 != 0` after the drain), generic list `Game+0x1cc` producer.
- Viewport unload list `R+0xd8` node class; render-data owned-string bit `+0x16c & 1` producer.
- `+0x128` handle of a game instance (typed+0x20); `+0x154` native type instance dtor.
- 23 unnamed engine callers of 0x4e9450 (notes/18 §4.3); none is a script native.

## Done when
- every ported function carries its `// 0xADDR Name` comment; untraced logic is `INVENTED:`
  (dtor cleanup), unmodelled exe parts are `shortcut:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
