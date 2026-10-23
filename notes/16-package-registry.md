# Package / class registry (`src/resource/manager.{h,cpp}`, `src/tools/resman.cpp`)

Links .rpk+.rdb resources to the `.class` units and `.cfg` files their record data names.
Formats: notes/01-rpk-rdb-format.md. VM/class format: notes/03-script-vm.md.

## Resource id scheme

- A record id (and `superid`) is `package << 16 | local`, 16 bits each.
- `package == 0` means the package the id was read from (self-reference).
- `package == n > 0` selects entry `n-1` of that package's own sub-package table
  (the 0x40-byte entries at 0x10 in the .rpk; index 1-based).
- Consequence: ids are only meaningful relative to a base package. `lookup(id)`
  therefore tries every open package as base and returns all hits. E.g. Axis
  `0xD2` is Axis-local; `0x00010005` read from Axis means Axis's sub-package 1
  (`system.rpk`) local 5, which resolves to the open `system` package.
- The next superid in a chain is interpreted in the namespace of the package
  that owns the current record, so a chain can hop packages.

## Package matching

- Open packages are keyed by `mount`: lower-case, `/` -> `\`, `.rpk` stripped,
  trailing `\`. Full opened path is kept; matching is by path-boundary suffix,
  not just file name (`...\cars\racers\Axis\` vs `cars\racers\Axis.rpk`).
- This is how sub-package table entries ("system.rpk", "parts.rpk",
  "parts\\engines\\X.rpk") find an already-open archive.
- External packages are not auto-mounted: a chain ends with
  `external=true` + the sub-package path as written.

## Record data references

The .rpk embeds the `.rsd` text; the registry parses these lines per record:

    script <class file path>      e.g. cars\racers\Axis_data\scripts\Axis_FL_door.class
    script <FQN>                  e.g. java.util.Config (no separator / .class -> class name)
    native <handler> [file]       e.g. native part cars\racers\Axis_data\scripts\FL_door.cfg

- `file` refs (path separator or `.class`/`.cfg` suffix) are resolved on disk;
  FQNs and handler-only natives (camera/player/ground/trigger) stay name-only.
- Resolution tries each package's directory and its ancestors, plus cwd. The
  ancestor walk makes root-relative refs work without configuring a game root.
- Case-insensitive component lookup is required, not cosmetic: records use both
  `Axis_data` and `Axis_Data` (e.g. 0xD2 vs the Axis_200S car record) while the
  shipped directory is `Axis_data`; Windows paths were case-insensitive.

## Classpath rules

- Every TUFA unit in a referenced file is registered by `self_class` FQN;
  one file may hold several units (e.g. Controller.class also defines
  `java.io.controlconfig`). Repeated paths load once; repeated FQNs are skipped
  and counted as duplicates.
- `link()` resolves each unit's `super_class` and `interfaces` against the
  registry by name. Matches become pointers and count as internal links; names
  not in the loaded set stay null and count as external/missing. Only classes
  reachable through record references are loaded, so most engine superclasses
  are expected-missing here.

## Build and self-check

From the repo root (`/root/slrr-re`):

    g++ -std=c++17 -Wall -Wextra -Wpedantic -Isrc \
      src/tools/resman.cpp src/resource/manager.cpp src/resource/rpk.cpp \
      src/resource/rdb.cpp src/parts/cfg.cpp src/script/classfile.cpp \
      src/script/vm.cpp src/script/natives.cpp -o src/build/resman
    ./src/build/resman --selfcheck

CLI: `resman [--resolve ID] RPK RDB [RPK RDB ...]` lists packages/classes or
prints a record's chain, references, class-unit and cfg summaries.

Self-check over `bin/the game/system.rpk+.rdb` + `cars/racers/Axis.rpk+.rdb`:
**PASSED, 48 checks, 0 failures** (exit 0).

    counts: packages=2 records=373 references=145 class_files=71 class_units=72
            cfg_files=70 unresolved=4 empty=0 failures=0
            internal_links=4 external_links=68

- 2 packages, 373 records (33 + 340); 145 class/cfg file refs, 0 parse failures.
- 71 files -> 72 class units; 70 cfg files parse; no zero-byte placeholders.
- Part 0xD2 ("FL", type 8 typ_game_object): chain 0xD2 -> Axis 0x7 ">Stock" ->
  external parts.rpk local 0xf242. Script Axis_FL_door.class parses as
  `java.game.cars.Axis_FL_door extends java.game.parts.bodypart.FrontDoor` with
  `<init>(I)`; FL_door.cfg gives 1 body (36 kg, _phys_FL_door.scx), 1 each
  render/mesh/texture/click, 3 slots (slot 4 has 3 attaches), 1 stockpart, eof.
- VM registers all 72 FQNs and `find_method` resolves Axis_FL_door.<init>(I).
- 4 internal superclass links (controlconfig->Controller; Axis_200S/200XT/ZX360
  ->Axis_models); the other 68 superclasses are external to the opened set.

## Gaps / observations

- External sub-packages (parts.rpk, cars.rpk, racers.rpk, ...) are not mounted by
  the registry, so chains stop there by design; the self-check reports them.
- **Observation (inference):** four referenced `.class` files do not exist at all
  and have only `.java` sources next to them: `Axis_L_taillights_dark.class`,
  `Axis_R_taillights_dark.class`, `kit_200XT_body.class`, `kit_ZX360_body.class`.
  The self-check confirms each `.java` is present and counts the `.class` refs as
  unresolved, not as parse failures. This matches notes/03 (`JVM::compileSource`
  compiles missing/stale sources at runtime): a loader must fall back to source
  compilation for such parts instead of failing the package load. Not verified
  against the exe's loader path here.

## Runtime ids are GLOBAL in the exe (verified 2026-10-23) — src diverges
- Name registry (ResourceManager singleton [0x5db008]): `0x518940(name)` find-or-append returns the
  registry index (count at +0x166740, package slots at +0x1667b4 + idx*0x70); packages load lazily
  (0x517b80 -> Package::load 0x51e310 on first reference; a missing file leaves state 0 and is retried).
- `Package::load` registers every sub-package name of the .rpk in the registry and stores that index in
  its sub-package table (entry+2), and registers records via 0x518880 with ids remapped by 0x51e860.
- 0x51e860(id) in package P: hi = 0 -> `P.registryIndex << 16 | local`; 0 < hi <= nSub -> `sub[hi].
  registryIndex << 16 | local`; out of range -> 0; local 0 -> 0. So every runtime id is
  `registryIndex << 16 | local`, independent of who reads it.
- Script resource constants (`frontend:0x3EBr`, CONST type 3 = {+8 local, +0xc name entry}): at class
  load 0x54b310 walks the code and rewrites each 0x100e instruction into an int constant
  `0x55cad0(name, local)` = 0x5189e0: idx = registry(name) (loads it), then P.0x51e860(local). Static
  initialisers read them through 0x54b900 the same way. Scripts therefore only ever hold global ints.
- src today: the VM pushes `Value::Resource{name, local}` for opcode 0x400e (int natives reject it),
  `Runtime::resolve(id, base)` reads hi as a sub-package index of `base` (or of every open package when
  base is null), and `package_key` only finds already-open packages. Card 106 (design) fixes this.
