---
id: 119
status: draft
branch:
depends: []
---
# runtime: load a record's `script ...` class lazily, on first typed-data need (0x51fc50/0x51fcc0), not at package mount

## Goal
`Runtime::resolve` currently sets `r->script_class` for **every** record it creates
(`runtime.cpp:337`), and `script_class_of` loads the `.class` file
(0x55aca0 -> 0x55bec0, card 114). Because `mount_package` resolves all records of a
package at mount, every type record's class is loaded eagerly.

The exe does **not** do that. Package mount (0x51e310 -> 0x51e6b0 -> 0x518880 ->
0x518410) only creates the node, links it into its parent and marks it *not loaded*
(`0x51f790(0)`). The record's RSD text is parsed, and its `script <dir>\Name.class`
ref is turned into a class, only when the node's typed data is first requested:
0x51f4f0 `Resource__getTyped` -> 0x518a80 `ResourceNode__requestLoad` ->
vtable+0x28 (vf10) 0x51fc50 -> 0x51fcc0 (record loader). After that the node stays
loaded and the parse result is reused (`+0x94` bit 0, `+0x98`).

Change src to match: drop the eager `script_class_of` in `resolve`, add a cached
`ensure_script_class` that runs `script_class_of` once, at the two places where src
actually needs the class (`instantiate` for a scripted instance and
`script_instance`). Mount stops loading class files.

## Exe evidence (cleaned pseudocode, renamed fields)
`node+0x88` = record text/data pointer, `node+0x8c` = its length, `node+0x94` = flags
(bit 0 = typed data loaded), `node+0x98` = last load result, `node+0x7c` = record type.
`1.0f`/`1000.0f` below are `0x3f800000`/`0x447a0000`.

