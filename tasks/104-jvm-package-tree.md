---
id: 104
status: implemented
branch: card/104
depends: []
---
# script: the JVM package tree (FQN -> script directory), checked on all shipped classes

## Goal
The exe finds a class by fully-qualified name through a package tree of directories
(JVM init 0x55ccc0 registers 9 roots; subdirectories become sub-packages automatically).
Port that tree as a standalone module `src/script/packages.{h,cpp}` and add a real-data
check: every `.class` file below the 9 roots must resolve back to its own directory. Hooking
it into the VM as the lazy class loader is card 105 (not here).

## Exe evidence (Claude)
### 0x55ccc0 JVM init: roots (exactly these, in this order; paths relative to the game dir)
    "system\\scripts\\lang"          -> "java.lang"
    "system\\scripts\\net"           -> "java.net"
    "system\\scripts\\io"            -> "java.io"
    "system\\scripts\\util"          -> "java.util"
    "system\\scripts\\render"        -> "java.render"
    "system\\scripts\\sound"         -> "java.sound"
    "sl\\scripts\\game"              -> "java.game"
    "parts\\scripts"                 -> "java.game.parts"
    "parts\\accessories\\scripts"    -> "java.game.parts.accessories"
    then getClass("java.lang.String"), getClass("java.util.resource.GameRef").
    The tree root is a node named "java" (0x55f330(0, 0, "java")).
### Node (0x55f330 ctor(parent, dir, name)): {+0 name, +4 children (sub-package nodes),
    +8 dirs (dir entries), +0xc parent}; if dir != 0 -> addDir(dir) (0x55f550).
### 0x55f780 register(dir, dotted) on node N
    if dotted has a '.': first segment must equal N.name, else return 0.
        rest = after the first '.'; try each child: if child.register(dir, rest) != 0 return it;
        no child took it: if rest still has a '.': create child named rest's first segment
        (no dir) and recurse register(dir, rest) on it; else create child (N, dir, rest) and
        return 1.
    else (single segment): equal to N.name -> return N.addDir(dir); else 0.
### 0x55f550 addDir(dir): if dir already in N.dirs -> return -1. Else append dir entry
    {path = dir, names = not yet listed}; then for each SUBDIRECTORY s of dir
    (0x560180: find "dir\\*", is_dir, skip "." and "..") append child node (N, dir\\s, s)
    (that ctor recursively adds dir\\s, i.e. the whole subtree). Return 1.
### 0x55fd90 dir entry listing (lazy, first lookup): class names =
    for each FILE in "dir\\src\\*.java": name up to the FIRST '.', if the rest == ".java";
    then for each FILE in "dir\\*.class": name up to the first '.', if the rest == ".class",
    unless already listed. (Order: java names first, then class-only names.)
### 0x55fc10 findDir(fqn) on node N  (0x55c0c0 calls it on the root)
    needs at least one '.'; first segment must equal N.name.
    if the rest has no '.': for each dir entry of N in order: if rest is in its listing
        (exact, case-sensitive strcmp) -> return that dir path; else 0.
    else: for each child c: r = c.findDir(rest); if r return r; -> 0.
### 0x55c0c0 load by FQN: dir = root.findDir(fqn); if dir -> 0x55bec0(dir, ClassName)
    (compile if stale, then read "dir\\ClassName.class"); else log "Class not found: ...".

