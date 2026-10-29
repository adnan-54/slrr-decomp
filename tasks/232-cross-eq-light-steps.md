---
id: 232
status: implemented
branch: card/232
depends: [220]
---
# Traffic: cross path claims + ctCrossEq/ctCrossLight step + Graph::step eq/light walks

## Goal
Port the first two of the four `Graph::step` list walks (notes/37 §1-2): the active eq-cross list
(`Graph::active_eq`, T+0x90, link `Cross::active_next`) and the active light-cross list
(`Graph::active_light`, T+0x94, same link field). Each active cross updates its per-path claim
counters/blocks; lights also cycle their phase. A cross whose step returns false is unlinked.
Lists are filled by `cross_activate` (card 219) when a lane goes live. In today's vmboot no lane goes
live, so this is dead code there; **race/city numbers may move once lanes go live: report any check
delta.** Generic, no map data.

## Exe evidence (notes/37)
Per-path state at cross+0x58: `{int32 count (+0); int32 block (+4)}`, one per path. Path record:
`CrossPath::a` = entry link (byte +0), `b` = exit link (byte +1), `c` = priority (ushort +2).
All a/b compares are unsigned bytes; `block < prio` is a signed int compare.

### 0x415b40 ctCross__claimPath(cross, i)
```
if (state[i].count == 0) {
    a = path[i].a; b = path[i].b; prio = path[i].c
    for j in 0..paths-1:
        if (state[j].block < prio) {
            aj = path[j].a; bj = path[j].b
            if (aj != a) {
                if (bj == b) state[j].block = prio
                else {
                    inA = (a < b) ? (a < aj && aj < b) : (a < aj || aj < b)
                    inB = (a < b) ? (a < bj && bj < b) : (a < bj || bj < b)
                    if (inA != inB) state[j].block = prio
                }
            }
        }
}
state[i].count += 10
```
### 0x415b00 ctCross__pathBlocked(cross, i)
```
if (state[i].block != 0) return true
claimPath(i); return false
```
### 0x415c40 ctCrossEq__step(cross) -> bool
```
for i: state[i].block = 0
r = live_refs                                 // +0x38
if (r < 0) {
    any = false
    for i:
        if (state[i].count > 4) state[i].count = 5
        state[i].count -= 1
        if (state[i].count < 1) state[i].count = 0
        else { any = true; for j: if (path[i].b == path[j].b) state[j].block = 0x3ea }
    if (any) return true
    live_refs = 0; return false
}
if (r > 0) {
    for i:
        if (state[i].count > 4) state[i].count = 5
        c = state[i].count; state[i].count = 0
        if (c > 1) { claimPath(i); state[i].count = c - 1 }
    return true
}
return false
```
### 0x415d60 ctCrossLight__step(cross) -> bool
New light fields: +0x64 `remain` (float, ctor 0x4154b0 = 0), +0x68 `cur` phase (int, ctor = 0).
Existing: +0x6c `phase_time`, +0x70 `phase[link]`, +0x74 `phase_count`; links +0x14 (node = lane id).
```
ctCrossEq__step(cross)                        // result ignored
r = live_refs; if (r < 1) return r != 0
if (remain <= 0) {
    a = 0.1f, b = 0.1f; na = 0, nb = 0
    for k in links:
        L = lane of links[k].node; f = L.kc8 / L.kcc            // lane +0xc8 / +0xcc
        if (phase[k] == cur) { a += f; na++ } else { b += f; nb++ }
    if (na > 1) a /= na;  if (nb > 1) b /= nb
    if ((a + a) / b + phase_time < g.sim_time) {
        phase_time = g.sim_time; remain = 4.0f
        queue "change_lamp" to cross+0x44 (code 0x10)            // shortcut, see Files
    }
} else {
    remain -= g.sim_time - phase_time; phase_time = g.sim_time
    if (remain <= 0) { remain = 0; cur = (cur + 1) % phase_count
                       queue "change_lamp" to cross+0x44 (code 0x10) }
}
if (remain <= 0) { for i in paths: if (phase[path[i].a] != cur) state[i].block = 0x3ec }
else             { for i in paths: state[i].block = 0x3eb }
return live_refs != 0
```
### 0x41b290 Traffic__step walks (eq first, then light, then cars, then lanes)
```
p = &T+0x90; while (*p != T) { if (eq_step(*p)) p = &(*p)->next_0x54; else { n = (*p)->next; (*p)->next = 0; *p = n; } }
p = &T+0x94; same with light_step, link +0x60
... live cars (card 233) ... active lanes (already in src)
```

## Files
- edit: src/game/traffic.h
  - `Cross::path_state`: replace `std::vector<uint64_t>` by `struct PathState { int32_t count = 0, block = 0; }; std::vector<PathState>` (update `cross_init`'s assign).
  - `Cross`: add `float light_remain = 0;  // +0x64` and `int32_t light_cur = 0;  // +0x68` (ctor 0x4154b0).
- edit: src/game/traffic.cpp
  - new static `cross_claim_path(Cross&, size_t i)` 0x415b40, `cross_path_blocked(Cross&, size_t i)` 0x415b00
    (blocked is used by card 233; keep it non-static-unused-warning free: declare it in the anonymous
    namespace and use `[[maybe_unused]]` or add the declaration to traffic.h if needed).
  - new `cross_eq_step(Graph&, Cross&)` 0x415c40 and `cross_light_step(Graph&, Cross&)` 0x415d60.
    Lane lookup via the existing `lane_at(g, link.node)`; INVENTED: a null lane skips the link.
    "change_lamp": `// shortcut: Game__queueEvent(cross+0x44, 0x10, "change_lamp") not delivered (no sink in src)`.
  - `Graph::step`: before the lane walk add the eq walk over `active_eq` and the light walk over
    `active_light` (1-based indices into `cross_eq` / `cross_light`, `kLiveEnd` = end, link
    `Cross::active_next`, same unlink rule as the lane walk). Keep a shortcut comment for the heap pop,
    the live-car walk (card 233), the pseudo-car prologue and the T+0xc8 drain.
- read only: notes/37 §2.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`/`shortcut:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- report: `vmboot --race` time and `vmboot --city` check count before/after (capped: `ulimit -v 1500000; timeout 300`)
- card frontmatter: status implemented, branch name
