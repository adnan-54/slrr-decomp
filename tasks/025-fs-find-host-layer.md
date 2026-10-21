---
id: 025
status: todo
branch:
depends: []
---
# platform: fs::find — FindFirstFileA/FindNextFileA equivalent

## Goal
`java.io.FindFile` (card 026) lists a directory through the exe's wrapper
0x545520/0x5455e0/0x5454d0 around `FindFirstFileA`/`FindNextFileA`/`FindClose`.
Add one host-layer function returning every entry the Win32 pattern search
yields, in OS order, with the directory bit. On Windows call the Win32 API
directly (exact behaviour); elsewhere emulate it (INVENTED matcher).

## Exe evidence (cleaned pseudocode)
### 0x545520 os_find_first(pattern, name_out, is_dir_out)   (64 OS slots at 0x799d90)
    slot = first free of 64; none -> return -1
    h = FindFirstFileA(pattern, &fd[slot]);
    if (h == INVALID_HANDLE_VALUE) return -2;
    strcpy(name_out, fd.cFileName);
    *is_dir_out = (fd.dwFileAttributes >> 4) & 1;      // FILE_ATTRIBUTE_DIRECTORY
    return slot;
### 0x5455e0 os_find_next(slot, name_out, is_dir_out)
    if (!FindNextFileA(h[slot], &fd[slot])) return -3;
    strcpy(name_out, fd.cFileName); *is_dir_out = attr bit 4; return 0;
### 0x5454d0 os_find_close(slot) -> FindClose(h[slot]); h[slot] = 0
The caller (0x5652c0) converts every '/' in the pattern to '\\' before the call.

## API to add (src/platform/fs.h / fs.cpp)
```cpp
struct FindEntry {
    std::string name;   // cFileName (no directory part)
    bool is_dir = false;
};
// All matches of a FindFirstFileA pattern ("dir/*", "dir/*.png", "dir/name"), in OS
// order, including "." and ".." when the pattern matches them. False when nothing
// matches or the directory does not exist (FindFirstFileA failure).
bool find(const std::string &pattern, std::vector<FindEntry> *out);
```
- `_WIN32`: copy the pattern, replace '/' with '\\', then FindFirstFileA / FindNextFileA loop /
  FindClose; `is_dir = (attr & FILE_ATTRIBUTE_DIRECTORY) != 0`. This keeps Win32 quirks
  (8.3 short-name matches such as `*.png` hitting `x.pngold`).
- POSIX (INVENTED emulation, mark it): split at the last '/' or '\\' into dir (empty -> current
  directory, as `list_dir` already handles) and name pattern; an empty name pattern -> false
  (FindFirstFileA("dir\\") fails). Use `list_dir(dir)`; keep names that match; `is_dir` =
  `exists(full) && !is_file(full, nullptr)` with `full = dir.empty() ? name : dir + "/" + name`
  (dir already ends without a separator after the split; add one).
  Matcher: ASCII case-insensitive; `*` matches any run (also empty), `?` matches exactly one
  char; special case: a pattern ending in `.*` also matches names without a dot (so `*.*`
  equals `*`). No 8.3 emulation (`shortcut:` comment). Return false when no entry matched.

## Files
- edit: src/platform/fs.h, src/platform/fs.cpp

## Done when
- the Win32 path cites `// 0x545520/0x5455e0/0x5454d0`; the POSIX matcher is marked `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
