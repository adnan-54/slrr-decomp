---
id: 135
status: implemented
branch: card/135
depends: [134]
---
# compiler: compile a missing .class from `<dir>\src\Name.java` at class load

## Goal
JavaMachine::loadClass 0x55bec0 runs JVM::compileSource 0x5598f0 before reading
`<dir>\Name.class`. Port the "class file missing" case: when `<dir>\Name.class` does not
exist and `<dir>\src\Name.java` does, compile it with src/script/compiler and load the
result. This covers both callers that already end in `jvm::load_class_at` (getClass
0x55add0 -> 0x55c0c0 and the record path 0x51fcc0 -> 0x55aca0). 635 shipped scripts are
java-only (notes/20 "Compile on load"), 16 of them package-root base classes under
`parts\scripts` / `parts\accessories\scripts`.

## Exe evidence (cleaned pseudocode)

### 0x55bec0 JavaMachine::loadClass(dir, Name)
```c
JVM::compileSource(dir, Name);                 // 0x5598f0, result ignored
path = dir + "\\" + Name + ".class";
n = File__readAll(path, &buf);
if (n < 1) { log("JavaMachine::loadClass: failed to load " + path); return 0; }
cls = JVM::addClass(buf, n);                   // 0x554ee0
pkg = text of cls name up to the last '.';
if (Package__register(dir, pkg) < 1)          // 0x55f780
    log("Package conflict: multiple packages placed at the same location (...)");
return cls;
```

### 0x5598f0 JVM::compileSource(dir, Name)
```c
src = dir + "\\src\\" + Name + ".java";
cls = dir + "\\" + Name + ".class";
have_src = exists(src);                        // 0x565200
need = false;
if (!exists(cls)) need = true;
else {
    if (have_src && ftime(cls) != ftime(src)) need = true;   // 0x564ef0, DOS-packed UTC
    h = first 12 bytes of cls;
    if ((h.magic != 'TUFA' || h.ver != 4 || h.build != 0x11ae1) && h.build != 0x24f9d)
        need = true;
}
if (!have_src) {
    if (need) log("JVM::compileSource: recompilation needed of \"" + src + "\" but source not exists");
    return 1;                                  // old .class (if any) is still read
}
if (!need) return 1;
delete(cls);                                   // 0x564fa0 -> DeleteFileA
f = open(src);
if (!f) { log("JVM::compileSource: file not found: \"" + src + "\""); return 0; }
line = 1; yyparse();                           // 0x54db20
if (parse_root) JVM::compileHook();            // 0x5557c0: writes dir\<first class>.class
if (!exists(cls)) fatal("JVM::compileSource: Syntax error in \"%s\" at line %d");  // MessageBox + _exit(-1)
set_ftime(cls, ftime(src));                    // 0x5658d0 -> SetFileTime
return ok;
```
compileHook 0x5557c0: registers the unit's package with dir first (0x55f780); semantic
errors ("cannot find parent class", "no such class", ...) are logged only, the class is
still written. Referenced classes resolve through the loaded classes, else
JVM::getClass 0x55add0 (0x55aa20), which loads and recursively compiles them.

## Files
- edit: src/script/packages.cpp (compile step in load_class_at),
  src/resource/manager.h + manager.cpp (`ClassPath::add_data`),
  src/script/compiler/emit.h + emit.cpp (`ClassIndex` miss loader),
  src/tools/vmboot.cpp (check).

## How
1. manager.h/.cpp: split `ClassPath::add_file` into `add_data(const std::string &path,
   const std::vector<uint8_t> &bytes, std::string *err)` (classfile_parse + the existing
   registration) and `add_file` = read + add_data. Same one-load-per-path rule.
2. emit.h/.cpp: `ClassIndex` gets a public
   `std::function<const ClassUnit *(const std::string &)> load;`. `find(name)` on a miss
   calls `load(name)` when set and, if it returns a unit, `add`s it and returns it
   (members `mutable`). (0x55aa20: loaded class else JVM::getClass.)
3. packages.cpp `load_class_at`, replace the "SERVICE MISSING" block:
   ```
   disk = tree.disk_path(rel)
   if disk empty:
       java = tree.disk_path(dir + "\\src\\" + base + ".java")   // case-insensitive, SRC/Src ok
       if java empty: log "JavaMachine::loadClass: failed to load <rel>"; return false
       read text; parse_java(src, &ju, &err, &tokens)
         fail -> log "JVM::compileSource: Syntax error in \"<java_rel>\" ...", return false
         // INVENTED: the exe quits the process (0x569570 -> _exit(-1)); src fails the load
       tree.add(dir, ju.package or "java.unnamed")             // 0x5557c0 registers first
       ClassIndex index; for each u in classes.units(): index.add(*u)
       index.load = [&](name){ vm.get_class(name); e = classes.find(name);
                               return e ? e->unit : nullptr; }  // 0x55aa20 -> 0x55add0
       compile_unit(ju, tokens, index, &out, &err, nullptr)
         fail -> log err; return false
         // INVENTED: the exe logs semantic errors and still writes the class
       classfile_write(out, &bytes)
       if first unit's simple name != base: log failed to load; return false   // 0x55bec0 reads dir\Name.class
       classes.add_data(<disk dir>/<base>.class, bytes, &err)   // then the existing add_class / register path
   ```
   INVENTED (comment it): the exe writes `<dir>\<first class>.class` and sets its time to
   the source's; src keeps the bytes in memory so a run never writes into the game dir.
   shortcut: registry null (literal package spelling as written; runtime ids resolve
   case-insensitively), upgrade when the runtime registry keeps spellings.
   shortcut: only the missing-.class case; the stale-time and bad-header recompiles
   (1126 + 166 shipped pairs here) wait until the 207 remaining body diffs are explained.
   Vm::get_class's `loading_` set is the re-entry guard for cyclic references.
4. vmboot.cpp selfcheck, after the boot: for every `<root dir>\...\src\X.java` without
   `X.class` under the 9 package roots (16 files), `boot.vm()->get_class(<package>.X)` where
   the package is the source's `package` line. Then for every java-only source in the whole
   game dir call `jvm::load_class_at(tree, classes, vm, dir, X, &fqn)` and print
   `compile on load: N sources, ok K, failed F` + the first 10 failures.

## Done when
- `make -C src -j4 check` green.
- vmboot hard checks: all 16 package-root java-only classes resolve
  (e.g. `java.game.parts.enginepart.Driveshaft`, `java.game.parts.rgearpart.suspension.FLMcPhersonStrut`);
  the 4 Axis record scripts that resman lists as unresolved load through `load_class_at`.
  The 635-source sweep is report-only; record its numbers in notes/20.
- nothing written into the game dir: `find "bin/the game" -name '*.class' -newer src/Makefile` empty after the run (bin/ is gitignored, git status cannot show it).
- `// 0xADDR` comments on ported steps, INVENTED:/shortcut: marks as above.
- card frontmatter: status implemented, branch name.
