---
id: 105
status: implemented
branch: card/105
depends: [104]
---
# script VM: getClass loads missing classes through the package tree (0x55add0/0x55c0c0/0x55bec0)

## Goal
The src VM only knows classes added up front (`Vm::add_class`). The exe loads classes on
demand by FQN: getClass 0x55add0 -> loaded-class table, else 0x55c0c0 -> `findDir` in the
package tree (card 104, `jvm::PackageTree`) -> 0x55bec0 reads `dir\Name.class`, parses ALL
units in it, and re-registers the class's package with that directory. Add that path:
a `Vm::get_class` that calls an installable loader on a miss, use it wherever the
interpreter resolves a class by name, and a loader built on `PackageTree` + `ClassPath`.

## Exe evidence (Claude)
### 0x55add0 getClass(name)
    '[' -> array class (0x55a690; keep src's existing array handling);
    'L' prefix -> strip the 'L' and the last char (';');
    c = loaded(name) (0x55af50 hash);  if (!c) c = 0x55c0c0(name);
    if (!c) log "JVM::getClass: class \"<name>\" not found in file \"<file>\" at line <n>".
### 0x55c0c0 loadByName(name)
    dir = root.findDir(name);                              // 0x55fc10
    if (dir) return 0x55bec0(dir, name after the LAST '.');   // 0x569370 = strrchr
    log "Class not found: <name> ..."; return 0;
### 0x55bec0 load(dir, Name)
    JVM::compileSource(dir, Name)  (0x5598f0; src: SERVICE MISSING, see below)
    path = dir + "\\" + Name + ".class";  read whole file (0x565610); <1 byte ->
        log "JavaMachine::loadClass: failed to load <path>", return 0
    cls = parse(file) (0x554ee0: every unit in the file is registered; returns the first)
    pkg = class name up to its LAST '.';  r = root.register(dir, pkg) (0x55f780)
    if (r < 1) log "Package conflict: multiple packages placed at the same location (<cls>, <dir>)"
    return cls
    (PackageTree::add already returns -1 only for a dir owned by ANOTHER package, 0x55fb50.)
### Class init 0x549770 resolves the superclass and interfaces through getClass
    (FUN_0055af20(cls+0x14, index) -> recursive class init), i.e. loading a class pulls in its
    supers.
### 0x5598f0 compile fallback: needed when `dir\Name.class` is missing or stale and
    `dir\src\Name.java` exists. src has no compiler: log
    "JVM::compileSource: SERVICE MISSING (no compiler): <dir>\src\<Name>.java" when the .class
    is missing but the source exists, then fail the load like the exe's missing-file path.

## Changes
1. `src/script/vm.h` / `vm.cpp`:
   - `using ClassLoader = std::function<bool(Vm &, const std::string &fqn)>;`
     `void set_class_loader(ClassLoader f);`
   - `ClassState *Vm::get_class(const std::string &name)` (0x55add0): strip a leading 'L' and
     a trailing ';' as above; `find_class`; on a miss call the loader once (guard against
     re-entry for the same name with a `std::set<std::string> loading_`), then
     `find_class` again. Adding classes sets `linked_ = false` (existing); make sure
     `get_class` leaves the VM linked (`ensure_linked`) before returning a hit.
   - In `vm.cpp`, the interpreter's by-name lookups (lines ~599, 828-842, 883-886, 968, 1534:
     every `vm_->find_class(` / `find_class(` used to resolve a class a script names) call
     `get_class` instead. Keep `find_class` itself unchanged (const, no loading).
   - `ensure_linked`: when a class's `super_name` is not found, try `get_class(super_name)`
     before leaving `super` null (0x549770 loads supers). Avoid recursion issues: collect the
     missing super names first, load them, then link.
2. `src/script/packages.h` / `packages.cpp`: add
   ```cpp
   // 0x55c0c0 + 0x55bec0 over a ClassPath; adds every unit of the file to the VM.
   bool load_class(PackageTree &tree, ClassPath &classes, Vm &vm, const std::string &fqn);
   ```
   (include "resource/manager.h" for ClassPath and "script/vm.h"). Steps exactly as the
   evidence: findDir, Name = after the last '.', disk = `tree.disk_path(dir + "\\" + Name +
   ".class")`; missing -> compile-fallback log if `tree.disk_path(dir + "\\src\\" + Name +
   ".java")` exists, else the "failed to load" log; `classes.add_file(disk, &e)`; add each new
   unit (compare `classes.units()` size before/after) with `vm.add_class`; then
   `tree.add(dir, package)` and the conflict log when < 1; return
   `vm.find_class(fqn) != nullptr`. Logs go to stderr.
3. `src/app/boot.cpp`: `Boot::run` creates a `jvm::PackageTree` on the game directory (the
   directory of the opened system.rpk), calls `add_default_roots()`, and installs
   `vm_->set_class_loader(...)` calling `jvm::load_class(*tree_, *classes_, vm, fqn)` (keep
   the tree as a Boot member `std::unique_ptr<jvm::PackageTree> tree_`). Then, like the
   end of 0x55ccc0, `vm_->get_class("java.lang.String")` and
   `vm_->get_class("java.util.resource.GameRef")` (ignore failures, they only log).
4. `src/tools/vmboot.cpp` selfcheck (real data): after the boot, for every CONST_CLASS name
   in `game.Init`'s unit (the code that today reports "external" names), call
   `vm.get_class(name)`; report how many resolved and list the ones that did not (array
   descriptors `[...` are skipped). Expect `java.lang.GameType` (Init's super) to resolve now.
   Replace the old "missing superclass reported external: java.lang.GameType" check with
   "superclass java.lang.GameType loads through the package tree". Do not invent expected
   counts: print the numbers; the only hard checks are GameType + at least one other
   resolved name + 0 parse failures.

## Files
- edit: src/script/vm.h, src/script/vm.cpp, src/script/packages.h, src/script/packages.cpp,
  src/app/boot.cpp (and boot.h for the member), src/tools/vmboot.cpp

## Done when
- `// 0x55add0`, `// 0x55c0c0`, `// 0x55bec0` comments at the ported code; no unit tests
- `make -C src clean check` green; card frontmatter status implemented + branch
- if an existing check changes because classes now load, say which line and why under a
  `## Check impact` heading in this card

## Check impact
- `src/tools/vmboot.cpp`: the "missing superclass reported external: java.lang.GameType"
  check is replaced by "superclass java.lang.GameType loads through the package tree" plus
  the two loader checks, as the card asks.
- `Boot::run` now calls `get_class("java.lang.String")` and
  `get_class("java.util.resource.GameRef")` before it computes `external`, so those two
  names (and their supers) are no longer reported external and no longer counted in
  `ClassPath::missing_links`. The remaining external assertions pass unchanged because they
  compare against the live class map after the boot.
