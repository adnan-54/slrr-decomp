---
id: 058
status: todo
branch: ""
depends: []
---
# java.lang.Debug / java.util.Replay: error log and replay file natives

## Goal
Turn the four stubbed natives `Debug.errorEX`, `Debug.giveWarning`,
`Debug.writeFile`/`Replay.writeFile` and `Replay.readFile` into working handlers.
They only touch files: the first three append text lines to `error.log` or to an
arbitrary named file (text + CRLF, Windows file table in the exe), the last one
parses a `.rep` file and returns the float stored under a key for a frame.
No car/VM state is read or written.

## Exe evidence (cleaned pseudocode, resolved constants)

Argument binding: the exe's `CallInfo` unpacker 0x4b1af0 hands the natives their
arguments in declared order; our `Vm` already does this, so no port of it.

### 0x4d9350 Debug.errorEX(Ljava.lang.String;)V
```
msg = arg0
buf[256] = format(msg)        // 0x569530 -> 0x5277d0; msg is used as the printf format
sink(0, buf)                  // 0x544e80
```
### 0x4d9380 Debug.giveWarning(Ljava.lang.String;)V
```
msg = arg0
buf[256] = format(msg)        // 0x569610 -> 0x5277d0
sink(9999, buf)               // 0x544e80
```
### 0x544e80 sink(code, text) — shared by both
```
if (text == NULL) text = "Fatal Error";
else {
    error_log(text);                    // 0x544fa0
    if (code > 9999) goto done;
}
MessageBoxA(NULL, text, DAT_007a8620, 0x10);   // 0x10 = MB_ICONERROR
done:
if (code < 9999) _exit(code);               // errorEX: _exit(0); giveWarning (9999): returns
```
### 0x544fa0 error_log(text)
```
slot = file_open_write("error.log");    // 0x545920; -1 when its 64-slot table is full
if (slot < 0) return;                   // silently does nothing
seek_end(slot);
if (first_error_log_call_in_process) write("---\r\n");
write(text); write("\r\n");
flush(slot);                            // never closes
```
Image constants: "error.log" (0x5e0504), "Fatal Error" (0x5e0510), "\r\n"
(0x5e04f8), "---\r\n" (0x5e04fc).

### 0x4e30d0 Debug.writeFile / Replay.writeFile(String, String)
```
a = arg0; b = arg1;
tmp[256] = format(a, b)     // 0x5277d0: a is the printf format, b the single vararg string
append_file(tmp, b)         // 0x569650 -> 0x544ee0
```
### 0x569650 append_file(path, text) -> 0x544ee0
```
slot = file_open_write(path);   // 0x545920: CreateFileA(GENERIC_WRITE, share 2, OPEN_ALWAYS(4), FILE_ATTRIBUTE_NORMAL(0x80))
if (slot < 0) return;           // silently does nothing (also when 64 slots are used)
seek_end(slot);
write(text); write("\r\n");
flush(slot);                    // never closes
```
### 0x5277d0 format(dst, fmt, va) — shared formatter
Minimal printf: `%%`, `%c`, `%d`/`%i`, `%x`/`%X`, `%f`, `%s`; supports `0` flag,
width and `.precision`; `h`/`l`/`L` are skipped. `%s` pads to the width with
spaces. No destination size argument (callers use 256-byte buffers).

