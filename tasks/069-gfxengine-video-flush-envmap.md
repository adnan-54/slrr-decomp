---
id: 069
status: todo
branch:
depends: []
---
# render: GfxEngine flush/renderOK/video/envmap natives

## Goal
Turn seven `java.render.GfxEngine` natives from stubs into the exe's behaviour:
`setGlobalEnvmap`, `flush`, `renderOK`, both `openVideo` overloads, `closeVideo`,
`isPlayingVideo`. All are declared **static native** (2.2.1 source
`bin/2.2.1 sources/system/Scripts/render/src/GfxEngine.java:12-24`; the 2.3.1
native table is `analysis/natives.tsv:245-251`), so in our VM the first declared
argument is `a[0]` (`Vm::run_native` only pushes a receiver for instance
methods, src/script/vm.cpp:1608).

Only `setGlobalEnvmap` has an effect src can carry: the game's env-map ref node.
The video backend (DirectShow) and the render-frame host do not exist in src, so
those natives return exactly the exe's values for the uninitialised video
subsystem / pre-first-frame cases. Do **not** invent a video backend or a render
host, and do not make them throw.

## Exe evidence (cleaned pseudocode, renamed fields)

### 0x4dc910 setGlobalEnvmap(ResourceRef)V
Disassembly (static native, `getArgs` = `Native__getArgs 0x4b1af0`):
```
55 8b ec 51            push ebp; mov ebp,esp; push ecx
8d 45 fc 50            push &arg
6a 00                  push 0
ff 75 08               push ctx
e8 ce 51 fd ff         call 0x4b1af0            ; getArgs(ctx, 0, &arg)
8b 45 fc               mov eax,[arg]
83 c4 0c               add esp,0xc
8b 0d 14 a0 5d 00      mov ecx,[0x5da014]       ; ECX = game
ff 70 0c               push dword [eax+0xc]     ; arg ref node's RI field
8d 89 98 00 00 00      lea ecx,[ecx+0x98]       ; game's embedded ref node
e8 84 01 f3 ff         call 0x40cac0            ; ref-node rebind
8b e5 5d c3            mov esp,ebp; pop ebp; ret
```
`FUN_0040cac0` (this = a 0x10-byte ref node `{+0 prev, +4 next, +8 id, +0xc owner}`):
unlink from the old owner's list (owner+0x78), link into the new owner's list,
`node.id = owner+0x80`. It is the same helper `ResourceRef.set(ResourceRef)`
uses (0x4e4350), already reimplemented as `Runtime::bind`
(src/script/natives.cpp:1512 `rr_set_ref`, src/game/runtime.cpp:216).

Offset note: notes/12 §2.1 says `Game__renderFrame` reads the env-map resource at
`game+0xa4`. That is consistent: the node is at `game+0x98`, its RI at node+0xc
= `game+0xa4`.

The exe does not null-check the argument: a Java-null argument faults on
`[eax+0xc]` at 0x4dc92e. An existing object with an unbound ref node yields 0,
which unlinks the game slot.

### 0x4dc840 flush()V
Bytes (0x4dc840, 0x1e long):
```
6a 00                  push 0
e8 19 8d 00 00         call 0x4e5560            ; GfxEngine.forceRendering, arg unused
8b 0d 08 b0 5d 00      mov ecx,[0x5db008]       ; ECX = resource manager
83 c4 04               add esp,4
6a 01                  push 1
e8 49 ae 03 00         call 0x5176a0            ; resource-manager flush(1)
e9 e4 65 f4 ff         jmp 0x422e40             ; tail call, no ret
```
- `0x4e5560` = `for (7) { 0x517f60(); 0x518f80(); DAT_005e8ca0 = 0;
  Game__renderFrame 0x421920(); DAT_005dadbc++; }` (notes/12 §2).
- `0x5176a0(param)` with `this = [0x5db008]`, arg 1: sets the four per-type
  force gates `+0x16d858/5c/60/64` and uses an age threshold of 1, i.e. forces
  every releasable resource node out (Ghidra body, notes/12 line 394).
- `0x422e40`: `game = [0x5e938c]; if (game) { if (x = game[+0x33c]) x->vtbl(1)
  [0x44dcb0]; if (x = game[+0x320]) x->vtbl(1) [0x4438b0]; if (x = game[+0x324])
  x->vtbl(1) [0x443780]; }` — three per-frame subsystem updates (identities
  UNRESOLVED).

### 0x4dc900 renderOK()I
```
0f b6 05 f8 e0 5f 00   movzx eax, byte ptr [0x5fe0f8]
c3                     ret
```
`DAT_005fe0f8` is written only at 0x47b5cd inside `Game__renderAndMeasure`
0x47b5b0: the return value of `Game__renderFrame` 0x421920 (0 = frame skipped).
Zero before the first rendered frame.

