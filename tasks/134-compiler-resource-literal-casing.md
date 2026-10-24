---
id: 134
status: implemented
branch: card/134
depends: [133]
---
# compiler: resource literal package name through the name registry

## Goal
A resource literal `cars.racers.baiern:0x102r` is stored in the shipped class as
`cars\racers\Baiern.rpk` #0x102: the exe compiler resolves the literal to a global id through
the resource name registry and writes the registry slot's spelling back (notes/20 "Resource
literal casing"). Port that: compile_unit takes an optional registry; classdump's
`--compile-check` fills it the way the shipped GameLogic did before the classes were compiled.
Fixes the 68 "resource literal package casing" bodies (Baiern/Duhen/Einvagen/Enula/Focer/MC/
Nonus/Prime/SuperDuty part scripts, TimeAttackTest).

## Exe evidence (cleaned pseudocode)

### 0x54b310 constant-pool rebuild walk (called by JVM::compileHook 0x5557c0 per tree)
```c
// for each tree node n (pre-order), old = parse-time pool:
op = n.op;
if ((op & 0xf000) == 0x1000) {                 // constant-carrying nodes
    if (op != 0x100e || n.cidx == -1) goto next;
    e = old[n.cidx];                            // type 3 = resource {+8 local, +0xc name idx}
    if (!e || e.type != 3) gid = 0;
    else {
        name = old[e.nameIdx] (type 0 string) or NULL;
        gid = 0x55cad0(name, e.local);          // = 0x5189e0(name, local)
    }
    n.cidx = 0x54b170(gid);                     // new pool entry
} else if ((op & 0xf000) == 0x4000 && n.cidx != -1) {
    n.cidx = 0x54b260(old string, len);         // strings re-added as-is
}
```

### 0x5189e0 registry resolve (name, local) -> global id
```c
idx = 0x518940(name);
if (idx == 0xffff) return 0;
if (idx >= count) return 0;
if (!0x51e310(slot[idx])) return 0;             // Package::load fails (missing file)
return slot[idx].0x51e860(local);               // hi 0: idx << 16 | local; local 0 -> 0
```

### 0x518940 find-or-append
```c
for (i = 0; i < count; i++)
    if (_stricmp(slot[i].name /* +0xc */, name) == 0) return i;   // 0x51e2f0 -> CRT 0x57c11e
slot[count].init(count, name);                  // 0x51e2a0: strncpy(+0xc, name, 0x40) as given
return count++;
```

### 0x54b170 add resource constant from a global id
```c
pkg = 0x517b80(gid);                            // slot gid >> 16
s = 0x54b260(pkg->name, strlen(pkg->name));     // string entry with the REGISTRY spelling
if (0x54b560(s, gid & 0xffff) == -1)            // dedup on (string, local)
    add {vtbl Rid, type 3, local = gid & 0xffff, name = s};
```
gid 0 (missing package) -> slot 0 = `system.rpk`, local 0.

### Who registered first (shipped data)
GameLogic.java 345-360 (bin/the game/sl/Scripts/game/src): `System.rpkScan(d)` for
`parts\engines\`, `parts\`, `cars\racers\`, `cars\fake_racers\`, `maps\`,
`sl\Scripts\game\CareerEvents\`, then every directory `multibot\maps\<name>\`.
System.rpkScan (System.class body 38): `name = ff.first(dir + "*.rpk"); while (name) {
openLib(dir + name); name = ff.next(); }`; openLib 0x4e0470 = `0x518940(string)` unchanged.
So those slots hold `dir` as the script wrote it + the on-disk file name.

## Files
- edit: src/script/compiler/emit.h, src/script/compiler/emit.cpp (parse_resource stays; the
  ResourceLit case / add_resource maps the name through the registry), src/tools/classdump.cpp
  (compile-check builds the registry).

## How
1. emit.h: `struct NameRegistry { std::vector<std::string> names; std::function<bool(const
   std::string &)> loads; };` (exe 0x518940 slots; `loads` = Package::load 0x51e310 succeeds).
   `compile_unit(..., NameRegistry *reg = nullptr)`; null = keep the name as written (today).
2. emit.cpp, at the ResourceLit codegen (not in the dry walk): `i` = first slot with
   case-insensitive equal name (`strcasecmp`), else append the literal name; if `!loads(names[i])`
   use `names[0]` and id 0; else `names[i]` and the literal id. Then add_resource(name, id).
3. classdump --compile-check: base registry = {"system.rpk"} + for each scan dir above (in that
   order; multibot\maps\ subdirs sorted, INVENTED: FindFile order) every `*.rpk` file in
   `<game dir>\<dir>` with dir as written + the on-disk file name. `loads` = the file exists,
   case-insensitive per path component, under the game dir (the `bin/the game` argument).
   Give each unit a fresh copy of the base registry (INVENTED: the exe's compile order across
   files is unknown; literals registered within one unit persist for that unit).

## Done when
- `make -C src -j4 check` green (compile-check stays report-only).
- `classdump --compile-check "bin/the game"`: baseline `bodies: total 7096, identical 6821;
  CONS exact 2681 of 2809`; after: bodies identical >= 6889 (the 68 casing bodies), no
  `res:...baiern.rpk`-style lower-case names left, `--show TimeAttackTest` has `res:system.rpk#0`.
- `// 0xADDR` comments on the ported steps; INVENTED: marks as above.
- card frontmatter: status implemented, branch name; record the new numbers in notes/20.
