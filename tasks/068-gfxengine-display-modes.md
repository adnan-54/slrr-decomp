---
id: 068
status: implemented
branch: card/068
depends: []
---
# render: GfxEngine display-mode natives (num/curr/name/changeVideoMode)

## Goal
Turn the four `java.render.GfxEngine` display-mode natives from stubs into the
exe's behaviour. All four are declared **static native** (shipped
`bin/2.2.1 sources/system/Scripts/render/src/GfxEngine.java:12-17`; 2.3.1
scripts call them as `GfxEngine.numDisplayModes()`), so in our VM the first
declared argument is `a[0]` (`Vm::run_native` only pushes a receiver for
instance methods, src/script/vm.cpp:1608).

The exe reads a process-wide, pre-sorted mode list `DAT_005eb450` (count
`DAT_005eb454`, 0x14-byte entries, w/h/depth at entry +0/+4/+8) and C3DLow's
current mode (`+0x1bc/+0x1be/+0x1c0`, i16) plus `+0x1c2` windowed
(ctor defaults 640x480x16, fullscreen: notes/12 §1.1, 0x4223a0).

**No mode-list producer exists in src/** (no adapter enumeration). Model the
list as empty; the natives then return exactly the exe's no-mode values
(count 0, index 0, empty string). Do not invent an enumerator.

## Exe evidence (cleaned pseudocode, renamed fields)
`getArgs` = `Native__getArgs 0x4b1af0(ctx, firstArgIndex, &out...)` (notes/06:393).

### 0x4e5770 numDisplayModes()I
Tail thunk (disasm):
```
4e5770  E9 6B 0D F4 FF   JMP 0x4264E0
0x4264E0  A1 54 B4 5E 00   MOV EAX,[DAT_005eb454]
          C3               RET
```

### 0x4e54f0 currDisplayMode()I
Tail thunk: `4e54f0  E9 1B EC F3 FF  JMP 0x424110`
```
int currDisplayMode(void) {          // 0x424110
  int i = 0;
  if (mode_count > 0) {
    int *e = (int *)(modes + 8);     // entry +0x8, stride 0x14
    do {
      if (e[-2] == (int)*(int16_t *)(renderer + 0x1bc) &&   // width  (sign-extended i16)
          e[-1] == (int)*(int16_t *)(renderer + 0x1be) &&   // height
          e[ 0] == (int)*(int16_t *)(renderer + 0x1c0))     // depth
        return i;
      i++;
      e += 5;
    } while (i < mode_count);
  }
  return 0;
}
```

### 0x4e5510 displayModeName(I)String
Statics: `ESI = ctx`; `buf[256]` at ebp-0x104; mode at ebp-4; `ECX = [ctx]` for the
String factory (`8B 0E` before the call).
```
void displayModeName(ctx) {
  int i;
  char buf[256];
  getArgs(ctx, 0, &i);          // 0x4b1af0
  formatModeName(i, buf);       // 0x424210
  newString(buf);               // 0x55b980, ECX = [ctx] (JVM string factory)
  return;                       // EAX (the String) is left as the native result
}
// 0x424210
int formatModeName(int i, char *buf) {
  if (-1 < i && i < DAT_005eb454) {
    sprintf(buf, "%d %d %d", entry[i].w, entry[i].h, entry[i].depth); // 0x427c20
    return 1;
  }
  *buf = 0;                     // -> ""
  return 0;
}
```
Cross-check (shipped script): `OptionsDialog.java:186-191` splits the result into
`token(0/1/2)` as width/height/depth and skips `depth == 16` — the third field is
the **depth**, not a refresh rate.

### 0x4e5480 changeVideoMode(III)V
```
void changeVideoMode(ctx) {
  int w, h, depth;
  getArgs(ctx, 0, &w, &h, &depth);                       // 0x4b1af0
  C3DLow *r = *(C3DLow **)(*(void **)0x5da014 + 0x404);  // game -> renderer
  r->vtable[5](r, w, h, depth);                          // [0x593bcc+0x14] = 0x422ec0
}
```
`0x422ec0` (this = C3DLow):
```
void requestVideoMode(int w, int h, int depth) {
  if ((int)*(int16_t *)(this+0x1bc) == w &&
      (int)*(int16_t *)(this+0x1be) == h &&
      (int)*(int16_t *)(this+0x1c0) == depth) return;     // already current
  if (*(char *)(this+0x1c2) == 1) return;                 // windowed: no-op
  // format probe via IDirect3D9 (*(this+0x14)), CheckDeviceFormat(vtable+0x28):
  //   X8R8G8B8(0x16) RT ok, A8R8G8B8(0x15) ok, R5G6B5(0x17) ok, X1R5G5B5(0x18) ok
  // depth==16: R5G6B5 else X1R5G5B5/X8R8G8B8 fallback; else A8R8G8B8 -> X8R8G8B8
  //            -> R5G6B5 -> X1R5G5B5  (exact code pasted below, NOT modelled)
  *(uint32_t *)(this+0x174) = w;            // backbuffer w
  *(uint32_t *)(this+0x178) = h;
  *(uint32_t *)(this+0x17c) = chosenFormat; // +0x17c BMP format code
  *(uint32_t *)(this+0x19c) = chooseDepthFormat(chosenFormat); // 0x426b10
  *(uint8_t  *)(this+0x1b5) = 1;            // reset pending
  *(uint32_t *)(this+0x364) = 0; *(uint32_t *)(this+0x368) = 0;
  *(uint32_t *)(this+0x394) = 0; *(uint32_t *)(this+0x398) = 0;
  *(int16_t  *)(this+0x1bc) = (int16_t)w;   // current width
  *(int16_t  *)(this+0x1be) = (int16_t)h;   // current height
  // NOTE: +0x1c0 (depth) is NOT written by this function.
  *(uint32_t *)(this+0x1c8) = 0; *(uint32_t *)(this+0x1c4) = 0; // viewport x,y
  *(uint32_t *)(this+0x1cc) = w; *(uint32_t *)(this+0x1d0) = h; // viewport size
  *(uint32_t *)(this+0x1dc) = w; *(uint32_t *)(this+0x1e0) = h;
  DAT_005e93c0 = 0; DAT_005e93c4 = 0; DAT_005e93c8 = w; DAT_005e93cc = h; // window rect
  if (*(char *)(this+0x1c2) != 0) { GetClientRect(hwnd, &r); ... } // dead: returned above
  *(uint32_t *)(this+0x1dc) = *(uint32_t *)(this+0x1cc);
  *(uint32_t *)(this+0x1e0) = *(uint32_t *)(this+0x1d0);
}
```
Format-selection code as decompiled (for reference only):
```
depth==0x10: if R5G6B5 ok -> 0x17; else if (X1R5G5B5 failed && X8R8G8B8 ok) -> 0x16;
             else -> 0x18
else:        if A8R8G8B8 ok -> 0x15; else if X8R8G8B8 ok -> 0x16;
             else if R5G6B5 ok -> 0x17; else if X1R5G5B5 ok -> (keep +0x17c);
             else -> 0x18
```

## Files
- edit: `src/script/natives.cpp` only.
  - Add the state block next to `g_default_text_color` (line ~4970):
    ```cpp
    // java.render.GfxEngine display state (notes/12 §9): sorted mode list
    // DAT_005eb450 (count DAT_005eb454, 0x14-byte entries) and C3DLow's current
    // mode +0x1bc/+0x1be/+0x1c0 (i16), +0x1c2 windowed. Ctor 0x4223a0:
    // 640x480x16, fullscreen.
    struct GfxDisplayMode { int32_t width, height, depth; }; // entry +0/+4/+8
    std::vector<GfxDisplayMode> g_display_modes; // shortcut: no enumerator in src
    int16_t g_video_width = 640;    // +0x1bc
    int16_t g_video_height = 480;   // +0x1be
    int16_t g_video_depth = 16;     // +0x1c0
    bool g_video_windowed = false;  // +0x1c2
    ```
  - Implement `gfx_num_display_modes`, `gfx_curr_display_mode`,
    `gfx_display_mode_name`, `gfx_change_video_mode` (static `Vm&, const Args&,
    Value*, std::string*`), each with its `// 0xADDR Name` comment:
    - num/curr: ignore args; `Value::make_int`.
    - name: `want(who, a, 1)`, `get_i(who, a[0], &i, err)`; in range ->
      `snprintf(buf, sizeof buf, "%d %d %d", m.width, m.height, m.depth)` then
      `Value::make_str(buf)`; else `Value::make_str("")`.
    - changeVideoMode: `want(who, a, 3)` + `get_i` on a[0..2]; port the two
      early-outs exactly (compare against the i16 fields, store `(int16_t)`);
      then set only `g_video_width/g_video_height`; leave `g_video_depth`
      untouched (exe quirk). Output null.
  - Point the four registry rows (line ~5434) at the functions:
    `nullptr, true` -> `gfx_..., false`.

Marks required in code:
- `shortcut:` format selection (+0x17c/+0x19c/+0x1b5) and the viewport/window
  writes (+0x1c4..+0x1e0, DAT_005e93c0..cc) are not modelled: src has no D3D
  device/reset. The exe returns void, so the native's modelled effects are the
  two guards plus the current width/height.
- `shortcut:` src never runs `Game__initVideo` (0x421280), so the current mode
  stays at the C3DLow ctor values and `g_video_windowed` stays false.
- `UNRESOLVED:` display-mode list builder (DAT_005eb450/DAT_005eb454) not
  traced; notes/12 §9 names it "vf3" without a confirmed address. The list is
  empty, so num=0 / curr=0 / name="".
- `UNRESOLVED:` the last two dwords of the 0x14-byte entry are not read by these
  natives and were not identified.

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