### 0x4dc870 openVideo(String,int,int)I
```
getArgs(ctx, 0, &file, &a, &b);           // 0x4b1af0
if (file == 0) return -1;                 // 0x4dc882
return Video__open(file, a, b);           // 0x44e9b0
```
`Video__open` 0x44e9b0 first line: `if (DAT_005ef0c8 == 0) return -0x7fffbffb;`
(0xfffffffffffffffb in the decompiler; 32-bit E_FAIL = **0x80004005**).
`DAT_005ef0c8` is set to 1 only by `Video__init` 0x44e910
(`CoInitializeEx(NULL, 2); DAT_005ef0c8 = 1;`). Otherwise the function builds a
DirectShow graph (`CoCreateInstance` CLSID_FilterGraph, "TEXTURERENDERER",
"SOURCE"/"Output" pins, 5 interface globals), copies the file name to the
global buffer at 0x5eeebf (Ghidra writes it 1 byte past the loop end — exe
quirk), stores `DAT_005da2b0 = a` (non-exclusive), and on success calls
`Video__start(b)` 0x44edc0, returning 0; every failure returns E_FAIL or the
failing HRESULT.

### 0x4dc8b0 openVideo(String,int,int,int)I
```
getArgs(ctx, 0, &file, &a, &b, &c);       // 0x4b1af0
if (file == 0) return -1;
Video__setLoop(c);                        // 0x44ef50: DAT_005ef0d4 = c
return Video__open(file, a, b);           // 0x44e9b0
```
The 4th argument is stored **only when the name is non-null**, before the open.
`DAT_005ef0d4` is read only by video-backend paths (0x44e8f3, 0x44f109) and
reset by `Video__close`; its meaning is UNRESOLVED (notes/12 calls it "setLoop";
the 3-arg form already passes its 3rd argument as the playback loop to
`Video__start`).

### 0x4dc830 closeVideo()V
`e9 4b 1f f7 ff` = `jmp 0x44e780` (`Video__close`). Its entire body is guarded
by `if (DAT_005ef0c8 != 0)`: it would set `DAT_005ef0cc = 0`,
`DAT_005ef0d4 = 0`, stop/enumerate the graph, release all interfaces.

### 0x4dc860 isPlayingVideo()I
`e9 3b 21 f7 ff` = `jmp 0x44e9a0`:
```
a1 cc f0 5e 00         mov eax,[0x5ef0cc]
c3                     ret
```
`DAT_005ef0cc` is set to 1 only by `Video__start` 0x44edc0
(IMediaControl::Run succeeded), cleared by `Video__close`.

### Service-missing values (src has no COM/DirectShow host)
`Video__init` 0x44e910 is never run, so `DAT_005ef0c8` stays 0 for the rebuild:
- `openVideo` with a null name → `-1`;
- `openVideo` with a non-null name → `0x80004005` (E_FAIL, the exe's
  uninitialised-video branch);
- `closeVideo` → no-op (guard fails);
- `isPlayingVideo` → `0` (`DAT_005ef0cc` initial value).

