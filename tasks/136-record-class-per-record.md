---
id: 136
status: todo
branch: card/136
depends: [135]
---
# VM: a record-path `script <dir>\Name.class` gets its own class (21 freeride tracks)

## Why (vmboot 2026-10-24, 30000 frames)
No `native not implemented` hit at all (the 16 remaining stubs, Chassis 12 etc., are never
called on the Garage -> dealer -> buy -> testdrive path). The one failed thread is
`THRD-GARAGE-TESTDRIVE: java.game.CareerEvent.countRequirements line 200: field access on
non-object` (`track.location`, track == null). Cause: all 21 `multibot\maps\<map>\t_data.rpk`
records 0x102 point at `<map>\data\track_data.class`, every one declaring
`package java.game; class track_data extends TrackData`. src keys classes by FQN (ClassPath
`entries_`, `Vm::by_name_`, `Runtime::Resource::script_class` = name), so only the first map
(Norisring) gets a class; for the other 20, `load_class_at` returns an empty fqn (all units
already known), `GameRef.create(.., "track data")` returns null (221 such calls in the run).
Full RE: notes/03 "Record-path class load binds a fresh Class per record".

## Exe evidence (cleaned pseudocode)

### 0x51fcc0 record text `script <path>` (only the .class branch shown)
```c
if (ends_with(path, ".class")) {
    dir  = path up to (excluding) the last '\\';
    Name = text between the last '\\' and the last '.';
    node->cls /*+0x10*/ = JVM_findOrLoadByPath(dir, Name);   // 0x55aca0, a Class*
} else node->cls = JVM::getClass(path);                      // 0x55add0
```

### 0x55aca0 JVM_findOrLoadByPath(dir, Name)  (asm 0x55aca0..0x55adca)
```c
key = dir + "." + Name; replace every '\\' in key by '.';     // DAT_005b4710 = "."
found = 0;
for (bucket = 0; bucket < 0x200; bucket++)                    // JVM+8, 0xc per bucket
    for (e = bucket chain; e; e = e->next)
        if (strcmp(className(e->cls) /*unit+0x3c*/, key) == 0) found = e->cls;
if (found) return found;
return JavaMachine::loadClass(dir, Name);                     // 0x55bec0, tail call, EAX = Class*
```
The key is a directory path and shipped class names are `java.<pkg>.<Name>`, so on shipped
data it never hits: every record-path load re-reads the file and creates a new Class.

### 0x55bec0 -> 0x554ee0 addClass -> 0x548ea0 Class ctor -> 0x5492f0 hash insert
```c
cls = new Class(unit);                      // own statics, own state
h = sum(name chars)*0x25 + strlen(name)*7; b = h & 0x1ff;
if (bucket[b] empty) bucket[b] = {h, cls, next=0};
else { n = {h, cls}; n->next = bucket[b].next; bucket[b].next = n; }   // after the head
```
Lookup by name 0x55af50 starts at the bucket head -> `getClass(name)` returns the FIRST class
loaded under that name. src `Vm::by_name_` keeping the first entry already matches that.

## Files (3 modules; boot.cpp only follows a signature)
- src/script/vm.h + vm.cpp
- src/script/packages.h + packages.cpp
- src/game/runtime.h + runtime.cpp (+ the 3-line `path_class_loader` lambda in src/app/boot.cpp)

## How
1. vm: `ClassState *Vm::add_class(const ClassUnit *unit)` returns the new state. A unit whose
   name is already in `by_name_` still gets its own ClassState in `states_` (not indexed:
   0x55af50 returns the first). Keep `states_` iteration in ensure_linked as is (dups link
   to their super by name like every class). Also let the Vm own extra parsed files:
   `void Vm::keep_file(std::unique_ptr<ClassFile>)` (or any owner) so the units stay alive.
2. packages: new `ClassState *load_class_by_path(PackageTree&, ClassPath&, Vm&, dir, base)`
   = 0x55aca0 + 0x55bec0 for the record path: compile-on-load step as in load_class_at
   (missing .class -> src\Name.java), but ALWAYS parse the bytes into a fresh ClassFile
   (`classfile_parse`, bypass ClassPath's one-load-per-path rule), `vm.add_class` every unit,
   return the first unit's ClassState; then the same `tree.add(dir, pkg)` / "Package conflict"
   log as load_class_at. The first time a file is seen, also register it in ClassPath
   (`classes.add_file/add_data`) so name lookups (`classes.find`) keep working; reuse the
   first-parsed units for that first load (no double ClassState for the first load).
   Leave load_class_at (getClass path 0x55c0c0) unchanged.
3. runtime: `Resource` gets `ClassState *script_cls = nullptr` next to `script_class`.
   `script_class_of` (record ref `.class` file branch) uses the new loader; drop the
   `classes_->entries()` path-hit shortcut for file refs (0x55aca0 never dedupes by path) and
   fill both `script_cls` and `script_class` (= its name, for class_is_a/isScripted).
   `path_class_loader` returns `ClassState *`. Instances copy `script_cls` from their type
   (runtime.cpp ~506). `script_instance()` uses `r->script_cls` when set, else
   `vm_->find_class(r->script_class)` as today.

## Done when
- `make -C src -j4 check` green.
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: no failed thread
  (today: 1, `THRD-GARAGE-TESTDRIVE ... countRequirements line 200`), and a new hard check
  in vmboot: for each of the 21 `multibot\maps\*\t_data.rpk` records 0x102, create an
  instance (as CareerEvent.init does: `GameRef.create(null, type, null, "track data")`), call
  its `init()` and compare its `e_name` field with the `e_name = "...";` literal in that
  map's `data\track_data.java` (21/21 equal, 21 distinct ClassState pointers).
- Report the new `freeride:` / `final actualState:` lines (freeride may now reach
  `java.game.Track`; if a new thread fails there, report it, do not patch around it).
- `// 0xADDR` comments on ported steps; no INVENTED logic expected beyond ownership.
