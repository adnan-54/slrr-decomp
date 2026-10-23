---
id: 095
status: todo
branch:
depends: []
---
# script: System.openLib / System.compileAll / GfxEngine.forceRendering

## Goal
Implement the three natives that are still registered with `nullptr` in
`src/script/natives.cpp`: `GfxEngine.forceRendering()V` (0x4e5560), `System.openLib(String)I`
(0x4e0470) and `System.compileAll(String)I` (0x4e54c0). openLib becomes a real runtime
package mount (`ResourceManager` gains an .rpk-only open, like the exe, which never
reads an .rdb); compileAll ports the exe's recursive `.java` scan and its count, but not
the missing Java compiler; forceRendering is a no-op because src has no render host.

## Exe evidence (cleaned pseudocode, renamed fields)
`getArgs(ctx, out0, out1, ...)` = `Native__getArgs 0x4b1af0` (notes/06). For a static
one-argument native the declared argument lands in `out1`; src's `a[0]` is the same
argument (cf. the 0x4dc91d comment). Manager singleton = `[0x5db008]`.

### 0x4e5560 java.render.GfxEngine.forceRendering()V
    for (i = 7; i != 0; i--) {        // mov esi,7; ... dec esi; jnz
        ecx = [0x5db008]; 0x517f60(); // resource streaming tick
        ecx = [0x5db008]; 0x518f80(); // manager list tick (below)
        [0x5e8ca0] = 0;               // dword
        ecx = [0x5da014]; 0x421920(); // Game__renderFrame (notes/12 §2.1)
        [0x5dadbc]++;                 // frame/iteration counter
    }
    0x517f60: streaming tick, calls 0x51e310/0x51e900/0x51f9b0/0x542c30 (notes/00).
    0x518f80 (thiscall, ECX = manager): walks the list at [mgr+0x16b0]; sets
    node+0x40 |= 2 when node+0x3c is null or a virtual +0x2c test fails; then moves
    every marked node to the free list at [mgr+0x16f8], clears its +0x3c/+0x30/+0x34,
    calls 0x5176a0(0) and 0x41fd90(). UNRESOLVED: its exact subsystem name (reads as
    a deferred-release/purge tick of the resource manager).
    `flush()` 0x4dc840 reaches 0x4e5560 directly (notes/12 §9); src's gfx_flush is
    already a no-service no-op.