## Changes
1. New `src/script/packages.h` / `packages.cpp` (namespace `jvm`):
   ```cpp
   // 0x55f330 package node; dirs keep exe-style relative paths ("system\\scripts\\lang").
   class PackageTree {
     public:
       // game_dir: where relative exe paths are resolved (case-insensitive, like Win32).
       explicit PackageTree(std::string game_dir);
       void add_default_roots();                           // 0x55ccc0, the 9 roots in order
       int add(const std::string &dir, const std::string &dotted);   // 0x55f780
       std::string find_dir(const std::string &fqn);       // 0x55fc10, "" when not found
       std::string disk_path(const std::string &exe_path) const;     // INVENTED helper
       size_t node_count() const;
     ...
   };
   ```
   Nodes in `std::vector<std::unique_ptr<Node>>` children; dir entries with a lazily filled
   `std::vector<std::string> names` + `bool listed`. `disk_path`: split the exe path on '\\'
   and '/', walk from `game_dir` segment by segment with `fs::list_dir`, matching each
   segment case-insensitively (INVENTED: Linux stand-in for case-insensitive Win32 paths);
   "" when a segment is missing. Directory scans (0x560180, 0x55fd90) use `fs::find` on the
   disk path ("<disk>/*", "<disk>/src/*.java", "<disk>/*.class"); a missing directory just
   lists nothing. Keep the ".java first, then .class" listing order and the FIRST-dot rule.
   Every function gets its `// 0xADDR` comment.
2. `src/Makefile`: add `script/packages.cpp` to `LIB_SRC` (next to `script/vm.cpp`).
3. `src/tools/vmboot.cpp` selfcheck: add a block that builds `jvm::PackageTree tree(kGameDir);
   tree.add_default_roots();`, then walks every directory below the 9 roots' disk paths
   (recursively, skipping `src` subdirectories) and for each `X.class` file reads it with the
   existing class parser (see how vmboot/classdump load a ClassUnit) to get its own class
   name; check `tree.find_dir(that name)` maps (via `disk_path`) to the directory the file is
   in. Report one line: classes checked, resolved, mismatches (print the first 5). Expect 0
   mismatches and at least 1 class checked; also check `find_dir("java.util.resource.GameRef")`
   and `find_dir("java.game.Init")` are non-empty (both exist in the install).
   If shipped data really contains a class whose package does not match its directory,
   do not hide it: count and print those separately and explain in the card.

## Files
- new: src/script/packages.h, src/script/packages.cpp
- edit: src/Makefile, src/tools/vmboot.cpp
- read only: src/platform/fs.h, src/script/classfile.h (class name of a unit)

## Done when
- `make -C src clean check` green with the new vmboot lines; no unit tests
- card frontmatter: status implemented, branch name

## Result (implemented)
Ported 0x55ccc0/0x55f330/0x55f780/0x55f550/0x560180/0x55fd90/0x55fc10 into
`src/script/packages.*`; vmboot --selfcheck builds the tree and walks all 9 roots.
Live output: `335 classes checked, 260 resolved, 75 package/dir mismatches
(shipped), 0 failures, 72 extra units, 0 unreadable (57 nodes)`.

Two shipped-data facts the per-unit walk surfaced (both counted, not hidden):
- **75 .class files under `sl/Scripts/game/CareerEvents/` and `.../Gamemodes/`
  declare `package java.game`** (the parent package) in both the `.class` and the
  shipped `.java` source, so `find_dir("java.game.<Name>")` cannot reach them:
  the tree only looks in the parent directory's listing. These live one level too
  deep for the FQN loader; the install loads them from their directory instead
  (`GameLogic` calls `System.rpkScan("sl\\Scripts\\game\\CareerEvents\\")`). The
  check classifies a mismatch as this case iff the declared package is a strict
  ancestor of the directory's package, and reports the count separately.
- **72 extra class units** live in files named after another class (e.g.
  `Thread.class` also holds `java.lang.Agent`, `Dialog.class` holds
  `java.render.osd.dialog.MenuDialog`). 0x55bec0 loads `dir\ClassName.class`,
  i.e. only the unit matching the file name is reachable by FQN; the others are
  nested/auxiliary units and are counted as `extra units`, not checked.

Also fixed a latent bug in `src/platform/fs.cpp`: the POSIX `wild_match` never
matched when pattern and name ended together, so `fs::find` only worked for
patterns ending in `*` (`dir/*`); `*.java`/`*.class`/`dir/name` always failed.
Root-cause one-line fix (`if (!*p) return !*s;`), needed by the 0x55fd90 listing.