### 0x51e310 Package::load — mount only, no rsd parse
```
for each record blob in the RPAM chunk:
    id = 0x51e860(record[0]); 0x51e6b0(record, id, &descriptor, 0);
```
### 0x51e6b0 record mount
```
... 0x518880(own_id, superid, &descriptor, extra, 0) ...
```
### 0x518880 register
```
if (registry has not loaded this package) 0x51e310();     // mount on demand
node = 0x518410(superid, descriptor, extra, 0);
if (node) 0x51f790(0);                                    // setLoaded(false)
```
(ECX=node is implied by the thiscall; Ghidra's print omits the load.)
### 0x518410 Resource__createInstance
Creates the typed node by descriptor type (type 8 -> 0x51faf0), resolves the
parent, tail-inserts it into `parent+0x50/+0x58`, then
`if (*(int*)(&DAT_005db2a8 + node->type*4) != 0) 0x51f440(parent);` — 0x51f440 is a
ref-link helper (`0x40cac0 ResRefLink.set`), not a loader. **No 0x51fcc0 call.**
### 0x51f4f0 Resource__getTyped — first use
```
if (!(param_1 & 0x40000000)) {
    if (node->flags(+0x94) & 1) { /* clamp +0x98, already loaded */ }
    else if (!(node->flags & 0x10000000) && ...)
        0x518a80(node, node->(+0x9c), param_2, param_3);   // requestLoad
}
...
if (node->flags & 1) return 0;             // loaded
return 0x80000001;                         // still streaming / not loaded
```
### 0x518a80 ResourceNode__requestLoad (ECX = ResourceManager)
```
if (node->(+0x8c) == 0) { 0x51f790(1); node->(+0x98) = 1.0f; return; }
if ((param_2 == 0.0f && param_3 == 0.0f) || node->(+0x82) == -1) {   // synchronous
    ...
    if (node->(+0x9c) <= 1000.0f && node->(+0x9c) != 1000.0f) {
        vf10 = *(code**)(*node + 0x28);
        node->(+0x9c) = 1000.0f;
        vf10(node->record_data /*+0x88 or blob from 0x51e500*/, 1.0f);
        node->(+0x9c) = 1.0f;
    }
    ...
}
else { /* queue streaming request; node stays unloaded this call */ }
```
### 0x51fc50 ResourceType_Game vf10 (vtable +0x28) — the only 0x51fcc0 caller found
```
if (!(node->flags(+0x94) & 1)) {
    f = 0x51fcc0(node, data, node->(+0x8c) /*length*/, quality /*1.0f*/);
    0x51f790(1);                  // setLoaded(true)
    node->(+0x98) = f;
    return f;
}
node->(+0x98) = 1.0f; return 1.0f;          // second request: cached, no reparse
```
### 0x51fcc0 record loader (the rsd parser / class ref)
For the `script` key it does (already ported semantics, card 114/notes/03):
`script <dir>\Name.class` -> 0x55aca0(dir, Name); `script <FQN>` -> 0x55add0(FQN);
empty -> "Missing class: %s". It stores the class at `node+0x10` (src
`Resource::script_class`). It is reached **only** through vf10 0x51fc50.

### 0x4da620 GameRef.create (native) -> 0x523c80, first need of a type's class
```
0x523c80(parent, gametype, ..., alias):
    inst = 0x5188e0/0x518880(...);            // create the game instance
    ... 0x51f790(1) ...
    typed = (**(code**)(*inst + 0xc))(1.0f);  // vf3 typed data; record parsed here
    gt = 0x468b00(0x80000000, 1.0f, 0, 0);    // GameType typed data (0x468b00 not traced)
    if (typed->(+0x44) == 0) log "create: Undefined GameType Constructor! ..."
```
So create needs the type's parsed data (class + constructor) — this is the first
use after mount for every type the game instantiates.

## Files
- edit: `src/game/runtime.h`
  - in `struct Resource`, next to `std::string script_class;` add
    `bool script_class_checked = false;   // node+0x94 bit0: typed data parsed (0x51f790)`
  - in the private section next to `script_class_of` declare
    `void ensure_script_class(Resource *r);`
- edit: `src/game/runtime.cpp`
  - `Runtime::resolve` (line ~337): **delete** `r->script_class = script_class_of(...)`
    and replace it with a comment:
    `// 0x51e310 mount only creates/link nodes (0x518880 does 0x51f790(0)); the`
    `// record text is parsed on the first typed-data request (0x51f4f0 -> 0x518a80`
    `// -> vf10 0x51fc50 -> 0x51fcc0). See ensure_script_class().`
  - add, right after `script_class_of` (cites the whole lazy path):
    ```cpp
    // 0x51fc50 (vf10 +0x28) / 0x51fcc0: the record's `script ...` ref is resolved
    // once, on the first typed-data request (0x51f4f0 -> 0x518a80), never at
    // package mount. INVENTED: src has no typed-data layer, so the deferred
    // product is just the class; script_class_checked is the node+0x94 bit0 flag.
    void Runtime::ensure_script_class(Resource *r)
    {
        if (!r || r->script_class_checked) return;
        r->script_class_checked = true;   // mark first: script_class_of may load classes
        if (r->package && r->record)
            r->script_class = script_class_of(*r->package, *r->record);
    }
    ```
  - `Runtime::instantiate` (line ~487), replace
    `if (type) r->script_class = type->script_class;` with
    ```cpp
    // 0x523c80: create fetches the type's typed data (classes) before the ctor.
    if (type) {
        ensure_script_class(type);
        r->script_class = type->script_class;
    }
    ```
  - `Runtime::script_instance` (line ~557): after `if (r->script) return r->script;`
    add `ensure_script_class(r);` (0x4daf90 getScriptInstance hits the same typed-data
    path).
- read only: `src/script/natives.cpp` (`gr_is_scripted` line ~2310 reads
  `r->script_class` for type 8; see UNRESOLVED — do **not** edit it in this card).

## Done when
- the removed/added code cites `// 0x51e310`, `// 0x518880`, `// 0x51f4f0`,
  `// 0x518a80`, `// 0x51fc50`, `// 0x51fcc0` and `// 0x523c80` as above
- `make -C src clean check` green; no new unit tests or hand-made expected values
- run `tools/vmboot-capped.sh --selfcheck | tail -3` and report the stop line
  (must still be Init ctor + 600 frames, 0 failed threads); temporarily print/count
  `script_class_of` calls during `mount_package` and report that it is now 0 for
  records that are never instantiated (remove the print before commit)
- card frontmatter: status implemented, branch name

## UNRESOLVED (do not invent around these)
- Full caller list of 0x51fcc0 could not be enumerated (the rea callers/xref tools
  were not permitted in this session). It was reached structurally: the decompiled
  bodies of 0x51e310, 0x51e6b0, 0x518880 and 0x518410 contain no call to it; the
  only caller visible is vtable slot +0x28 -> 0x51fc50. If another direct caller
  exists, it would still be a first-use path, not the mount loop.
- 0x468b00 (GameType typed-data/type-entry lookup used by create and
  `isScripted(type 8)` 0x4db460) is not traced (notes/13 §599). It is assumed to
  go through the same getTyped/requestLoad path; not verified.
- `gr_is_scripted` (natives.cpp ~2310) still reads `r->script_class` directly. A
  type-8 record whose typed data was never requested (never created) now has an
  empty class there and reports unscripted, whereas the exe reaches the class via
  0x468b00. Boot/Catalog iterate types that GameLogic has already created, so the
  shipped path should not hit this; route it through `ensure_script_class` (make it
  public or add a caller) in a follow-up if it does.
- The streaming branch of 0x518a80 (record not local, queued via 0x51e5c0) keeps
  the node unloaded for that call and getTyped returns 0x80000001; src has no
  asynchronous resource layer, so `ensure_script_class` always parses
  synchronously. Shipped packages are local, so the observable result is the same.