### 0x4e0470 java.lang.System.openLib(String)I
    name = getArgs arg;
    idx  = 0x518940(name);            // ecx=[0x5db008]; find-or-append, returns index
    enc  = idx << 16;
    if (0x517b80(enc) != 0) {         // ecx=[0x5db008]; ensure-loaded package slot
        0x517b80(enc);                // same call again, result ignored
        ecx = pkg;                    // pkg = result of the second 0x517b80
        if (0x51e2e0() >= 2)          // [pkg+0x50] state; cmp eax,2 / jae
            return idx;
    }
    ecx = &zeroed_1c_byte_temp;       // handle temp
    0x51f9b0(enc | 1);                // release it
    return -1;

    0x518940 (ECX = mgr): scan [mgr+0x166740] slots with 0x51e2f0(name); on a match
    return that index; else append with 0x51e2a0(count, name), count++, return old
    count. (The decompiler's 0xffff check is dead: the append path follows it.)
    0x517b80(enc): hi = enc>>16; hi==0xffff -> mgr+0x166744; hi < count ->
    `0x51e310(pkg)` (ECX = pkg = mgr+0x1667b4+hi*0x70) and return pkg iff that is
    non-zero, else 0; hi >= count -> 0.
    0x51e310 (ECX = pkg) = Package::load: state = [pkg+0x50];
        if state == 0:
            if (0x565200(pkg+0xc) == 0) return state;   // path check; 0 = missing
            [pkg+0x50] = 1;
            h = 0x565760(pkg+0xc);                       // opens the file
            if (h) { read "RPAK", version 0x200, counts, 0x42-byte records (each
                     record name registered via 0x518940), blob ... }
            [pkg+0x50] = 2; return 2;                    // also when h == 0
        return state;
    0x565200(name) validates the path as given (turns '/' into '\\'); non-zero = the
    file is there. 0x565760(name) = the file open used for the header read (identity
    not traced, UNRESOLVED).
    0x51e2e0: `return [ECX+0x50]` (package state; ECX = pkg above).
    0x51f9b0: releases the temporary handle; it has no effect on the -1 result.
    => openLib succeeds (returns the registry index) as soon as the path exists, even
    if the RPAK parse then fails; it never removes the failed registry entry.

### 0x4e54c0 java.lang.System.compileAll(String)I
    dir = getArgs arg;
    ecx = [ctx];                      // compiler/service object
    return 0x555510(dir);             // bytes after the call: pop esi; mov esp,ebp;
                                      // pop ebp; ret -> eax of 0x555510 survives
    0x555510(dir):
      log("CompileAll: begin file scan...");
      if (strlen(dir) + 2 > 0xff) {           // len >= 254
          format "Path is too long! Can't compile %s"; log; return 0;
      }
      copy dir (256-byte buffer, truncated);
      append '\' unless the last char already is '\'; append '*' -> "dir\*";
      find-first 0x5652c0 / find-next 0x5654c0 (FindFirstFileA/FindNextFileA wrappers,
      is_dir = attribute bit 4):
        for each entry:
          skip "." and "..";
          if (!is_dir) {
              if (strchr(name, '.') == ".java") { 0x5598f0(...); count++; }
          } else {
              count += 0x555510(dir + '\' + name);   // recursive, same checks
          }
        when find-next < 0: close 0x565270; log("CompileAll: success!"); return count;
      => the returned int is the number of `.java` files handed to the compiler.
    SERVICE MISSING: 0x5598f0 is the Java source compiler; src has no equivalent.
    UNRESOLVED: the first argument of the 0x5598f0 call, raw line
    `FUN_00569340(local_448,local_148,iVar6 + (-3 - (int)local_148));` (path prefix
    derivation not decoded — irrelevant once the call is skipped).
    The `.java` test uses the FIRST dot (`strchr`), not a suffix test.

## Changes
1. `src/resource/manager.h`
   - `#include <deque>`; change the storage to `std::deque<Package> packages_;`
     (0x517b80 hands out `Package*`, and Runtime::Resource stores `const Package*`;
     push_back on a vector would dangle them on a runtime mount).
   - declare `bool open_rpk(const char *rpk_path, std::string *err);` next to `open`.
2. `src/resource/manager.cpp` — `ResourceManager::open_rpk`: same as `open` but only
   `pkg.rpk.load(...)`, `rdb_path.clear()`, `mount = canonical_package(rpk_path)`,
   `packages_.push_back(...)`. The exe loads only the .rpk (notes/01 §.rdb).
3. `src/script/natives.cpp`
   - next to `package_index`/`package_key` (~line 2503, so `package_index` is visible):
     * `system_open_lib`: `need_rt`, `want(...,1)`, `get_s`; `Value::Null` -> return
       `-1` (`INVENTED`: the exe has no null check, 0x4e0494 hands the raw pointer to
       0x518940); normalize `'\\' -> '/'` (exe 0x565200 normalizes the other way for
       Win32, POSIX needs '/'); `pkg = rm.find_open_package(path)`; if null
       `rm.open_rpk(path.c_str(), &e)` and `pkg = &rm.package(rm.package_count()-1)`;
       on load failure return `-1`; else return `(int32_t)package_index(pkg)`.
       `INVENTED:` the exe returns the index in its global name registry (0x518940);
       src ids use the ResourceManager package index, so that index is returned and
       `find_open_package`'s canonical suffix match replaces the exact-name registry.
       `shortcut:` a failed load leaves no registry entry (the exe keeps one, so a
       later openLib reuses its slot) and a corrupt .rpk returns -1 (exe: index).
     * `system_compile_all` + a file-static `int32_t compile_scan(const std::string&)`:
       `want`/`get_s`, normalize separators, return `compile_scan(path)`
       (`INVENTED`: a null/non-String argument is the usual native error; the exe
       passes the raw pointer to strlen/FindFirstFileA without a check).
       compile_scan: log "CompileAll: begin file scan..."; `if (dir.size() + 2 > 0xff)`
       -> log the "Path is too long! Can't compile %s" line and return 0; build
       `dir` with a trailing `'/'` (only when `dir` is non-empty and does not already end
       in a separator: 0x555510 checks len != 0 first) then `"*"`; `fs::find` (the FindFirstFileA/Next
       wrappers); for each `fs::FindEntry`: skip `.`/`..`; `is_dir` ->
       `n += compile_scan(dir + "/" + name)`; else first dot via `find('.')` and
       `compare(dot, npos, ".java") == 0` -> `n++`; log "CompileAll: success!"; return n.
       `SERVICE MISSING:` no Java compiler; the exe still counts each such file, so
       only the 0x5598f0 call is skipped (comment it as such at the `n++`).
   - next to `gfx_flush` (~line 5543): `gfx_force_rendering`, `*out = make_null()`,
     comment `// shortcut: src has no render-frame/resource-streaming host; the 7x
     loop has nothing to observe (same no-service path as flush 0x4dc840).`
   - registry rows (replace the function pointer only, keep the row count at 422):
     `system_open_lib` at 7004, `system_compile_all` at 7005,
     `gfx_force_rendering` at 7111; set the trailing stub flag to `false`.

## Files
- edit: src/script/natives.cpp
- edit: src/resource/manager.h, src/resource/manager.cpp

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
  (the registry-count check must stay 422: rows are replaced, not added)
- service gaps stated in-code: forceRendering void/no-op, compileAll scan+count only
- card frontmatter: status implemented, branch name