## Changes
1. `src/game/runtime.h` — add one public field to `Runtime`, directly after the
   `struct Handle` definition:
   ```cpp
   // The game's embedded env-map ref node (game+0x98; its RI is game+0xa4).
   // 0x4dc910 GfxEngine.setGlobalEnvmap rebinds it via FUN_0040cac0;
   // Game__renderFrame 0x421920 step 2 resolves it to a texture. No reader yet.
   Handle global_envmap;
   ```
   (It must live on `Runtime`, not in a file-static: `bind` links the node into
   `Resource::handles` and a runtime's resources die with it.)

2. `src/script/natives.cpp` — insert the block right after
   `tx_get_default_color` (before the Viewport lifecycle block) and point the
   seven registry rows at the functions.

   ```cpp
   // ---- java.render.GfxEngine: video, flush, envmap (notes/12 §2, §9) --------

   // DAT_005ef0d4: 4-argument openVideo's extra option (0x44ef50 stores it).
   // shortcut: only the DirectShow paths read it; kept to match 0x4dc8b0's write.
   int32_t g_video_option = 0;

   // 0x4dc910 GfxEngine.setGlobalEnvmap(ResourceRef)V
   bool gfx_set_global_envmap(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("GfxEngine.setGlobalEnvmap", err)) return false;
       if (!want("GfxEngine.setGlobalEnvmap", a, 1, err)) return false;
       // static native: the declared argument is a[0] (getArgs(ctx, 0, &arg) at 0x4dc91d)
    Runtime::Handle *src = value_handle(a[0]);   // arg ref node +0xc
       // INVENTED: a Java-null argument faults in the exe (no check at
       // 0x4dc92e); here it clears the slot like an unbound ref node.
       game_rt()->bind(&game_rt()->global_envmap, src ? src->target : nullptr);
       *out = Value::make_null();
       return true;
   }

   // 0x4dc840 GfxEngine.flush()V
   // exe: forceRendering(0) [0x4e5560: 7x {0x517f60, 0x518f80, DAT_005e8ca0=0,
   // Game__renderFrame 0x421920, DAT_005dadbc++}], resource-manager flush
   // 0x5176a0(1) on DAT_005db008, then 0x422e40 (three per-frame subsystem
   // updates on [0x5e938c]+0x33c/+0x320/+0x324).
   // shortcut: src has no render frame or resource-manager flush host.
   bool gfx_flush(Vm &, const Args &, Value *out, std::string *)
   {
       *out = Value::make_null();
       return true;
   }

   // 0x4dc900 GfxEngine.renderOK()I: movzx eax, byte [0x5fe0f8]; ret.
   // DAT_005fe0f8 is the result of the last Game__renderFrame, stored by
   // Game__renderAndMeasure at 0x47b5cd; 0 before the first frame.
   // shortcut: src never runs a render frame, so this stays 0.
   bool gfx_render_ok(Vm &, const Args &, Value *out, std::string *)
   {
       *out = Value::make_int(0);
       return true;
   }

   // 0x4dc870 GfxEngine.openVideo(String,II)I
   bool gfx_open_video(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("GfxEngine.openVideo", a, 3, err)) return false;
       if (a[0].kind == Value::Null) {           // 0x4dc882: null name -> -1
           *out = Value::make_int(-1);
           return true;
       }
       // SERVICE MISSING: Video__open 0x44e9b0 returns -0x7fffbffb (0x80004005,
       // E_FAIL) when Video__init 0x44e910 has not set DAT_005ef0c8. The
       // rebuild never runs Video__init (no COM/DirectShow host).
       *out = Value::make_int((int32_t)0x80004005u);
       return true;
   }

   // 0x4dc8b0 GfxEngine.openVideo(String,III)I
   bool gfx_open_video_ex(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!want("GfxEngine.openVideo", a, 4, err)) return false;
       if (a[0].kind == Value::Null) {
           *out = Value::make_int(-1);
           return true;
       }
       int32_t opt;
       if (!get_i("GfxEngine.openVideo", a[3], &opt, err)) return false;
       g_video_option = opt;                     // 0x44ef50 Video__setLoop
       *out = Value::make_int((int32_t)0x80004005u);
       return true;
   }

   // 0x4dc830 GfxEngine.closeVideo()V: jmp 0x44e780 (Video__close).
   // shortcut: Video__close's whole body is guarded by DAT_005ef0c8, which the
   // rebuild never sets, so there is nothing to release.
   bool gfx_close_video(Vm &, const Args &, Value *out, std::string *)
   {
       *out = Value::make_null();
       return true;
   }

   // 0x4dc860 GfxEngine.isPlayingVideo()I: jmp 0x44e9a0 = mov eax,[0x5ef0cc]; ret.
   // DAT_005ef0cc is set to 1 only by Video__start 0x44edc0 and cleared by
   // Video__close; neither is reachable without a video backend.
   bool gfx_is_playing_video(Vm &, const Args &, Value *out, std::string *)
   {
       *out = Value::make_int(0);
       return true;
   }
   ```

3. Registry rows (src/script/natives.cpp, the `java.render.GfxEngine` block):
   set `gfx_set_global_envmap`, `gfx_flush`, `gfx_render_ok`,
   `gfx_open_video`, `gfx_open_video_ex`, `gfx_close_video`,
   `gfx_is_playing_video` and change each row's `true` (explicit stub) to
   `false`. Leave `forceRendering`, `printScreen` and the four display-mode rows
   (card 068) untouched.

## Files
- edit: `src/script/natives.cpp` (7 functions + 7 registry rows)
- edit: `src/game/runtime.h` (`Runtime::global_envmap`)

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is
  `INVENTED:`, missing services are `shortcut:`/`SERVICE MISSING:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name

Notes for the implementer:
- Card 068 also edits this file (display-mode natives); keep changes local to
  the seven rows and the new block.
- `Video__init` 0x44e910 sets `DAT_005ef0c8` unconditionally, even if
  `CoInitializeEx` fails (exe quirk); it is still not run here.
- `0x4dc8b0` writes the option only on the non-null path — keep that order.
