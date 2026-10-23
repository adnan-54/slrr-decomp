---
id: 114
status: todo
branch:
depends: [113]
---
# runtime: load a type record's `script <dir>\Name.class` on demand (0x51fcc0 -> 0x55aca0 -> 0x55bec0)

## Goal
Car vehicle-type records (e.g. `Zed_VT` in cars/racers/Zed.rpk) carry
`script cars\racers\Zed_Data\scripts\Zed_VT.class` (declared package `java.game.cars`).
`Runtime::script_class_of` (src/game/runtime.cpp ~261) only finds classes already in the
ClassPath by path, so `type->script_class` is empty, `GameRef.create` makes no script
object and boot stops at `GameLogic.initVehicleTypes line 605: null receiver`.
The exe loads the class file from that path when it is not loaded yet.

## Exe evidence
- 0x51fcc0 record loader (notes/03 "Class load path"): `script <dir>\Name.class` ->
  0x55aca0(dir, Name); `script <FQN>` (no .class) -> 0x55add0(FQN).
- 0x55aca0(dir, Name) (Claude, decompiled):
  ```
  key = dir + "\" + Name;  replace every '\' in key by '.'
  for each loaded class c (hash buckets, 0x200 of them):
      if (strcmp(pathKeyOf(c) /*0x54b660(c->+0x3c) -> 0x54b970*/, key) == 0) found = c
  if (!found) 0x55bec0(dir, Name)      // JavaMachine::loadClass: read <dir>\Name.class,
                                       // add units, register package dir (0x55f780)
  ```
- 0x55bec0 is already ported as the tail of `jvm::load_class` in src/script/packages.cpp
  (~264): path `dir\base.class`, `disk_path`, `classes.add_file`, `vm.add_class` for new
  units, `tree.add(dir, pkg)` package registration + "Package conflict" log.

## Changes
1. `src/script/packages.h/.cpp`: split `load_class` into
   `bool load_class_at(PackageTree&, ClassPath&, Vm&, const std::string &dir,
   const std::string &base, std::string *fqn_out)` (= 0x55bec0: everything from
   `rel = dir + "\\" + base + ".class"` on; `fqn_out` = first new unit's class name) and keep
   `load_class(tree, classes, vm, fqn)` = find_dir + load_class_at (behaviour unchanged).
2. `src/game/runtime.h/.cpp`: add a hook
   `std::function<std::string(const std::string &dir, const std::string &base)> path_class_loader;`
   (public member or setter). In `script_class_of`, when the Script ref is a file
   (`ref.file`) and no ClassPath entry matches its path: split `ref.path` (record text, may
   use `\`) into dir + base (strip `.class`), call the hook if set, return its FQN
   (empty on failure). Keep the existing path match first (= 0x55aca0's "already loaded").
   Check how DataRef::path is stored (relative to game dir, slashes) and pass the dir in the
   exe's form (`cars\racers\Zed_Data\scripts`), since PackageTree::disk_path resolves
   relative to the game dir.
3. `src/app/boot.cpp` (after the VM class loader is installed, ~line 79): set
   `runtime_->path_class_loader` to call `jvm::load_class_at(*tree_, *classes_, *vm_, dir,
   base, &fqn)` and return fqn.

## Done when
- `make -C src clean check` green; no new tests.
- Report the new `tools/vmboot-capped.sh --selfcheck | tail -3` stop line.
