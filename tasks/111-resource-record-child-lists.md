---
id: 111
status: todo
branch:
depends: []
---
# resource: link a package's records into parent child lists (getFirstChild/getNextChild)

## Goal
`ResourceRef.getFirstChild`/`getNextChild` (0x4e3510/0x4e35e0) already have
correct natives in `src/script/natives.cpp` (they read `Resource::children[0]` /
`next_sibling`), but record-backed resources never get those lists filled:
`Runtime::resolve` creates a node and its `parent`, then stops. The exe links
every record of a package into its super-id parent at package mount
(`Package::load` runs `0x51e6b0 -> 0x518410` for each record, and 0x518410 tail-
inserts the node into the parent's child list). Add that mount step so e.g.
`GameLogic.initVehicleTypes`' `VEHICLETYPE_ROOT = cars:0x1000r` +
`getChildNodes()` sees the vehicle-type records (GameLogic.java:592).

Scope note (answered while tracing): the exe does **not** scan the directory for
`*.rpk`. `sl.rpk` and `particles.rpk` are opened by literal name in 0x4b0910;
all other packages come from the sub-package name registry and are opened lazily
by `0x517b80 -> 0x51e310` on first id reference. No `*.rpk` literal exists in the
string table (`analysis/strings.tsv` only has `\*.class` and `\src\*.java`).
Therefore "mount all records" belongs to the existing lazy `registry_package`
path, not to a startup scan.

## Exe evidence (cleaned pseudocode)

### 0x4e3510 ResourceRef.getFirstChild (ECX = ResRefLink*)
```
FUN_004b1af0(param_1, &local_8);        // first script arg = receiver
FUN_004ecc70(local_8, DAT_007a2620);    // receiver int field -> owner node (0 if none)
link = new(0x10); link[0..3] = 0;
id = FUN_004e9b30(link);                // ECX = link
if (id != 0) return FUN_004e7320(param_1, link);   // wrapper by node type
free(link); return 0;                   // no child -> null
```

### 0x4e9b30 (ECX = link)
```
if (link == 0) return 0;
owner = link[3];
if (owner == 0) return link[2];
first = *(int*)(owner + 0x30);
if ((*(int*)(owner + 0xc) != 0) && (*(int*)(first + 4) != 0) && (first != 0))
    FUN_0040cac0(first);                // ResRefLink.set: cache target, id from node+0x80
return link[2];                         // link id (0 when not set)
```

### 0x4e35e0 ResourceRef.getNextChild — identical, calls FUN_004ea610
### 0x4ea610 (ECX = link)
```
if (link == 0) return 0;
owner = link[3];
next = *(int*)(owner + 4);
if ((owner != 0) && (next != 0) && (*(int*)(next + 4) != 0))
    FUN_0040cac0(next);
return link[2];
```

### 0x518410 Resource__createInstance — parent link at mount
After creating the typed node `iVar1` (switch on descriptor type, `*param_2`):
```
if (param_5 == 0) {                    // no explicit parent
  if (param_1 == 0) {                  // super id 0 -> engine root
    param_5 = 0;
    if (*(int*)(*(int*)(engine + 0x11c) + 4) != 0) param_5 = *(int*)(engine + 0x11c);
  } else {
    param_5 = FUN_005183a0(param_1, *param_2, 0);   // resolve super id (mounts its package)
  }
  if (param_5 == 0) log("Unregistrable node: %s:%08X (invalid parent %s:%08X)");
}
*(int*)(iVar1 + 0x24) = param_5;        // parent
iVar2 = *(int*)(param_5 + 0x58);        // tail
*(int*)(iVar2 + 4) = iVar1;             // tail->next = node
*(int*)(iVar1 + 4) = param_5 + 0x50;    // node->next = list head
*(int*)(iVar1 + 8) = iVar2;             // node->prev = tail
*(int*)(param_5 + 0x58) = iVar1;        // tail = node
```
So: append at tail, child order = record order in the .rpk.

### 0x51e310 Package::load (traced in notes/16)
For each record blob entry: `id = FUN_0051e860(record+0)` (super id remap),
`FUN_0051e6b0(record, id, &descriptor, 0)`, which ends in
`FUN_00518880(own_id, superid, &descriptor, 0, extra, 0)` and then
`FUN_00518410(superid, &descriptor, extra, 0, 0)`. A package's records are
therefore all mounted, in file order, when the package is loaded.

## Changes (3 files max; keep record ids local as today)
1. `src/game/runtime.h` — in `Runtime`, declare private
   `void mount_package(const Package *p);` and
   `std::vector<const Package *> mounted_;`.
2. `src/game/runtime.cpp` — add `// 0x51e310 Package::load / 0x518410` before
   `Runtime::resolve`:
   ```cpp
   void Runtime::mount_package(const Package *p)
   {
       if (!p) return;
       for (const Package *m : mounted_) if (m == p) return;
       mounted_.push_back(p);   // mark first: parent resolution re-enters resolve
       // 0x518410: every record of a package is mounted at load.
       // INVENTED: create all nodes first. The exe resolves the super id only
       // against already-registered nodes and logs "Unregistrable node" when it
       // is missing; src's resolve() creates a forward-referenced parent on
       // demand. Shipped packages list parents before children, so the result
       // is the same; only a malformed forward reference would differ.
       for (const RpkRecord &rec : p->rpk.records) resolve(rec.id, p);
       // 0x518410 tail insert: child order = record order in the .rpk.
       // shortcut: order across packages is mount-completion order, not the
       // exe's per-record interleaving inside 0x51e6b0 (not fully traced).
       for (const RpkRecord &rec : p->rpk.records) {
           Resource *r = resolve(rec.id, p);    // cached by the loop above
           if (!r || !r->parent) continue;
           std::vector<Resource *> &ch = r->parent->children;
           if (!ch.empty()) ch.back()->next_sibling = r;
           r->next_sibling = nullptr;
           ch.push_back(r);
       }
   }
   ```
   In `Runtime::resolve` (runtime.cpp:274), split the current one-liner
   `const RpkRecord *rec = target ? target->rpk.find(id & 0xffffu) : nullptr;`
   so that `if (target) mount_package(target);` runs **before** the
   `find`/`if (!target || !rec) return nullptr;` (0x517b80 mounts the package
   on any reference to its slot). The existing cache lookup then returns the
   node created during the mount; recursion into other packages still goes
   through `registry_package` as today. Do not change `parent_id`/`parent`
   semantics.
3. `src/script/natives.cpp` — read only. `rr_get_first_child` (line 1803),
   `rr_get_next_child` (1814) and registry rows 7682/7683 already match
   0x4e3510/0x4e35e0 and need no edit. `rr_get_parent`/`rr_get_parent_id` are
   out of scope (card 106 owns global ids).

Do not add a new helper or rework `instantiate`/`reparent`/`define_script_resource`
(they already maintain `children`/`next_sibling`); the append above is local to
record mounting.

## Done when
- `mount_package` cites `// 0x51e310` and `// 0x518410`; untraced logic is `INVENTED:`
- `make -C src clean check` green (vmboot --selfcheck exercises the GameLogic
  mount path); no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

## UNRESOLVED (do not invent around these)
- Field-level list shape: 0x518410 inserts via parent+0x50/+0x58 while
  `getFirstChild` reads `*(owner+0x30)`; the sentinel init in 0x51eaa0/0x41f880
  does not obviously make 0x30 the same slot. Functional conclusion (0x30 =
  first child, node+4 = next sibling) is confirmed by the natives and by
  scripts using getChildNodes(), but the raw offset reconciliation is not.
- The `*(int*)(node+4) != 0` validity test in 0x4e9b30/0x4ea610 is unexplained
  (raw line above); it does not affect the null/next semantics modelled here.
- Cross-package child ordering (0x51e6b0 interleaving) not traced end-to-end.
- Engine root node `*(engine+0x11c)` is not modelled: super-id-0 records keep
  `parent = nullptr`, so `getParent()` on a top-level record returns null
  where the exe would return the engine root.
- No directory scan for `*.rpk`: the string table and 0x4b0910 show explicit
  sl.rpk/particles.rpk plus lazy registry mounts, but FindFirstFileA callers
  were not enumerated (tool access denied), so "no scan anywhere" is not
  proven beyond the string/xref evidence.

## Claude gate (overrides the scope note above)
- There IS a scan, but in script: `GameLogic()` (sl/Scripts/game/GameLogic.java ~347) calls
  `System.rpkScan("cars\\racers\\")` etc. (script method in System.class: FindFile `*.rpk` +
  `System.openLib`), then `initVehicleTypes()`. Our VM runs that bytecode already.
- So mounting on `resolve` alone is NOT enough: `cars:0x1000r.getChildNodes()` resolves
  cars.rpk, never Axis.rpk. The exe links records at package load (openLib -> 0x518940 ->
  0x517b80 -> 0x51e310). Make `mount_package` public and ALSO call it where
  `System.openLib` (natives.cpp, the openLib native) loads/gets a package for the running
  Runtime (`game_rt()`), so every opened .rpk links its records into their parents.
  Keep the resolve-time call too (lazy mounts by name). natives.cpp may be edited for this.
- Report the new `tools/vmboot-capped.sh --selfcheck | tail -3` stop line, and how many
  children cars:0x1000 got (temporary print is fine, remove before commit).
