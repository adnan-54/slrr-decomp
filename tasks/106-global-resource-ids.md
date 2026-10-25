---
id: 106
status: merged
branch: card/106
depends: [105]
---
# resource: global runtime ids (registry index << 16 | local), lazy package mount, int resource constants

## Goal
src treats ids relative to a base package and keeps script resource constants as a special
`Value::Resource`. The exe uses GLOBAL ids everywhere scripts can see them (notes/16, section
"Runtime ids are GLOBAL"): `id = registryIndex << 16 | local`, packages are looked up by name in
a registry and mounted on first use, and every script resource constant (`frontend:0x3EBr`) is an
int global id. Port that.

## Exe evidence (Claude, decompiled)
### 0x518940 registry find-or-append(name)  (ECX = manager [0x5db008])
    for i in 0..count-1: if slot[i].nameMatches(name) (0x51e2f0) return i;
    slot[count].init(count, name) (0x51e2a0); return count++;
### 0x517b80 slotByIndex(enc)  -> hi = enc >> 16; hi < count ? (Package::load(slot) != 0 ? slot : 0) : 0
### 0x51e310 Package::load: state 0 -> if the file is missing (0x565200) return 0 (state stays 0,
    retried next time); else parse the .rpk, register each sub-package name via 0x518940 and keep
    that index in the sub-package entry (+2), state = 2.
### 0x51e860 globalId(P, id):  lo = id & 0xffff, hi = id >> 16
    if lo == 0 or hi >= registry count -> 0
    if hi == 0 -> P.registryIndex << 16 | lo
    if hi <= P.subCount -> P.sub[hi].registryIndex << 16 | lo     (1-based sub-package table)
    else -> 0
### 0x5189e0 / 0x55cad0 resourceConstant(name, local): idx = 0x518940(name); load slot idx
    (0x51e310; failure -> 0); return slot.globalId(local)  (0x51e860 with P = that package).
    A null/empty name -> registry slot of name "" (keep: returns 0 in practice).
### 0x54b310 at class load: every opcode 0x100e (src: 0x400e "resource constant") is rewritten into
    an int constant with the value above. 0x54b900 does the same for static field initialisers.
### 0x4e0470 System.openLib returns the registry index (card 095 used the package index + INVENTED).

## Changes
1. `src/resource/manager.h` / `manager.cpp` (ResourceManager):
   - registry: `std::vector<std::string> reg_names_; std::vector<const Package *> reg_pkgs_;`
     (canonical names via the existing `canonical_package`).
   - `uint32_t registry_index(const std::string &name);`            // 0x518940
   - `const Package *registry_package(uint32_t idx);`               // 0x517b80 + 0x51e310
     not loaded yet -> `resolve_path(name + ".rpk")` (keep the name if it already ends in .rpk),
     `open(path, nullptr, &e)`; failure -> nullptr and stay unloaded (retried later).
   - `open()` registers the opened package's canonical name (find-or-append) and stores the index.
   - `uint32_t global_id(const Package &p, uint32_t id);`           // 0x51e860 (registers
     sub-package names on demand via registry_index; no mount needed for the arithmetic).
   - `uint32_t registry_index_of(const Package &p) const;`
2. `src/game/runtime.cpp` `Runtime::resolve(id, base)`:
   - `base != nullptr` -> `id = resources_->global_id(*base, id)` (0x51e860), then the global path;
   - global path: `p = resources_->registry_package(id >> 16)`, `rec = p->rpk.find(id & 0xffff)`.
   - delete the "every open package as base" loop (src invention) and `target_package`'s
     `find_open_package` use (sub-package mapping now goes through global_id).
   Records stay cached by `(Package*, local)`; `Resource::id` keeps the record's local id as today
   (do not change what natives report as `getId` here; note it under UNRESOLVED if unsure).
3. `src/script/vm.h` / `vm.cpp`: `using ResourceResolver = std::function<int32_t(const std::string
   &pkg, int32_t local)>; void set_resource_resolver(ResourceResolver f);` Opcode 0x400e pushes
   `Value::make_int(resolver(name, local))` when a resolver is installed, else the old
   `Value::make_resource` (tools without a runtime). shortcut comment: the exe rewrites at class load
   (0x54b310); resolving at execution gives the same value unless registry order matters.
4. `src/script/natives.cpp`: `natives::install` (or Runtime activation, whichever already has both
   the Vm and the Runtime) installs the resolver = `rm.global_id(*rm.registry_package(
   rm.registry_index(name)), local)` with null-package -> 0. `package_key(name)` ->
   `registry_index(name)`; `package_index(pkg)` -> `registry_index_of(*pkg)`; openLib: return the
   registry index and drop its INVENTED comment (keep the -1 failure path).

## Files
- edit: src/resource/manager.h, src/resource/manager.cpp, src/game/runtime.cpp, src/script/vm.h,
  src/script/vm.cpp, src/script/natives.cpp

## Done when
- `// 0x518940`, `// 0x51e860`, `// 0x51e310`, `// 0x55cad0` comments at the ported code
- `make -C src clean check` green. Existing checks that resolve ids with a base package must keep
  passing (they now go through global_id). If a check relied on the removed "every open package"
  heuristic, list it under `## Check impact` with the exact line and what the exe would do instead;
  do not change expected values to make it pass.
- card frontmatter: status implemented, branch name

## Check impact
None: no existing check resolves ids through `Runtime::resolve`. `resman --selfcheck`
uses `ResourceManager::lookup(0xD2)`, which keeps its relative-id scan unchanged, and
still passes (63 checks).

## UNRESOLVED
- `Resource::id` still stores the record's local id; what the exe's `getId` reports for
  a resource reached through a global id was not traced (kept as today per the card).