### 0x4e2f00 Replay.readFile(ILjava.lang.String;Ljava.lang.String;)F
```
index = arg0; key = arg1; path = arg2
slot = open_read(path);            // 0x565760: copy to 256 bytes, '/' -> '\'; then 0x565000(path, 0)
if (slot == 0) return 0.0;         // 0x565000 returns a 1-based slot, 0 on failure
size = file_size(slot);            // 0x565920
buf = malloc(size); read(slot, buf, size);   // 0x565820
in_replay = false; result = 0.0;   // kf and value are NOT initialized by the exe
p = buf; end = buf + size;
while (p < end) {
    line[1024] = read_line(p, &p); // 0x5281b0: copy through '\n', NUL-terminate; no length check
    if (line[0] == 0) {
        if (!in_replay) continue;
    } else if (!in_replay) {
        // 0x528300 is strstr (returns non-0 when found)
        if (line[0] == ';' || line[0] != '[' || strstr(line, "[replay]") == 0) continue;
        in_replay = true;
    }
    tok = strtok(line, delims, &ctx);            // 0x528350
    if (_stricmp(tok, "kf") == 0) {              // 0x57c11e/0x57c167, 0 = equal
        v = strtok(NULL, delims, &ctx);
        if (v) sscanf(v, "%d", &kf);             // 0x527140
    } else if (_stricmp(tok, key) == 0 && index == kf - 1) {
        v = strtok(NULL, delims, &ctx);
        if (v) sscanf(v, "%f", &value);          // value local_34 uninitialized if v == NULL
        in_replay = false;
        result = value;
    }
}
free(buf); close(slot);
return result;
```
Image constants: "kf" (0x5a5970), "%d" (0x5a5990), "%f" (0x5a59d4),
"[replay]" (inline literal, also confirmed in the corpus).
Corpus check: `bin/the game/multibot/replay/LagunaSeca_Baiern_DTM_bot.rep` is
`[replay]` followed by repeating blocks `kf 0..N`, `sc 1..N+1`,
`th/br/hb/sw/cl/nos/gr/grs/hrn/rpm <float>`. The frame number in the API call
is compared against `kf - 1`, keep that exactly.

## UNRESOLVED
- RESOLVED (Claude gate): tokenizer delimiters. `local_14 = 0x0a092c20; local_10 = 0xd` are
  little-endian bytes `20 2c 09 0a 0d 00` = `" ,\t\n\r"`. Use exactly that set.
- `MessageBoxA` caption `DAT_007a8620`: in the uninitialized `.data` tail, filled
  at runtime, not readable from the image. Only needed if a real message box is
  ever added (see below).
- The exe leaves `kf` (`local_2c`) and `value` (`local_34`) uninitialized when a
  token is missing. Their values there are stack garbage; the port must pick a
  deterministic value (see INVENTED below).

## Service gaps and how to fill them
- **Message box + process exit** (errorEX): src has no dialog or fatal service.
  Do not invent a UI. Write the same line to stderr (the existing engine log
  channel, `system_log` 0x4e5700) and then `std::exit(0)`; mark the replacement
  `INVENTED:`. The exe behaviour is `MessageBoxA` then `_exit(0)`; return type is
  void, process status 0.
- **`error.log` append**: no service needed; implement locally with
  `std::fopen("error.log", "ab")` and a process-global `first` flag for the
  `"---\r\n"` header.
- **exe write table leak**: 0x545920 never closes handles; each writeFile/error
  line uses one of 64 global slots, after which writes are silently dropped. Do
  not port the leak: open/append/close per call and add
  `shortcut: exe never closes its 64 write handles, upgrade if exact slot exhaustion matters.`
- **readFile**: no service needed. Use `file_path` (255-byte check, '\' -> '/')
  and `fs::read_file`, then parse in memory. The exe's 32-slot read cache and
  fixed 1024-byte line buffer are not observable; add
  `shortcut: in-memory lines, exe reads with a fixed 1024-byte buffer and no bounds check.`

## Files
- edit: src/script/natives.cpp — add `debug_error_ex`, `debug_give_warning`,
  `debug_write_file`, `replay_read_file` (near `system_log`, 0x4e5700) and wire
  the registry rows at ~4794-4799: `Replay.writeFile` and `Debug.writeFile`
  share `debug_write_file`; flip each `nullptr, true` to `<handler>, false`.
- read only: src/platform/fs.h (fs::read_file), src/script/vm.h (Value/Args)

## Implementation notes
- Reuse `want`, `get_i`, `get_s`, `file_path`, `file_log` already in the file.
- Formatting: 256-byte `std::snprintf`. errorEX/giveWarning use
  `snprintf(buf, 256, "%s", msg)` and writeFile uses
  `snprintf(buf, 256, path.c_str(), text.c_str())`; add
  `shortcut: %d/%x/%f in the format string are not type-correct in C++, the exe reads the same stack slot as that type.`
- `%s`-only calls (the usual plain path/message) are exact.
- Case-insensitive compare: small ASCII fold (same style as `find_compare` at
  ~2205); the exe uses CRTDLL `_stricmp` (0x57c167). `strstr` is `strstr`.
- `kf = 0` and `value = 0.0f` initial values: `INVENTED:` (exe stack locals are
  uninitialized).
- Missing value token on a matching key: the exe returns the uninitialized
  `value`; return 0.0f instead and mark `INVENTED:`.
- Keep the exact class/method/sig strings of the existing rows.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
