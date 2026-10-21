---
id: 026
status: todo
branch:
depends: [025]
---
# io: FindFile.first / next / close

## Goal
Make the three `java.io.FindFile` natives non-stubs. The exe snapshots the whole
match list at `first`, sorts it case-insensitively, and serves it from one of 32
global list slots; the script object only holds the slot index (`handle`) and the
filter (`flags`: 0 FILES_DIRS, 1 FILES_ONLY, 2 DIRS_ONLY). Directory enumeration
comes from `fs::find` (card 025).

## Exe evidence (cleaned pseudocode, renamed fields)
### 0x5652c0 find_open(path, name_out, is_dir_out) -> slot or <0   (lists: 32 slots at 0x7a59d0)
    char p[256]; strcpy(p, path); replace '/' with '\\';
    // packed-fs branch guarded by DAT_007a5b50: that global is never written (3 reads only) -> dead
    os = os_find_first(p, name, &is_dir);          // see card 025
    if (os < 0) return os;                          // -2: no match / bad dir
    slot = first i < 32 with list[i] == NULL; if none: return -1   // (os handle leaks; ignore)
    list[slot] = []
    do { list[slot].append({name, is_dir}); } while (os_find_next(os, name, &is_dir) == 0);
    os_find_close(os);
    qsort(list[slot], count, 0x104, cmp_names);    // 0x564e40 -> 0x5280c0(a, b, fold=1)
    count[slot] = n; cursor[slot] = 0;
    find_next(slot, name_out, is_dir_out);          // first sorted entry
    return slot;

### 0x5280c0 compare with fold=1 (sort key)
    loop: ca = *a++, cb = *b++ (signed char); if 'A'<=c<='Z' -> c += 32 (both);
          if ca < cb return -1; if ca > cb return 1; until both are 0 -> return 0

### 0x5654c0 find_next(slot, name_out, is_dir_out) -> 0 ok / -1 end
    if (list[slot] && cursor[slot] < count[slot]) {
        strcpy(name_out, list[slot][cursor].name); *is_dir_out = list[slot][cursor].is_dir;
        cursor[slot]++; return 0;
    }
    return -1;

### 0x565270 find_close(slot)
    if ((unsigned)slot < 32) { free(list[slot]); list[slot] = NULL; count = cursor = 0; }

### 0x4d9e90 FindFile.first(String path, int flags) -> String
    h = find_open(path, name, &is_dir);
    this.handle = h;  this.flags = flags;        // FUN_004ed490 int field writes
    if (h >= 0) {
        rc = 0;
        while (rc == 0) {
            if (strcmp(name, ".") && strcmp(name, "..")) {
                if (is_dir == 0 ? flags < 2 : flags != 1)   // signed compares
                    return new String(name);
            }
            rc = find_next(h, name, &is_dir);
        }
    }
    return null;              // slot is NOT closed on exhaustion (script calls close())

### 0x4d9fb0 FindFile.next() -> String
    h = this.handle; flags = this.flags;          // FUN_004ece80 int field reads
    if (h >= 0)
        while (find_next(h, name, &is_dir) == 0)
            if (strcmp(name, ".") && strcmp(name, "..") && (is_dir == 0 ? flags < 2 : flags != 1))
                return new String(name);
    return null;

### 0x4d9e40 FindFile.close() -> void
    h = this.handle; this.handle = -1;
    if (h >= 0) find_close(h);

## Implementation notes
- File-static table in natives.cpp: `struct FindList { bool used; std::vector<fs::FindEntry> e; size_t cursor; };`
  `FindList g_find[32];`. Slot search order and the -1/-2 codes as above
  (`fs::find` false -> -2; no free slot -> -1).
- Path: run the existing `file_path()` helper (256-byte limit, '\\' -> '/'), then `fs::find`.
- Sort with `std::stable_sort` and the 0x5280c0 comparator (compare as `signed char`).
  INVENTED: stable order for names equal under folding (only possible on case-sensitive hosts).
- Fields: `vm.get_instance_field(self, "handle"/"flags", ...)` (ints; a non-int read counts as -1/0)
  and `vm.set_instance_field(self, "handle", Value::make_int(h), &e)`.
- Strings: `Value::make_str(name)`; end -> `Value::make_null()`.
- Receiver is a[0]; `first` args: a[1] path (String), a[2] flags (int).

## Files
- edit: src/script/natives.cpp — table + `find_open/find_next/find_close` helpers and three natives
  with `// 0xADDR` comments; switch rows `FindFile.first (Ljava.lang.String;I)Ljava.lang.String;`,
  `next ()Ljava.lang.String;`, `close ()V` to `<fn>, false`.
- read only: src/platform/fs.h

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
