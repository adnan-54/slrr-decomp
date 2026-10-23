# 12 — Render pipeline (D3D9 renderer `C3DLow`, scene passes, materials, meshes, textures, render natives)

Read from the exe with rea/Ghidra (pseudocode + local `objdump` where ECX was lost). New names: `analysis/names.render.json`
(223 entries; it also replaces the `C3DLow__vfN` labels of `names.json` with descriptive names — vf indices are kept in the table at the end).
D3D vtable offsets were decoded mechanically (IDirect3DDevice9 slot = offset/4, e.g. `+0xe4` SetRenderState, `+0x104` SetTexture,
`+0x10c` SetTextureStageState, `+0x114` SetSamplerState, `+0x164` SetFVF, `+0x190` SetStreamSource, `+0x148` DrawIndexedPrimitive).
Confidence tags as in the other notes: **observed** (read), **inferred** (from call shape/strings).

## 0. Key objects and globals
| what | where | notes |
|---|---|---|
| game object | `PTR_DAT_005da014` (= ECX of the main loop) | `+0x404` = `C3DLow*` (the renderer), `+0x14c` / `+0x198` viewport lists, `+0x100` overlay-quad list, `+0xb4` per-camera hook list, `+0xa4` global env-map resource |
| renderer | `DAT_005e938c` (`C3DLow*`, also game+0x404) | class `C3DLow` : `C3DLowInterface`, vtable 0x593bcc, 150 slots, object size **0x66c** (`C3DLow__create` 0x44f9f0) |
| IDirect3D9 | `C3DLow+0x14` (also `DAT_005e93fc`) | `Direct3DCreate9(0x20)` in the ctor |
| IDirect3DDevice9 | `DAT_005ee33c` | written by `CreateDevice` |
| HWND | `DAT_005e9394` | window class/title "Street Legal" |
| HINSTANCE | `DAT_005e939c` | `GetModuleHandleA(NULL)` |
| texture manager | `DAT_005e9390` (0x35dc bytes, also `C3DLow+0x32c`) | |
| frame counter | `DAT_005dadbc` | ++ after every rendered frame |

**Units (observed):** script space (metres) ×10 = render space. Natives multiply script positions by 10 (`Camera.setFog`, camera near/far) and divide by 10 on the way back (`Text.getPos`, `Viewport.unproject`, bone positions `×0.1`). Render `.scx` files store centimetres; the render loader multiplies by 0.1 (→ render units), bounding boxes are stored ×0.01 (→ metres). Collision `.scx` (note 11) uses ×0.01 directly.

## 1. Video init — `Game__initVideo` 0x421280 (thiscall on the game)
Args `(w, h, depth, windowed, vsync, msaa, aniso)` from the `video_*` config keys (00-overview). Order:
1. If `w==0 || h==0` → 640×480×32, windowed=1. If `DAT_005da01c` (shadow-map size, default 0x100) is 0 → `DAT_005da018` (shadow mode, default 2) = 0.
2. `game+0x404 = C3DLow__create()` → `C3DLow__ctor` 0x4223a0, then `vf0 initDevice(w,h,depth,windowed!=0,vsync!=0,msaa,aniso)`.
3. `vf30 setPerspective(π/2 = 0x3fc90fdb, aspect 1.0)`.
4. `Render__allocScratchBuffers(0x20,1,0x20,0x20,1,0x20,0x10,1,0x20)` 0x420350 (global 16-byte-element scratch arrays `DAT_005f05d4..63c`, `DAT_005f0784..794`, sizes in `DAT_005da354..3c8`).
5. `Mat44__setIdentity` on the game's matrix; if shadows: `DAT_005e8c88/8c = vf77 createRenderTexture(DAT_005da01c, 0)` twice (two square shadow targets), `DAT_005e8c90 = 0` (ping-pong index).
6. Allocates `DAT_005f05c0` (0xb78), `DAT_005eff5c`, `DAT_00773ad4` (0x402c) helper objects; once: a `CompatibleMesh_dynamic` (`DAT_005e8c94`, 0x128) with one `CM_Section_dynamic` (`DAT_005e8c9c`), its VB = `vf41 createVertexBuffer(0x400, FVF 0x12 = XYZ|NORMAL, 2)` and IB = `vf42 createIndexBuffer(0x400, 2)`.

### 1.1 `C3DLow__ctor` 0x4223a0 — defaults (offsets in the C3DLow object)
`+0x1bc/+0x1be` (u16) width/height = 640/480, `+0x1c0` (u16) depth 16, `+0x1c2` windowed=0, `+0x1b8` adapter (set by vf4), `+0x18` default **CULLMODE = 2 (CW)**, `+0x30` ambient 0, `+0x38` = 1.0, `+0x230` far = 125000.0 (0x47f42400), `+0x234` near = 8.0, `+0x238` default FOV 0.955 rad (0x3f747ae1), `+0x330/+0x334/+0x338` sampler MIN/MAG/MIP = 2 (LINEAR), `+0x494` (=[0x125]) 0x200, `+0x610` (=[0x184]) = 0x1000 (quad index-buffer budget), `+0x60c` = 2, 8 HW-light slots `+0x36c..+0x388` = −1 and enable bytes `+0x38c..+0x393` = 0, `+0x3f4` = −1, `+0x1b8` = −1 (“adapter not chosen”). Loads cursors 0x74–0x7e from the exe resources (`DAT_005e93d4..f8`, current `DAT_005e93a0`). Calls `vf3 enumDisplayModes(0)`.

### 1.2 Window — `C3DLow__createWindow` 0x423fd0
`RegisterClassA{style 0, WndProc = C3DLow__wndProc 0x427900, icon 0x6b, IDC_ARROW, BLACK_BRUSH, class "Street Legal"}`.
Fullscreen: `CreateWindowExA(WS_EX_TOPMOST, …, WS_POPUP|WS_VISIBLE, 0,0,0,0)`, `ShowWindow(SW_HIDE)`. Windowed: `CreateWindowExA(0x300 = WS_EX_CLIENTEDGE|WS_EX_WINDOWEDGE, …, WS_VISIBLE, 0,0,w,h)`, then `MoveWindow` so the **client area is exactly w×h**, `ShowWindow(SW_SHOWNORMAL)`; `SetFocus`.
WndProc: `WM_DESTROY` → PostQuitMessage; `WM_ACTIVATEAPP` → `DAT_005da030 = wParam` (render throttle flag); `WM_MOVE/SIZE/SHOWWINDOW/ACTIVATE` → client rect into `DAT_005e93c0..cc` (fullscreen: 0,0,w,h); `WM_SETCURSOR` → cursor or hidden; mouse messages (0x200–0x205): `DAT_005e93ac/b0` = pixel x/y, `DAT_005e93b8/bc` = NDC `2x/w−1`, `2y/h−1`, buttons `DAT_005e93b4`; `WM_SYSCOMMAND` SC_MAXIMIZE/MOVE/SIZE/KEYMENU swallowed in fullscreen; `WM_NCHITTEST` → 1 in fullscreen; 0x40d → `FUN_0044efc0` (DirectShow graph event); 0x464 → `FUN_00547e50`; 0x51 (WM_INPUTLANGCHANGE) → `FUN_00544cc0`.

### 1.3 `C3DLow__initDevice` (vf0) 0x425a30
1. Store size: `+0x1bc/+0x1be` (u16), `+0x1cc/+0x1d0` (int), `+0x1c2` windowed, `+0x1c4/+0x1c8` = 0 (viewport x,y), `DAT_005da038 = aniso`. Create window. Windowed: viewport size = client rect. `+0x1dc/+0x1e0` = viewport size copy. If adapter `+0x1b8 == −1`: `vf2 enumAdapters`, adapter := 0. `GetAdapterIdentifier(0,0,buf)`.
2. **Back-buffer format.** Fullscreen: probe `CheckDeviceFormat(0, HAL, X8R8G8B8, D3DUSAGE_RENDERTARGET, SURFACE, f)` for X8R8G8B8 (0x16), A8R8G8B8 (0x15); `CheckDeviceFormat(0,HAL,f,RT,SURFACE,f)` for R5G6B5 (0x17), X1R5G5B5 (0x18). depth==16: R5G6B5 → X1R5G5B5 → X8R8G8B8; else A8R8G8B8 → X8R8G8B8 → R5G6B5 → X1R5G5B5. Windowed: `GetAdapterDisplayMode(0)` format, size = client rect. `+0x1c0` (u16) = 32 for 0x15/0x16, 16 for 0x17/0x18.
3. **D3DPRESENT_PARAMETERS** (stored at `C3DLow+0x174..0x1ab`, i.e. `[0x5d..0x6a]`, reused by Reset):
   `BackBufferWidth/Height` = w,h (fullscreen) / client rect; `BackBufferFormat` as above; `BackBufferCount = 1`; `MultiSampleType = msaa` only if `CheckDeviceMultiSampleType(0,HAL,fmt,windowed,msaa,NULL)` succeeds (else 0); `MultiSampleQuality = 0`; `SwapEffect = 1 (DISCARD)`; `hDeviceWindow = hwnd`; `Windowed`; `EnableAutoDepthStencil = 1`; `AutoDepthStencilFormat = C3DLow__chooseDepthFormat(fmt)`; `Flags = 0`; `FullScreen_RefreshRateInHz = 0`; `PresentationInterval = vsync ? 1 (ONE) : 0x80000000 (IMMEDIATE)`.
   `chooseDepthFormat` 0x426b10: 16-bit order D16_LOCKABLE(0x46), D16(0x50), D15S1(0x49), D32(0x47), D24X8(0x4d), D24S8(0x4b), D24X4S4(0x4f); 32-bit order D32, D24X8, D24S8, D24X4S4, D16_LOCKABLE, D16, D15S1. First passing `CheckDeviceFormat(adapter,HAL,adapterFmt (A8R8G8B8→X8R8G8B8),D3DUSAGE_DEPTHSTENCIL,SURFACE,f)` **and** `CheckDepthStencilMatch`. (No fallback: falls off the end of the 7-entry array.)
4. **CreateDevice**(adapter `+0x1b8`, HAL, hwnd, flags, &pp, &`DAT_005ee33c`) tried with **0x44** (HARDWARE_VP|MULTITHREADED), then **0x84** (MIXED_VP|MT), then **0x24** (SOFTWARE_VP|MT); all fail → "Create hw 3d device FAILED", return 0.
5. `Video__coInit` (CoInitializeEx(NULL, APARTMENTTHREADED)). `+0x1b5` reset-pending = 0. `+0x3c = GetAvailableTextureMem()`.
6. Allocates `+0x320` (0x14), `+0x33c` (0x1c), `+0x324` (0x14) managers, the texture manager (0x35dc → `DAT_005e9390`, `+0x32c`).
7. **Quad index buffer** `+0x618 = vf42 createIndexBuffer(n, 1)` with `n = (+0x610 ≤ 0x400) ? 0x200 : +0x610/2` (default 0x800 indices), filled with `{0,1,3, 0,3,2}+4k` (quads → 2 triangles).
8. Init helpers `0x4447e0, 0x44bf80, 0x444840, 0x448820, 0x444c00`; `+0x5b0 = vf41 createVertexBuffer(0x400, FVF 0x144 = XYZ|DIFFUSE|TEX1, 2)`; `+0x598..+0x5a8` render-target table cleared.
9. Shadow-depth format `+0x5ac`: D24X8 (0x4d) if `CheckDeviceFormat(0,HAL,X8R8G8B8,DEPTHSTENCIL,SURFACE,0x4d)` else D24S8 (0x4b) else 0.
10. `GetRenderTarget(0, +0x344)`, `GetDepthStencilSurface(+0x348)` (the default back buffer / depth, restored by vf19(NULL)).
11. `C3DLow__setDefaultStates` (§1.4). A 4×4 texture `+0x340 = vf59 createTexture(4,4,1,slot 3)` filled with 0x80808080 (neutral grey, `Texture__uploadArgb`). `vf54 initMaterialShaders` (`+0xd8`). `C3DLow__initPostShaders` 0x428860: `CreateQuery(OCCLUSION)` probe → `+0x4a0` occlusion supported; pixel shaders `clear_below.psh` → `+0x498`, `blur.psh` → `+0x49c`.

### 1.4 `C3DLow__setDefaultStates` 0x426d10 (also after every Reset)
`vf8 clearAllBuffers`; `C3DLow__chooseTextureFormats` (§1.5); `GetDeviceCaps(+0x40)` (D3DCAPS9 lives at **`C3DLow+0x40`**, so caps field f is at `+0x40+f`: RasterCaps `+0x64`, MaxAnisotropy `+0xac`, MaxSimultaneousTextures `+0xd8`, VertexShaderVersion `+0x104`, PixelShaderVersion `+0x10c`).
- If `MaxAnisotropy && aniso`: MINFILTER := 3 (ANISOTROPIC). For samplers 0..7: MINFILTER = `+0x330`, MAGFILTER = `+0x334`, MIPFILTER = `+0x338`, MAXANISOTROPY = min(caps, aniso) (only if both ≠0), ADDRESSU = ADDRESSV = WRAP.
- `+0x24/+0x28/+0x2c` = 5/6/1; RS SRCBLEND = SRCALPHA, DESTBLEND = INVSRCALPHA; TSS0 ALPHAOP = MODULATE, ALPHAARG2 = TFACTOR; CULLMODE = `+0x18`; COLORVERTEX = 1; LOCALVIEWER = 1; LIGHTING = 1 (`+0x360` = 1); NORMALIZENORMALS = 0.
- `C3DLow__applyViewport` (SetViewport{`+0x1c4..+0x1d0`, MinZ 0, MaxZ 1}); `vf30 setPerspective(+0x238, 0)` (aspect 0 → w/h).
- `+0x170` = (VS version < 1.1), `+0x171` = (PS version > 1.0).
- Fog: RasterCaps & FOGTABLE → FOGTABLEMODE = LINEAR, else FOGVERTEXMODE = LINEAR; RANGEFOGENABLE = 1.
- `+0x5b4..` cleared; `C3DLow__buildDebugSphere` (wire-sphere vertex table at `DAT_005eb458`, stride 0x10, colour 0xffffffff).
- SetMaterial{Diffuse=(1,1,1,·), Ambient=(1,1,1,·), Specular 0, Emissive 0, Power 0}; `vf70 setAmbient(+0x30)`; AMBIENT/DIFFUSE/SPECULAR/EMISSIVE MATERIALSOURCE = MATERIAL; SPECULARENABLE = 0.
- Lights 0..7: SetLight{type 3 DIRECTIONAL, colours 0, Direction (0,−1,0), Range 1.0}, LightEnable(i, FALSE). Then `FUN_0043eff0` (material-state cache reset, inferred).

### 1.5 Texture-format slots — `C3DLow__chooseTextureFormats` 0x424e20 → table `DAT_005da278[14]`
Probed with `CheckDeviceFormat(adapter, HAL, displayFmt, usage, D3DRTYPE_TEXTURE, f)`. Static values in the image are the fall-backs; runtime values:
| slot | use | choice order |
|---|---|---|
| 0 | 16-bit opaque | R5G6B5 → X1R5G5B5 → A1R5G5B5 → X4R4G4B4 → A4R4G4B4 → X8R8G8B8 |
| 1 | 16-bit alpha | A4R4G4B4 → A1R5G5B5 → A8R8G8B8 |
| 2 | 32-bit opaque | R8G8B8 → X8R8G8B8 → slot0 → A8R8G8B8 |
| 3,4,5 | 32-bit alpha | A8R8G8B8 else slot1 |
| 6 | DXT1 source | DXT1 if supported else slot0 |
| 7 | DXT5 source | DXT5 if supported else slot1 |
| 13 | DXT3 source | DXT3 if DXT5 supported else slot1 |
| 8 | bump | V8U8 (static) |
| 9 / 10 | 32-bit render target / its depth | A8R8G8B8 → X8R8G8B8 → R8G8B8 (usage RT); depth D32 → D24X8 → D24S8 (+DepthStencilMatch) |
| 11 / 12 | 16-bit render target / depth | A1R5G5B5 → X1R5G5B5 → R5G6B5; D16 → D15S1. If only one RT class exists both pairs alias it |
`C3DLow+0xc` = all three DXT formats supported. Missing slot 0/1/2/3 or RT → MessageBox "No 16bit format found" / "No 32bit format found" / "… with alpha found" / "No RenderTarget format found" + quit.
DDS pixel-format → slot (`vf65` 0x44e020): DXT1→6, DXT3→13, DXT5→7, else 3; block bytes (`vf66`): DXT1 8, else 16.

## 2. Frame scheduling (main loop `0x47d760`, see 00-overview)
- `FUN_0054d3a0` (called when `java.render.Frontend` has a `render` monitor) **does not draw**: it pops the waiting script threads of the Frontend "render" event and `Thread::notify`s them (0x5629a0). Scripts that `wait()` on render are woken once per loop.
- Every `fps_lock` ms: if `Video__isFullscreenPlaying` 0x44e980 (`DAT_005ef0cc && !DAT_005da2b0`) → `Video__drawFrameQuad` 0x44ee30 (ZFUNC ALWAYS, ZWRITE off, FVF 0x104 = XYZRHW|TEX1, SetTexture(0, video tex `DAT_005ef0e4`), CLAMP U/V, SetStreamSource(0, `DAT_005eee98`, 0, 0x18), DrawPrimitive(TRIANGLESTRIP, 0, 2), restore WRAP/LESSEQUAL/ZWRITE). Else `Game__renderAndMeasure` 0x47b5b0 and `DAT_005dadbc++`.
- `Game__renderAndMeasure`: `FUN_0056a230(1)` (start timer 1), `DAT_005fe0f8 = Game__renderFrame()`, frame ms → 64-entry ring `DAT_005f0c98[frame & 63]`, smoothed `DAT_005f0c78 += (t − s)·0.1`, max in `DAT_005f0c94`.
- `GfxEngine.forceRendering()` (0x4e5560) = 7 × {resource tick `0x517f60`, `0x518f80`, `DAT_005e8ca0 = 0`, `Game__renderFrame`, `DAT_005dadbc++`}.

### 2.1 `Game__renderFrame` 0x421920 (ECX = game) — exact order
1. Zero per-frame counters (`DAT_005e8c78`, `DAT_005f07b0`, `DAT_005f05c4..cc`); `FUN_0046dc60` (expire/recycle render instances: hash table `DAT_005f0920` 32 buckets × 0xc).
2. `vf74(0)` (`+0x39c` global env-map texture := 0); if `game+0xa4` (GfxEngine.setGlobalEnvmap resource) resolves to a texture → `vf74(tex->+0x64)`.
3. **Present the previous frame**: `if (!DAT_005e8ca0) vf6 present()` = `Present(NULL,NULL,NULL,NULL)`, `DAT_005e9388++`. (`DAT_005e8ca0` is set by a camera that wants to skip one present, see `Camera__setup`.) Then clear the flag.
4. Purge dead viewport nodes of `game+0x198` and `game+0x14c` (`node+0x34 == 0` → `delete`).
5. `vf9 beginFrame()`; returns 0 → skip the frame.
6. For each node of `game+0x198`: `Viewport__purgeDead` 0x4654c0 (only frees dead children; this list is never drawn — **inferred**: inactive/deactivated viewports).
   For each node of `game+0x14c`: `Viewport__render(game+0x420, game+0x410)` 0x4653c0.
7. `Game__drawOverlayQuads` 0x421820: for each item of `game+0x100` whose resource is a texture with flag 4: `vf100 drawColorQuad(tex->+0x64, item+0x38)`, then delete the item (one-shot fades/overlays).
8. `vf10 endFrame()`; `FUN_00457250` (texture LRU: textures unused for >63 frames while the window is active are released); `DAT_005ef1c8 ^= 1`; `FUN_004536e0` / `FUN_00453510` / `FUN_004535e0` (per-frame streaming of texture/mesh uploads: the queues at `DAT_005ef338`, `DAT_005ef430`, `DAT_005ef240`, the last capped at `DAT_005da32c` items per frame); `FUN_00478de0` (skidmark ageing).
So one frame = Present(prev) → BeginScene … EndScene; the CPU work for frame N+1 overlaps the GPU flip of frame N.

### 2.2 `vf9 beginFrame` 0x422880
- Gamma (**observed**): when `DAT_005da034` differs from `DAT_005da044`: for i = 0..255 build `x = i·(1/255)` (ds:0x593e24), `y = pow(x, gamma)` (CRT `pow` 0x574d06), `y ·= 65535.0` (ds:0x593e38), clamp at 0xffff and store as u16, same for R/G/B → `SetGammaRamp(0, 0, ramp)`; `DAT_005da044 := gamma`. So `ramp[i] = min(65535, (i/255)^gamma · 65535)`. The config reader 0x47c2db..0x47c302 sets `DAT_005da034 = 1 / video_gamma` when the `video_gamma` key exists (`FLT 1.0 / value`), otherwise the ctor default 1.3 stays. Adjacent config: `shadow_detail` → `DAT_005e8ca4` (shadow strength threshold), `object_detail` → `DAT_005da338`.
- `TestCooperativeLevel`: DEVICELOST (0x88760868) → return 0 (skip). DEVICENOTRESET (0x88760869) or `+0x1b5` (mode change pending) → `C3DLow__releaseForReset` 0x422770 (unbind 8 textures, stream 0, indices; release default-pool VBs/IBs/RTs `0x443150/0x443120/0x44d4c0`, render targets `+0x598[+0x5a8]`, `+0x344/+0x348`, post shaders; `Video__closeRT`) → `Reset(&pp at +0x174)`; failure → `OutputDebugString("Reset failed\n")`, `_exit(-1)`; then `C3DLow__restoreAfterReset` 0x4268a0 (recreate pools, re-get RT/DS, re-set PROJ `+0x1e4` / VIEW `+0x23c`, viewport, `setDefaultStates`, `FUN_0044f300`).
- `C3DLow__pollOcclusionQueries` 0x428cb0 (flare visibility; either from queries or by reading a locked depth copy `+0x4a4`).
- If an offscreen RT `+0x4a4`: SetRenderTarget(0, it) → Clear(TARGET, 0, z 0.99999) → back to `+0x344`.
- Per-frame resets of the managers (`0x44dcb0`, `0x4438b0`, `0x443780`, `0x44dc00(1)`, `0x443760`, `0x443740`).
- `BeginScene`; `Video__drawFrameQuad` (video-to-texture background if playing); `vf67 setLighting(0, force 1)`; CULLMODE = `+0x18`; ZWRITEENABLE = 1; `+0x24 = +0x28 = 2`, SRCBLEND = ONE, DESTBLEND = ONE (`+0x28`); ALPHABLENDENABLE = 0; ZFUNC = LESSEQUAL; resets `+0x1b4`, `+0x624`, `+0x61c`, `+0x4b4..+0x4c0`, `+0x308/+0x30c`, `+0x4/+0x8/+0x10`.
### 2.3 `vf10 endFrame` 0x424970
If `+0x624` (blend override active) → `vf13 setBlendMode(2,1)` + `vf14 resetBlendMode`; if `+0x1b4` (offscreen RT bound) → `vf19 setRenderTargetTexture(0,0)`; if `+0x5c0` flag → flush (`FUN_00444810`, on error MessageBox + shutdown); `EndScene`.

## 3. Viewports and cameras
- Viewport node (`ViewPortNode`, in `game+0x14c`; `+0x34` = resource id of its `res_viewport` (type 0x12) resource; `+0x38` = viewport resource; `+0x44` camera list (`CameraNode`s, DoubleList); `+0x90` second list; `+0xd4..+0xe0` relative rect (x,y,w,h in 0..1, from the viewport resource data `+0x10..+0x1c`); `+0xe4..+0xf0` pixel rect = rel × backbuffer size (`Game__getViewportSize` = vf25); `+0x24` clear flags.
- `Viewport__render` 0x4653c0: `Viewport__updateRect` 0x464d30 (pixel rect, then `vf26 setViewportRect` with the float rect converted by `FUN_0056a740` = float→int), purge dead children, then **for each camera in `+0x44`: `Camera__setup(aspect = vpW/vpH)` 0x460840, `Camera__render(viewport)` 0x460ba0** (then a CFG `guard_check_icall` stub).
- `Camera__setup` 0x460840: `DAT_005f08e8 = camera` (current camera); one-shot “skip present” logic (`+0x73b0`, sets `DAT_005e8ca0 = 1` until frame `+0x73ac + DAT_005da398(4)`); clears `+0x738c..+0x7398`; then:
  - `FUN_00461b20(aspect)` → `FUN_00461dd0`: reads camera instance `+0xa9c` (camera type resource), instance data `+0x114` = type data. View matrix = instance `+0x1f4` object `+0xc0..+0xfc` if present, else identity; copies it to camera `+0x39c..+0x3d8` and the previous view to `+0x35c..+0x398`. From the type data: fov `+0x10`, near `+0x14`, far `+0x18`, aspect `+0xc`, fog enable `+0x24`, fog start `+0x1c`, fog end `+0x20`, fog colour `+0x28`, extra fog/shadow floats `+0x2c/+0x30/+0x34/+0x38`. Builds the projection into camera `+0x41c` via `vf32` (+0x80). Then `FUN_00567630`, `FUN_00460450`, `FUN_00567600(3)` (a vector, ×0.1) → camera `+0x338..+0x34c`, `FUN_00515c30(6, camera+0x1d4)`.
  - `FUN_0045e910`: sun/light setup for the camera. Current sun light = `DAT_005f06d4`; its camera link `+0xacc`; when `camera+0x1a8 == 0`, adds 2.0 to the projection extents at camera `+0x1e0/+0x1f0/+0x200/+0x210/+0x220` (inferred: depth range for the ortho shadow projection); converts the sun direction to render units (×0.1) into camera `+0x350..+0x358`; calls `FUN_0045eb10` (sun/cell objects, shadow+LOD paths, **partly read**) and `FUN_0045f880`.
  - `FUN_0045f880` — scene collection / light-table build: frustum-cull queue `DAT_005f0698/DAT_005f0694`, cell lists camera `+0x71a8/+0x71ac`, LOD lists `+0x6de0/+0x6de4`, projected-light receiver list `+0x6fc4/+0x6fc8`, per-camera instance hash at `+0xae4` (see §3.1). Light table `DAT_005f06f0` is copied from camera `+0x6bfc/+0x6c00` (0x8c-byte records starting at light `+0x48`, record index stored at light `+0xd8`; type `+0x48 == 3` also writes the sun direction to camera `+0x73b4..+0x73bc`); `vf68 setLightTable(table, n)`, `vf69 setActiveLights(0,0)`.

### 3.1 Camera object (CameraNode, vtable 0x5993c8, ctor in 0x45ce00..0x45d1a0) — embedded render groups
| offset | group (vtable) | ctor |
|---|---|---|
| +0x4b4 | `rg_text_section` (0x599148) | 0x45d840 |
| +0x528 | `rg_ground` (0x599170) | 0x45d330 |
| +0x5b4 | `rg_object` (0x5991c0) | 0x45d440 |
| +0x628 | `rg_objectprelit` (0x599210) | 0x45d5f0 |
| +0x69c | `rg_object_prelit` (0x599260) | 0x45d580 |
| +0x710 | `rg_object_merge` (0x599350) | 0x45d510 |
| +0x784 | `rg_transparent` (0x5992b0) | 0x45d8b0 |
| +0x80c | `rg_prelittransparent` (0x599288) | 0x45d740 |
| +0x890 | `rg_horizon` (0x599300) | 0x45d3d0 |
| +0x904 | `rg_particle` (0x599378) | 0x45d6c0 |
| +0x988, +0xa0c | `rg_skidmark` ×2 (0x5993a0) | 0x45d7c0 |
| +0xae4 | (sub-object, `0x45cd30`) | |
| +0x6b10 … +0x72a0 | 9 hash tables (16 buckets × 0xc, `0x4584b0`): per-camera instance caches (`(id & 0x7ff)*3 + 0x2b9` dwords from the camera = instance hash used by the material buckets), flare buckets at +0x6b10 | |
Other camera fields used by the render: `+0x1a4` clear mask (bit0 Z, bit1 colour), `+0x3cc..+0x3d4` sun direction (×0.1 for shadows), `+0x4a0/+0x4ac/+0x4b0` shadow params, `+0x6bfc/+0x6c00` light list (array of light instances, `+0x48` type 3 = directional, `+0x38/+0x3c` disabled flags, `+0xd8` index into the light table), `+0x6fc4/+0x6fc8` projected-light state, `+0x739c/+0x73a0/+0x73b0` “don't render” flags.

### 3.2 `Camera__render` 0x460ba0 — pass order (each “group” = begin/loop{render; next}/end, §4)
Skipped entirely if `+0x739c || +0x73a0 || +0x73b0`. `DAT_005f07a4` counts passes.
1. `mask = camera+0x1a4 & viewport+0x24`; if mask: `vf7 clear(colour = mask&2, z = mask&1, 0xff0000ff)` (Clear z = 0.99999, colour opaque blue — normally hidden by the sky).
2. **horizon/sky** `+0x890` (begin = `vf67 setLighting(0)` + `vf11 beginSkyView`: view matrix = inverse(inverse-view with translation zeroed), ZFUNC ALWAYS, ZWRITE off, FOG off if fog enabled; end = `vf12 endSkyView`).
3. `rg_ground` `+0x528` if `DAT_005da384`; `rg_object_prelit` `+0x69c` if `DAT_005da388`; `rg_object_merge` `+0x710` if `DAT_005da3a0`; `rg_object` `+0x5b4` and `rg_objectprelit` `+0x628` if `DAT_005da3a4` (all the `DAT_005da38x/3ax` toggles default 1 — debug switches).
4. `vf67 setLighting(0,0)`; `rg_skidmark` `+0x988`; `DAT_005f0660 = 0`; `rg_skidmark` `+0xa0c`.
5. `Game__cameraPreShadowHooks(camera)` 0x421760 (game `+0xb4` list items with flag bit0: call the hook object's vf7).
6. **Shadows** if `DAT_005da39c && DAT_005da018`: `vf13 setBlendMode(2, 1)`; `FUN_00459cb0` resets the ground-shadow quad table (`DAT_005f0578`, entries 0x30 B: texture `+0`, records `+0x24`, count `+0x28`, cap `+0x2c`); for each shadow caster in `DAT_005f06d8[DAT_005f06dc]` visible to this camera (`FUN_00463520`): `FUN_0045c7c0(sunDir·0.1, camera+0x4a0, +0x4ac, +0x4b0, shadowRT[DAT_005e8c90], camera+0x6fc4)`; then `FUN_0045add0` draws the accumulated ground-shadow quads; `vf14 resetBlendMode`. This is **projected textured geometry**, not stencil shadow volumes — see §3.4.
7. **Projected (dynamic point/spot) lights** if `DAT_005da020 && DAT_005da3a8 && vf93(1)` (vf93 = `(+0x7c >> 10) & 1`, a caps bit) and `DAT_005f06b8`: `vf67(0,0)`; material `DAT_005f0738 = vf50(0xfffffc)` (CMaterial_ProjectedLight, created once); gather the distinct type-4/5 lights of every affected instance (`inst+0x6e4[+0x6e8]`, light record type at `DAT_005f06f0 + i*0x8c`) → `vf69 setActiveLights(list, min(n,8))`; if `+0x6fc8`: identity world, then for each receiver (`DAT_005f0910[DAT_005f0914]`, mesh = `+0xc4 → +0x128`): `vf56 beginMaterial(mat, 0)`, for pass 0..7: `vf57 setPass`, draw each section whose `section+0x44` flag is set (`vf46 drawSection`), `vf58 endMaterial`.
8. `rg_prelittransparent` `+0x80c` if `DAT_005da3ac`; `rg_transparent` `+0x784` if `DAT_005da3b0`.
9. `vf67(0,0)`; `DAT_005f0660 = 1`; `rg_skidmark` `+0xa0c` again (second half).
10. Directional lights: every camera light with type 3 and not disabled → index list `DAT_005f0744` → `vf69 setActiveLights`; `vf67(0,0)`; `rg_particle` `+0x904`.
11. **Lens flares**: `vf102 beginOffscreen` 0x427d30 (bind flare/glow RT); copy camera `+0x3cc/3d0/3d4` (sun dir) to `DAT_005f0664/68/6c`; `DAT_005f0680 = 0` (0x1c-byte quad-record count), `DAT_005f070c = 0` (0x74-byte flare-record count); for the 16 flare buckets at `camera+0x6b10` (stride 0xc; bucket object `+4`, next `+8`) call `Camera__collectFlares` 0x4630a0; `vf103 endOffscreen` 0x428970; `vf13 setBlendMode(0,1)` (additive, Z-write off); for each flare record i < `DAT_005f070c` with `+0x70 != 0`: `vf113` 0x444e50 begin flare material, `vf114 drawLensFlare(DAT_005f067c + (record+0x64)·0x1c, count = record+0x68)` 0x445600, `vf115` 0x445010 flush/end; `vf14 resetBlendMode`; `vf69 setActiveLights(0,0)`. Details in §3.5.
12. `rg_text_section` `+0x4b4` (3-D text / OSD); `Game__cameraPostHooks` 0x421690.
13. Identity world (`vf33`); debug lines: `DAT_005f06a0` (paged 1024×0x1c records) via `vf138 drawLines3D`, `DAT_005f06c0` via vf138, `DAT_005f0714` via `vf139 drawLine2D`; buffers freed each frame. `DAT_005f08e8 = 0`.

### 3.3 UI / 2-D path (overlay quads, text)
- **Overlay quads** (frontend fades): `Game__renderFrame` step 7 (`FUN_00421820` 0x421820) resolves each `game+0x100` item's texture resource (`item+0x30`; texture flag bit2 at resource data `+0x38`, wrapper `+0x64`) and calls `C3DLow vf100 drawColorQuad(wrapper, item+0x38)` 0x4289b0. `vf100`: bind texture via `vf19` (+0x4c); ZENABLE 0, ALPHABLENDENABLE 1, SRCBLEND 5 (SRCALPHA), DESTBLEND 6 (INVSRCALPHA), BLENDOP 1 (ADD), TEXTUREFACTOR = colour; TSS0 COLOROP/ALPHAOP = 2 (SELECTARG1), ARG1 = 3 (TEXTURE) for both (TSS states 1..6); SetFVF 4 (XYZRHW); `DrawPrimitiveUP(TRIANGLESTRIP, 2, quad, 0x10)`; restore ZENABLE 1, TSS0 MODULATE (4) with DIFFUSE (2) on colour+alpha, `vf149` re-applies the blend state. (observed)
- **Text**: `C3DLow vf129 drawText` 0x44c4a0, mode byte `C3DLow+0x5c4` (`[0x171]`):
  - mode 0 — immediate per glyph: `SetTexture(0, font atlas)` (font object `+4`, valid byte `+0xd`); pixel shader 0 (+0x15c); SetFVF 0x144 (XYZRHW|DIFFUSE|TEX1); sampler 0 state 7 = 0; ALPHABLENDENABLE 1, SRCBLEND 5, DESTBLEND 6; FOGENABLE off (restored). Glyph record stride 0x10: `+8` du, `+0xc` dv, `+0x10` u, `+0x14` v; u,v are multiplied by `*(int*)(font+4)` (atlas size). Per character `DrawPrimitiveUP(TRIANGLESTRIP, 2, verts, 0x1c)` (4 XYZRHW|DIFFUSE|TEX1 vertices).
  - modes 1/2 — batched: glyph quads are 0x70 bytes (4×0x1c) written by `FUN_0044bdc0` into the text-buffer manager (`+0x614/+0x624...`) and flushed with `FUN_00444810`; mode 2 performs a measuring pass first. Escape sequences start with 0x1b: `0x1b 'C'` + 8 hex chars = colour, `0x1b 'c'` = restore (inferred from the parse loop).
- **3-D text** in the world is the camera's `rg_text_section` group (`+0x4b4`), using the same font atlas.

### 3.4 Shadow pass (step 6) — projected shadows, no stencil volumes
Two independent projected-texture mechanisms; nothing in the exe builds stencil shadow volumes.
**A. Ground-shadow quads (camera step 6).**
- `Camera__render` step 6 clears `DAT_005f0578` (0x30-byte entries, count `DAT_005f057c`, cap `DAT_005f0580`; table grown by `FUN_005679b0`, records inside an entry are 0x50 bytes at entry+0x24) via `FUN_00459cb0`.
- For each caster object in `DAT_005f06d8[0..DAT_005f06dc]` that `FUN_00463520(camera, obj)` says is visible, `FUN_0045c7c0` (ECX = caster; args `sunDir·0.1`, camera+0x4a0, +0x4ac, +0x4b0, RT, camera+0x6fc4) computes a fade and draws:
  - fade: `mode = caster+0x5ac` (2→1, 1→length(caster+0x5bc..+0x5c4), 0→caster+0x5bc, else 0); `d² = Σ(param1[i] − caster+0x5b0[i])²`; if `d² ≥ 1e-4` → `f = sqrt(mode²/d²)·param2·param4 + param3`, clamp to [0,1], else `f = 1`. Draw only if `f ≥ DAT_005e8ca4` (`shadow_detail`).
  - `FUN_0045c6a0(rt, caster+0x6fc4 receiver list)`: `vf82` (+0x148, 0x44b9b0) with (rt, caster+0x5f0/5f4/5f8, caster+0x5fc/600/604, caster+0x620) sets the light view/UV matrices; then draws the caster's own material bucket list `caster+0x6dc` if non-empty and the receiver list has entries (`vf69 setActiveLights(caster+0x6e4/+0x6e8)`, `vf33 setWorldMatrix`, `FUN_0045b150`).
  - `FUN_0045c390(receiverList)`: for caster children in `+0`/`+4` and `+0x79`/`+0x7a` with flags `(obj+0x18 & 0x30000) == 0x20000` and `obj+0xf8` (mesh): computes bounds, calls `vf80` (+0x140, 0x44bb40) per receiver; when it returns 1, `FUN_0045b150` draws that receiver's material sections.
  - `FUN_0045b150`: for each receiver in `DAT_005f0910[0..DAT_005f0914]`: mesh = receiver+0xc4 → material at mesh+0x128; `vf56 beginMaterial(mat,0)`, `vf57` per pass (0..7), draw each section with flag `section+0x44` via `vf46 drawSection`, `vf58 endMaterial`.
  - `FUN_0045b530` (called unconditionally by `FUN_0045c7c0` after the draws) builds the CPU ground-shadow quads for objects with flags `&0x30000 == 0x30000` and `obj+0xf8 != 0`. It uses the object matrix at `obj+0x30` and the object's shadow parameters at `obj+0x10c/+0x110/+0x114` (3-vector), `+0x118/+0x11c/+0x120` (3-vector), `+0x124`, `+0x128`, `+0x150`, `+0x154`, `+0x158`, `+0x15c` (scalars), projects the mesh edges onto the ground, and appends to the per-texture entry in `DAT_005f0578` (dedup by texture resource). These parameters are on the render item (`+0xf8` = mesh), not on the mesh object.
  - `FUN_0045add0`: for each table entry with count: `vf104(entry texture)` begin textured quad batch, `vf106(entry+0x24, entry+0x28)` submit, `vf109()` end — plain textured quads.
- `vf82` 0x44b9b0(tex,p2..p4,p5..p7,size): size < 1e-4 → 0; `+0x3f8 = tex`, `+0x440/+0x444/+0x448 = p5..p7`, `+0x43c = size`; light view at `+0x44c` via `FUN_00423df0`; `+0x48c = 0`; builds the world→shadow-UV bias matrix (0.5/size) at `+0x3fc`; returns 1.
- `vf80` 0x44bb40: `|dot(n,dir)| > 0.99` → 0; `+0x3f8 = resource`, `+0x440..` = params, `+0x490 = 1`; light view `D3DXMatrixLookAtRH` at `+0x44c`; projection scale/bias `{2/size, −2/(dot·size)}` at `+0x3fc`; returns 1.
**B. Shadow-map render group `rg_shadow`** (vtable 0x5990f0; standard 9-slot render_group order, §4):
- `begin` 0x4591e0: if count `+0x78 != 0` tail-jumps `vf83` (+0x14c, 0x44a850) and returns 1.
- `render` 0x468310: for each caster `+0x74/+0x78`, if it has a mesh (`+0xc`) and `mesh+0xb0 == 0`, `vf85` (+0x154, 0x44b2f0) with (`section = caster+0x18`, position vec, scissor flag).
- `end` 0x459210: if count != 0 tail-jumps `vf84` (+0x150, 0x44b0d0).
- `add` 0x459a80 (grow `+0x40`, append, `FUN_0046a930`), `remove` 0x45b270 (swap-last, `FUN_0046b940`, `FUN_00459230` if `+0x24`), `isEmpty` 0x45b140 (`+0x78 == 0`). Who registers/drives this group was not traced (**inferred**: shadow-casting scene objects).
- `vf83` 0x44a850 (begin shadow RT): needs `C3DLow+0x3f8` wrapper with wrapper+4; `DAT_005eee8c = 1`; `+0x490 = wrapper`; `GetSurface` via wrapper+4 vtable +0x48; `SetRenderTarget(0, surface)`, `SetDepthStencilSurface(0)`; view `SetTransform(2, +0x44c)`; if `+0x43c > 1e-4` → `D3DXMatrixOrthoRH(+0x43c·2, +0x43c·2, −100, 100)` and `SetTransform(3, ...)`; `ZENABLE=0`, lighting off, `ZFUNC=ALWAYS(8)`, `ALPHABLENDENABLE=0`, `TEXTUREFACTOR=0xffffffff`.
- `vf85` 0x44b2f0 (draw one caster section, software world): optional full-screen viewport/scissor (`SCISSORTESTENABLE=1` when `DAT_005da268` and `C3DLow+0x64 & 0x1000000`); world matrix = rows `C3DLow+0x2c0..+0x2e8` with translation from param2, w=1; `SetTransform(0x100, m)`; `SetTexture(0,0)`; `CULLMODE=1` (NONE); `drawSection(param1)`; restore `CULLMODE=+0x18`.
- `vf84` 0x44b0d0 (end/restore main pass): `TSS0 COLOROP=MODULATE(4)`, `COLORARG2=TEXTURE(2)`; `ALPHABLENDENABLE=+0x1c`; lighting on; `ZENABLE=1`, `ZFUNC=LESSEQUAL(4)`; `SCISSORTESTENABLE=0`; `SetRenderTarget(0,+0x344)`, `SetDepthStencilSurface(+0x348)`; restore view/proj (`SetTransform(3,+0x1e4)`, `(2,+0x23c)`); `DAT_005eee8c=0`.
- Vtable slots `vf81/vf86/vf87/vf88/vf89/vf90/vf91/vf92` are **vtable-only (no live callers found)**: `vf86` 0x44a9b0 / `vf88` 0x44a200 / `vf90` 0x44a6e0 are alternative shadow-map pass begin functions (state setup incl. `ZENABLE=0`, `ZFUNC=ALWAYS`, `AMBIENT=0`, `TSS0 SELECTARG1`, and `DEPTHBIAS` 0xc3 / `SLOPESCALEDEPTHBIAS` 0xaf from `DAT_005da26c/270` in vf90); `vf87`/`vf89`/`vf91` restore helpers; `vf92` 0x44b4c0 builds `C3DLow+0x3ac` = `inverse(view)·m1·m2·bias` for the shadow comparison texture (z scale 0xffffff or 0xffff for 16-bit `DAT_005da2a0==0x50`, offsets `0.5+0.5/w`, `0.5+0.5/h`). Only `vf83/vf84/vf85` are live (via `rg_shadow`), and `vf80/vf82` via the ground path above.

### 3.5 Sprite/quad batches — flares, skidmarks, particles
Dynamic quad batch slots of `C3DLow` (all share the static quad index buffer `+0x618`, FVF `0x142 = XYZ|DIFFUSE|TEX1` unless stated):
| slot | addr | role |
|---|---|---|
| vf104 +0x1a0 | 0x446b00 | begin textured quad batch (bind texture, TSS/FVF, shared IB) |
| vf105 +0x1a4 | 0x4480f0 | variant begin (second texture / UV mode) |
| vf106 +0x1a8 | 0x446e90 | submit record array and draw |
| vf107 +0x1ac | 0x447170 | variant submit |
| vf108 +0x1b0 | 0x4476f0 | variant submit |
| vf109 +0x1b4 | 0x446d60 | end/restore batch |
| vf110 +0x1b8 | 0x448470 | skidmark begin (texture validity, FVF/TSS, dynamic VB) |
| vf111 +0x1bc | 0x4488b0 | `drawSkidmark` (point count at param+8, dynamic VB `+0x4c8`, descriptor `+0x4ac`; DrawPrimitive/DrawIndexedPrimitive) |
| vf112 +0x1c0 | 0x4486e0 | skidmark flush/end |
| vf113 +0x1c4 | 0x444e50 | flare begin material/batch |
| vf114 +0x1c8 | 0x445600 | `drawLensFlare` (screen-space flare quads) |
| vf115 +0x1cc | 0x445010 | flare flush/end |
| vf116 +0x1d0 | 0x4451f0 | emit one flare's geometry into the batch |
**Flares (observed).**
- `Camera__collectFlares` 0x4630a0: per flare bucket (object at bucket+4, next bucket+8; instance visibility byte `+0x38 == 0`), lazily creates one 0x74-byte record in `DAT_005f0708` (count `DAT_005f070c`, cap `DAT_005f0710`, grow-by-4 via `FUN_005679b0`); `FUN_00463f40(rec, instance, rec)` clears `rec+0x6c/+0x70` and releases instance `+0x1c`; zeroes `rec+0x10/+0x2c` when `DAT_005da024 == 0`; `FUN_0045c920(&DAT_005f0664, rec, out[7], rec+0x6c)`; `FUN_00462350(out)` appends the 7-float/0x1c-byte record to `DAT_005f067c[count DAT_005f0680, cap DAT_005f0684]`; `rec+0x68++`. `rec+0x64` is the first quad-record index for this flare (slice into `DAT_005f067c`), `rec+0x68` the number of quads. Records are rebuilt every frame (`DAT_005f070c = 0` at the start); there is no time-based flare lifetime.
- `FUN_0045c920`: computes `out[0..2]` = light direction (normalized from the renderer light `+0x7c/80/84` if its type `+0x48 == 3`), `out[3]` = colour from `rec+0x6c` switch (0 → global colour `+0x40`, 1 → `+0x1c`, 2/3 → packed ARGB expanded as float), `out[4] = rec+0xf8`, `out[5] = rec+0xfc`, `out[6] = rec+0x100` if `rec+0x2c` or `rec+0x10` else 0. When `out[6] != 0` it calls `vf116(rec, out)` to emit geometry into the batch.
- `vf113` 0x444e50: validates the flare texture wrapper (`rec+0`, valid byte `+0xd`) and global `DAT_005eee88 == 0`; resets batch counters `+0x510/+0x518/+0x520/+0x528`, stores tex record at `+0x504`; `TSS(0, ALPHAARG2, CURRENT)`, `SetTexture(0, wrapper+4)`, `CULLMODE=1 (NONE)`, `SetFVF(0x142)`, `SetIndices(+0x618)`, `FOGCOLOR=0` if fog enabled, identity world. On failure it clears the batch state and returns.
- `vf116` 0x4451f0: configured by `DAT_005da258`; for a record with texture `+0x18` (not-yet-valid) it emits a flare quad set: `setLighting(0,0)`, resets counters, `ZENABLE=0`, `CULLMODE=1`, `SetFVF(0x142)`, fog off, identity world, allocates vertices through the dynamic VB manager `C3DLow+0x4ac` (`FUN_00443d10`, 0x18-byte/xyz+diffuse+uv vertices), records the base vertex at `+0x51c`, and projects the 7-float params with the current view/proj into screen space.
- `vf114` 0x445600: `DAT_005da250` gate; requires an active flare texture (`+0x504`) and count > 0. First pass per 0x1c record: near-plane/depth cull using the view-projection rows and `rec+0xc` vs texture record `+0x58`, plus occlusion-query result (`rec+0x14`, `FUN_00428c50`); counts visible records. If the count exceeds the dynamic-VB capacity it recursively calls itself on chunks (`[eax+0x1c8]`), so long flare lists are split. It then emits screen-aligned textured quads (main batch, plus a second batch through VB `+0x508` with `ZENABLE=0` for the non-occluded/additive ghosts), incrementing the counters at `+0x510/+0x518` and `+0x520/+0x528`. Exact ghost layout (`rec+0x58/+0x5c…` spacing/size/colour rules) is only partly decoded (**inferred**).
- `vf115` 0x445010: `FUN_004450a0` flushes both batches: `SetStreamSource(dynamic VB +0x4ac, stride 0x18)` and `DrawIndexedPrimitive(TRIANGLELIST, 0, minIndex=+0x51c, numVerts=+0x518, 0, primCount=+0x510)`; if the record has flag `+0x60 & 8`, uses FVF `0x144` and stride `0x1c` (second UV); then the second batch (`+0x508`, stride 0x18, `ZENABLE=0`) with `+0x520/+0x528/+0x52c`; restores FVF 0x142 and `ZENABLE`. Then restores `FOGCOLOR=+0x358`, `TSS(0, ALPHAARG2, TFACTOR)`, `CULLMODE=+0x18`, `SetTexture(0,0)`, world `+0x2c0`; clears `+0x504`.
**Skidmarks.** `rg_skidmark` (two camera groups at `+0x988/+0xa0c`) drive vf110/vf112/vf111 through the RI cache entries. vf110 validates the texture (`rec+0`), resets `+0x4f0`, sets the same FVF/TSS and dynamic VB; vf111 requires `C3DLow+0x4c8` and consumes the point list (`param+8` = point count > 1), appending strip geometry to `+0x4ac` and drawing with the shared IB; vf112 flushes. Per-point widths/UVs and the fade/ageing (`FUN_00478de0`, §2.1) are not decoded (**outline**).
**Particles.** `rg_particle` uses the same vf104..vf109 family (vf104 begin, vf106/vf107/vf108 submit variants by blend/UV mode, vf109 end), with texture/record data from the particle RI; per-particle quad layout and blending selection remain **inferred**.

## 4. Render groups (`render_group`, base vtable 0x5990c8)
Vtable (9 slots): `[0]` deleting dtor, `[1] +4 begin() → bool`, `[2] +8 end()`, `[3] +0xc next() → bool` (more passes), `[4] +0x10 render()`, `[5] +0x14 add(item)`, `[6] +0x18 remove(index)`, `[7] +0x1c isEmpty()`, `[8] +0x20 matches(item)`.
Driver (camera and `render_group__renderChildren` 0x45f750): `if (g.begin()) { do g.render(); while (g.next()); g.end(); }`.
Base fields: `+0x04..+0x20` list links, `+0x24` parent/owner, `+0x28` DoubleList of child material buckets (head ptr at `+0x30`).

**Top-level groups** (`rg_object`, `rg_ground`, `rg_horizon`, …): `begin` sets state (`rg_object`: `vf67 setLighting(1,0)`; `rg_horizon`: `setLighting(0,0)` + `beginSkyView`), `next` = 0 (one pass; shared stub 0x461910), `render` = `render_group__renderChildren` (drive every child bucket), `end` (`rg_object`: `vf69 setActiveLights(0,0)`; `rg_horizon`: `endSkyView`), `add(item)` = find/create the bucket for the item (`FUN_00463850`/`0x463790`) and forward `bucket->add(item)`, `isEmpty` = child list empty, `matches` = 1.

**Material buckets** (`rg_object_material` 0x599198, `rg_objectprelit_material`, `rg_horizon_material`, `rg_object_merge_material`, `rg_object_prelit_material`): one bucket per (mesh `+0x74`, key `+0x78`, blend type `+0x88`, render variant `+0x90`) — `matches(item)` compares `item+0x10`, `item+4`, `item+0x1c`, `item+0xc → +0x128`.
- items are **per-instance cache entries** (stride 0x2c, class `RI_<kind>_instance_cacheentry`), not the RI instance itself: `item+0xc` = RI instance pointer, `+4/+8` = type/instance indices, `+0x18` = section data. They are created by the RI instance's vf5 (e.g. `RI_object__vf5` 0x469990 over the entry array at RI+0x11c; `RI_object__vf4(count)` 0x46acb0 returns `count*0x2c + RI+0x11c`). `+0x94` array, `+0x98` count, `+0x9c` capacity (grows by 0x40); `add` appends + `FUN_0046a930`; `remove(i)` swap-with-last.
- `begin` (0x45e7a0): if count && mesh data: `+0x84 = material = items[0]->+0x14`; mode `+0x8c` from blend type `+0x88`: {1→1, 2→5, 3→6, 4→7, 5→8, else 0}; `+0x7c = passes = vf56 beginMaterial(material, mode)`, `+0x80 = 0`; return passes ≠ 0.
- `next` (0x461920): `++pass < passes`.
- `render` (0x467c70): variant 0: `vf57 setMaterialPass(material, pass)`; for each cache entry: its RI instance id (`item+0xc → +0x98`) selects the per-camera light instance in the hash at `camera+0xae4` (bucket = `(id & 0x7ff)*0xc`; entry `{key, instance*, next}`, found instance stored in `camera+0x6b0c`); queue `{entry, lightInst ? lightInst+0x6e0 : 0 (light set)}` via the binary heap `FUN_0046b560` (keyed on the light-set pointer), then flush: `while (DAT_005f07d0) { (**(code**)(*heap[0] + 0xc))(); FUN_0046b4c0(); }` — i.e. cache-entry `vf3` = draw. Variants 1/2: per item set the colour override `FUN_00440a60(stage 0 or 2, r,g,b,a)` from `item+0xc → +0xcc` (ARGB / 255, i.e. **RenderRef.setColor**), `setMaterialPass`, `item->vf3()` draw (immediate); restore the material colour afterwards.
- `end`: `vf58 endMaterial(material)`. See §4.3 for what the cache-entry `vf3` draw does.

### 4.1 Material begin/pass/end (C3DLow vf56/57/58)
- `beginMaterial(mat, mode)` 0x43f3a0 (observed): `+0x314 = mode`; `n = mat->vf0(mode)` (CMaterial vtable slot 0 = number of passes and stores `mat+0x28 = mode`); if n: texture-manager `+0x27d8 = 0` (clear queued shader constants); mode 3 → RS 0x91 DIFFUSEMATERIALSOURCE = 1 (COLOR1), RS 0x92 SPECULARMATERIALSOURCE = 2 (COLOR2); `mat+0xc` (alpha ref) ≠ 0 → ALPHAREF(0x18) = it, ALPHATESTENABLE(0xf) = 1; `mat+0x10 & 0x2000000` (two-sided) → CULLMODE(0x16) = 1 (NONE).
- `setMaterialPass(mat, p)` 0x43f510: if material mode `+0x314 == 1`, `FUN_00444180` uploads the vertex-blend matrix palette (below); then `mat->vf1(p)` (CMaterial VTable slot 1; sets the textures/TSS/shaders of pass p, §6.2).
- `endMaterial(mat)` 0x43f460: `mat->vf1(−1)`; if mode == 1 `FUN_00444130` (VERTEXBLEND=0 + software-VP restore); `vf48(0,0)` clears the palette base; undo mode-3 material sources (RS 0x91/0x92 = 0), alpha test if `mat+0xc`, and CULLMODE back to `+0x18`; `+0x314 = −1`.

### 4.2 Blend modes — `vf13 setBlendMode(mode, zflag)` 0x422c60
`zflag` 0 → ZENABLE = 0, 1 → ZWRITEENABLE = 0 (anything else: no Z change). Sets `+0x1c` (blend on) = 1, then `+0x20` alpha-test, `+0x24` SRC, `+0x28` DST, `+0x2c` BLENDOP:
| mode | SRC | DST | OP | alpha test | meaning |
|---|---|---|---|---|---|
| 0 | SRCALPHA | ONE | ADD | on | additive (alpha-weighted) |
| 1 | SRCALPHA | ONE (dst 2) | REVSUBTRACT(2) | on | subtractive |
| 2 | SRCALPHA | INVSRCALPHA | ADD | on | alpha blend |
| 3 | ZERO | SRCCOLOR | ADD | off | multiply |
| 4 | ONE | ONE (dst 2) | MIN(5)? / op 5 | off | (op 5 = MAX) |
| 5 | ONE | ONE | op 4 (MIN) | off | |
| 6 | BLENDFACTOR(0xe) | INVBLENDFACTOR(0xf) | ADD | off | constant blend, BLENDFACTOR = `+0x34` |
| 7 | ZERO | ONE | ADD | off | colour write masked via blend |
Alpha test when on: ALPHATESTENABLE 1, ALPHAFUNC GREATER, ALPHAREF 4. Then ALPHABLENDENABLE/SRC/DST/BLENDOP are written. `vf14 resetBlendMode` 0x424a70: flush text (`C3DLow__flushText`), blend off, alpha test off, ZENABLE 1, ZWRITE 1. `vf149` re-applies `+0x1c..+0x2c`; `vf15` sets BLENDFACTOR `+0x34`.
(Ops: D3DBLENDOP 1 ADD, 2 SUBTRACT, 3 REVSUBTRACT, 4 MIN, 5 MAX — mode 1 uses 2 = SUBTRACT; the table keeps the raw values.)

### 4.3 Draw submission — `C3DLow vf46 drawSection` 0x443ea0, instance cache entries
- **Class layering (observed):** `TMR::RenderInstance` base vtable 0x599504, size 0xcc (`vf0` deleting dtor 0x467be0); slots 1–6 are the purecall stub 0x56ee6f, so the base is abstract and the bucket's `vf3` call must be a derived override. The concrete render items are `RI_<kind>_instance_cacheentry` objects (0x2c bytes, array at RI instance `+0x11c`, vf4(index) = `RI+0x11c + index*0x2c`): `vf3` draws, e.g. `RI_object_instance_cacheentry__vf3` 0x468cd0, `RI_ground…` 0x468b70, `RI_particle…` 0x468f60, `RI_skidmark…` 0x469100, `RI_text…` 0x4691f0, `RI_welding…` 0x469290. The RI instance classes themselves are `RI_object`/`RI_ground`/`RI_particle`/`RI_skidmark`/`RI_text`/`RI_welding` (override vf1..vf9).
- **Cache-entry draw (0x468cd0) — exact palette rule (observed):** fills a ~0x88-byte light/material key via RI `vf2` (when `DAT_005f07d8 == 0`; `RI_object__vf2` 0x46ad10 sets flag bits 0x40/4 and copies RI `+0xb0` → key `+0x64`, RI `+0xcc` → key `+0xc`); resolves the light instance in the per-camera hash (id = RI `+0x98`); builds the world matrix (RI `+0x9c == 0` → copy 16 floats from RI `+0x30..+0x6c`, else compute via `FUN_00422070`) and calls renderer `vf33` (+0x84) setWorldMatrix. Then:
  ```
  if (RI+0xd0 > 1) {                       // more than one palette matrix
      vf48(RI+0xd4, RI+0xd0);              // +0xc0: palette matrix base, count
      section = meshGroup[entry+4]->sections[entry+8];
      vf49(section+0x8, section+0xc);      // +0xc4: index array, count (vtable +0x38/+0x30)
  }
  ```
  This is **not** section-range selection: it enables the fixed-function vertex-blend matrix palette. `vf48` 0x4445b0 sets `+0x300 = base`, `+0x304 = count`, dirty mask `+0x310 |= 2`; `vf49` 0x4445d0 sets `+0x308 = index array`, `+0x30c = count`, same dirty bit. `FUN_00444180` then does: if `+0x300 && +0x30c > 0` → if `C3DLow+0xe8 < 2` `SetSoftwareVertexProcessing(1)`; `SetTransform(0x100 = D3DTS_WORLDMATRIX(0), base + indices[0]·0x40)`; if `count == 2` also `SetTransform(0x101, base + indices[1]·0x40)` and RS 0x97 VERTEXBLEND = 1. `FUN_00444130` clears VERTEXBLEND and restores software VP. Then draws one section (entry `+0x18`) or every section of the list (`FUN_0045c8f0` when a light instance was found). Each section goes to **`vf46` (+0xb8) drawSection**.
- **`vf46` exact D3D submission** (assembly-verified; `P = *(section+0)`):
  1. `ECX = *(C3DLow+0x32c)` (texture/state manager) → call `FUN_00442400` (flush pending render states, below).
  2. `SetFVF(P+0x8)`; `SetStreamSource(dev, 0, P+0x18, 0, P+0xc)`.
  3. If `u16(section+0x12)` (primitive count) == 0 → return.
  4. If `*(section+0xc)` (index-buffer holder) != 0: `SetIndices(dev, **section+0xc)`; `DrawIndexedPrimitive(TRIANGLELIST, P[0] + (s16)(section+0x14), 0, u16(section+0x10), (s16)(section+0x16), u16(section+0x12))`.
  5. Else `DrawPrimitive(TRIANGLELIST, P[0] + (s16)(section+0x14), u16(section+0x12))`.

  Section data (arg passed to vf46; `CM_Section_static::vf2/vf3` return `this+0x24`, which is exactly this sub-object):

  | off | type | meaning |
  |---|---|---|
  | +0x00 | u32 | P = vertex-buffer descriptor pointer (`+0` base vertex index added to draws, `+8` FVF, `+0xc` stride, `+0x18` IDirect3DVertexBuffer9*) |
  | +0x0c | ptr | ptr to a slot holding the IDirect3DIndexBuffer9* (0 ⇒ non-indexed) |
  | +0x10 | u16 | NumVertices |
  | +0x12 | u16 | PrimitiveCount (0 ⇒ skip) |
  | +0x14 | s16 | BaseVertexIndex offset, added to `P[0]` |
  | +0x16 | s16 | StartIndex |

- **Shader-constant queue + bank dispatcher (not render states).** `FUN_00442400` (`this` = texture manager `C3DLow+0x32c`) flushes pending `{reg, bankId}` pairs at `this+0x21d8` (stride 8), count `this+0x27d8`; dirty mask at `C3DLow+0x310`. Each pair is dispatched by `FUN_00441d20(reg, bankId, 0)` (jump table 0x4422b8) which computes one constant bank into the VS bank `+0x27dc` / PS bank `+0x33dc`; `FUN_0043e8e0` uploads the VS bank with device `+0x178` (SetVertexShaderConstantF) and the PS bank with `+0x1b4` (SetPixelShaderConstantF), then clears the dirty mask. Bank IDs (**observed**):
  | id | bank |
  |---|---|
  | 1 | world·view·proj (4 regs) |
  | 2 | view·proj (4 regs) |
  | 3 | world matrix rows (4 regs) |
  | 4 | inverse(world) |
  | 5 | transpose(inverse(world)) |
  | 6 | vertex-blend palette expansion (3 regs per matrix; base `+0x300`, indices `+0x308`, count `+0x30c`) |
  | 7 | transpose(view·proj) |
  | 8 | transpose(world·view·proj) |
  | 9 | 12 light vec4s (`FUN_0043f910` from the active light table) |
  | 0xa | fog `{1/(end−start), end·x}` |
  | 0xb | view z column |
  | 0xc | world·view |
  | 0xd | transpose(world·view) |
  | 0xe | projection rows |
  | 0xf | transpose(projection) |
  | 0x10 | camera position from inverse(view), w = 1 |
  Dirty-group filter in `FUN_00442400`: bank ids `{1,8,0xc,0xd}` → mask 3, `{2,7,0xb,0xe,0xf}` → mask 1, `{3,4,5,6}` → mask 2, `{9}` → 0x18, `{10}` → 4. Immediate constant writes use `FUN_00442300(reg, x,y,z,w)`.
- `vf44` 0x4267e0 = `drawSectionRange`; `vf45` 0x444000 = `drawIndexed` (`SetStreamSource(0, *(s+0x18), 0, *(s+0xc))`, `SetIndices(*arg2)`, `DrawIndexedPrimitive`); argument roles only partly read.

## 5. Transforms, projection, fog, lights
- Matrices in the C3DLow object: PROJ `+0x1e4` (64 B), VIEW `+0x23c`, inverse view (camera-to-world) `+0x27c` (valid flag byte `+0x2bc`), WORLD `+0x2c0`; dirty mask `+0x310` (1 proj/view, 2 world/uv, 4 fog, 8 lights, 0x10 world).
- `vf30 setPerspective(fovY, aspect)` 0x427540: aspect 0 → vpW/vpH; `+0x22c = fov`, `+0x228 = aspect`; **`D3DXMatrixPerspectiveFovRH(+0x1e4, fov, aspect, zn = +0x234, zf = +0x230)`** (right-handed); `+0x224 = 1/tan(fov·π/180…)` (projection scale, uses `fov*0.017453292`, i.e. treats it as degrees for this cached value — quirk). `vf27 setFarPlane(f)`: `+0x230 = f·1.1`; `vf28 setNearPlane`. `vf31 setOrtho`: `D3DXMatrixOrthoRH(2,2,0,1)`. `vf29 setProjectionMatrix(m)`.
- `vf34 setViewMatrix(view, invView|NULL)` 0x427750; `vf33 setWorldMatrix` 0x427810 (SetTransform WORLD + copy); `vf36 getFrustumPlanes(out[6×4])` builds 6 planes from the inverse view, fov and aspect, distances ×0.1.
- Sky view (`vf11`): VIEW = inverse(invView with translation zeroed) → camera-centred sky; ZFUNC ALWAYS, ZWRITE off.
- **Fog** `vf72 setFog({enable, start, end, colour})` 0x427260 (stored `+0x34c..+0x358`): if enabled and `DAT_005ee340` (override) → start/end = `DAT_005da03c/40` (50/250); FOGSTART/FOGEND (float bits), FOGCOLOR, FOGENABLE 1; `+0x35c` = fog colour ×4 per channel (clamped, for vertex-lit stuff). Disabled → FOGENABLE 0. Mode LINEAR (table or vertex, §1.4).
- `vf70 setAmbient(argb)`: RS AMBIENT, `+0x30`.
- **Light table**: `vf68 setLightTable(base, count)` → `+0x364/+0x368`, invalidates the 8 slot caches. Record = **0x8c bytes**: `+0x00` D3DLIGHT9 (0x68 bytes: type, Diffuse `+4`, Specular `+0x14`, Ambient `+0x24`, Position `+0x34`, Direction `+0x40`, Range `+0x4c`, Falloff `+0x50`, Att0/1/2 `+0x54/58/5c`, Theta `+0x60`, Phi `+0x64`) + `+0x68` intensity multiplier + engine data to 0x8c. Engine light types 4/5 = point/spot that are only drawn by the projected-light pass; `setActiveLights` maps them to 1/2 just for `SetLight` and restores the type.
- `vf69 setActiveLights(indices, n)` 0x427110: if lighting on (`+0x360`): for slot i < min(n,8): `SetLight(i, rec)` only when the index changed (`+0x36c+i*4`), `LightEnable(i,1)`; slots ≥ n disabled.
- `vf67 setLighting(on, force)` 0x426190: RS LIGHTING; off → disables all enabled HW lights (optionally resets them to a black directional light when `DAT_005da048`), on → re-applies `setActiveLights(+0x394, +0x398)`.
- `vf71 cpuLightVertices(verts, n, lightIdx[], nl, skipIdx, invWorld)` 0x4231f0: software per-vertex lighting for prelit geometry (vertex stride 0x28: pos 0, normal +0xc, ? +0x18, colour accum +0x1c..+0x24): start = ambient `+0x30`/255; point (1): `att = 1/(a0 + a1·d + a2·d²)` within Range, `N·L` diffuse + ambient term; spot (2): same with `cos(Phi/2)`/`cos(Theta/2)` cone falloff; directional (3): `−N·dir`. Intensity `+0x68` scales colours.

## 6. Materials (`CMaterial_*`, 7-slot vtables; factory `vf50 createMaterial(type, params, nparams, flags)` 0x42bff0)
| type id | class | | type id | class |
|---|---|---|---|---|
| 0 | Tex | | 0x30 | Color |
| 1 (0x19) | MixTA | | 0x31 | ColorSpecular |
| 2,6,0xa,0xe,0x12,… (all ≡2 mod 4 up to 0x26) | MixVCA | | 0x32 | ColorSpheremap |
| 3 (0x1b) | ColorMixTex | | 0x33 | ColorSpecularSpheremap |
| 4 | TexSpecular | | 0x34 | Water |
| 5 | MixTASpecular | | 0x35 | Holo |
| 7 (0x1f) | ColorMixTexSpecular | | 0x37 | MixConst |
| 8 (0x10,0x20) | TexSpheremap | | 0x15 | MixTASpecularSpheremapMasked |
| 9 (0x11,0x21) | MixTASpheremap | | 0x18 | TexBump |
| 0xb (0x13,0x23) | ColorMixTexSpheremap | | 0x1c | TexBumpSpecular |
| 0xc (0x14,0x24) | TexSpecularSpheremap | | 0x1d | MixTABumpSpecular |
| 0xd (0x25) | MixTASpecularSpheremap | | 0xfffffb | Weld |
| 0xf (0x17,0x27) | ColorMixTexSpecularSpheremap | | 0xfffffc | ProjectedLight |
| unknown id | → Tex (0) | | 0xfffffd / 0xfffffe / 0xffffff | DecalPreview / ShadowMap / ProjectedShadowTexture |
Ctor addresses are in names.render.json (`CMaterial_X__ctor`). Only the low 24 bits of `type` select. If `flags & 1`, the parameter list is serialised into a blob at `mat+0x14` (size `+0x18`: `{type, flags&~1, n, params…}`) so `vf51 createMaterialFromBlob` can rebuild it. Parameters are 0x24-byte records `{u32 kind, …}` built with `FUN_004411c0` (colour: slot 0 diffuse rgba, 1 ambient·intensity, 2 specular), `FUN_004412a0` (float: 0x1000000 power, 0x1000001 spheremap strength = (r+g+b)/6), `FUN_00441330` (texture: 0x6000000 base, 0x6000001 second/mix, 0x6000003 spheremap, 0x6000007 bump; second arg = texture index, third = UV set), `FUN_00441380` (0x6000005 texture with UV transform), `FUN_004412c0(0x5000000, 0x38)` (alpha ref 0x38).

### 6.1 SCX material record → material type (`vf52 createMaterialFromScx(rec, flags)` 0x42ba10)
Record (in memory; v3 files hold it verbatim, v1/v2 are converted, defaults by `ScxMaterial__setDefaults` 0x458390):
| off | type | meaning |
|---|---|---|
| +0x00 | u32 | record size (v1 0x38, v2 0x34, v3 from file) |
| +0x04..+0x10 | f32×4 | diffuse RGBA (A < 0.99 → transparent) |
| +0x14..+0x1c | f32×3 | ambient/emissive colour, scaled by +0x20 (default 1.0) |
| +0x24 | f32 | specular power |
| +0x28 | u32 | flags: bit0 transparent, bit1 → 0x80000000 material flag, 0x40 → MixTA, 0x1000 → MixVCA, (0x101==0x100) → ColorMixTex, 0x800 alpha-test(ref 0x38) instead of blend, 8 = has +0x42 |
| +0x2c | s16 | base texture index (−1 none) |
| +0x30 | s16 | “no-specular” texture (≠−1 disables specular selection) |
| +0x32 | s16 | spheremap texture |
| +0x34 | s16 | second texture (mix) |
| +0x36/+0x38 | s16 | bump texture (+0x38) |
| +0x3c | u32 | vertex stride (default 0x2c; v1/v2 0x34) |
| +0x40..+0x44 | s16×3 | UV-set/channel indices (all 0 → set to −1) |
| +0x46/+0x48 | s16 | UV set of base / second texture (+0x46 clamped to ≤2) |
| +0x5c..+0x64 | f32×3 | specular colour |
| +0x68 | char[] | name; `"_uvw"` + `'N'`/`'F'` at +0x6c adds a UV-transform texture param |
Selection: no textures → 0x30/0x31/0x32/0x33 (Color, +Specular if “bright” (any ambient·intensity channel > 0.1) and power > 8 and +0x30 == −1, +Spheremap if +0x32 ≠ −1); with textures → base family 0 Tex / 1 MixTA (0x40) / 2 MixVCA (0x1000) / 3 ColorMixTex (0x101==0x100, the 0x60000000 flags are dropped) + 4 if specular + 8 if spheremap. Material flags: |1 always, 0x80000000 if base texture present with UV, 0x40000000 transparent-blend, 0x20000000 alpha-tested.
`vf54 initMaterialShaders` 0x42d2b0 pre-creates one material of every shader-using class (registering the names "CMaterial_ColorSpecular", "…Spheremap", "CMaterial_TexSpecular", "CMaterial_MixVCASpecular", …, "CMaterial_Weld") and the vertex shader `water_tss.vsh` (`DAT_005ee380`), creates a 128×128 helper texture and a 100-vertex VB (FVF 0x83dc), and `DAT_005ee3b4 = vf50(0xfffffc)`. Vertex shaders: `Shader__loadVertex` 0x43ea60 uses **built-in shader tokens** for `mixtaspec_vcprelit.vsh` (0x594898), `mixtaspec_lightmapped.vsh` (0x594a18), `skinmesh1..4_diffenvmap.vsh` (0x594bb0, 0x594d80, 0x595018, 0x595308) and falls back to `D3DXAssembleShaderFromFileA("system\\shaders\\<name>")`; others used: `diff_envmap.vsh`, `bump.vsh/psh`, `bump_spec.vsh/psh`, `mixtabumpspec_vcprelit.vsh/psh`, `water_tss.vsh`; pixel shaders `clear_below.psh`, `blur.psh` (`Shader__loadPixel` 0x43e9e0, file only). Shader paths require VS ≥ 1.1 (`+0x170`); otherwise fixed function.

### 6.2 `CMaterial_*` vtable slot 0 (`vf0`, pass count) / slot 1 (`vf1`, pass setup)
Object layout (from the Color ctor 0x4296e0; the other classes are the same): `+0` vtable, `+4` type id, `+8` status bits (bit0 set if flags `&0x40000000`, bit1 if flags `&0x80000000`), `+0xc` alpha ref, `+0x10` flags (`0x2000000` two-sided, `0x40000000` transparent blend, `0x80000000` base texture/UV, `0x20000000` alpha-tested/fallback), `+0x14` parameter blob pointer, `+0x18` blob size, `+0x1c` **parameter record array**, `+0x20` record count, `+0x28` current mode (written by vf0). Records are 0x24 bytes `{u32 kind, ...}` built by `FUN_004411c0/412a0/41330/41380/412c0` (§6). Color has 5 records: slot0 `0x6000007` (bump), slot1 `0x6000004`, slot2 `0x6000005` (UV-transform base), slot3 colour kind 0 (diffuse), slot4 colour kind 2 (specular); hence the `+0x6c` (slot3) / `+0x90` (slot4) / `+0xb4` (slot5) / `+0xd8` (slot6) offsets used in the vf1 bodies. Texture record: `+4` `Texture` wrapper (`IDirect3DTexture9*` at wrapper+4, valid byte `+0xd`), `+8` address flags (bit0/1 suppress the CLAMP override). `vf0` stores the mode and returns the pass count; generic rule 1 pass, or 4 when `flags+0x10` has bit31; shared vf0 bodies: 0x4290a0 (ColorSpecular family) 1 pass for mode 0/1 else 0, 0x429510 (TexBump/TexBumpSpecular/Water/Weld) always 1, 0x462ea0 (ShadowMap/ProjectedShadowTexture) always 1. Color 0x428f10: mode 9 → 4 (global ProjectedLight), mode 3 → 0 if flags≥0 else 1/2 by optional params + `DAT_005da094`, else 1/4. ColorMixTex 0x428f80 has the same shape with `DAT_005da104/108/10c/110`. ProjectedLight is always 4; TexSpecular is 1 only when the base texture record is present and mode 0/1 (exact guard partly read). Other classes reuse these bodies; the full per-class pass-count table stays **partial**.

Bind helpers: `FUN_0043d990(stage, texrec)` = bind texture: if wrapper valid → `SetTexture(stage, wrapper+4)`, else neutral grey `C3DLow+0x340`; sampler ADDRESSU/V = CLAMP(3) unless texrec+8 bits 0/1; spheremap kind `0x6000003` with no texture uses global envmap `+0x39c`. `FUN_0043d930(stage, texrec)` = restore ADDRESSU/V = WRAP(1) unless the address bits are set, then `SetTexture(stage, 0)`.

Per-class vf1 (pass `p`). **Offsets below are raw parameter-array offsets**; `P = *mat+0x1c`, slot k at `P + k·0x24` (slot0 `P+0`, slot1 `P+0x24`, slot2 `P+0x48`, slot3 `P+0x6c`, slot4 `P+0x90`, slot5 `P+0xb4`, slot6 `P+0xd8`). TSS raw values: 1 COLOROP, 2 COLORARG1, 3 COLORARG2, 4 ALPHAOP, 5 ALPHAARG1, 6 ALPHAARG2, 0xb TEXCOORDINDEX, 0x18 TEXTURETRANSFORMFLAGS, 0x1c RESULTARG; D3DTOP 2 SELECTARG1, 3 SELECTARG2, 4 MODULATE, 7 ADD, 0xd BLENDTEXTUREALPHA, 0x12 MODULATEALPHA_ADDCOLOR, 0x14 MODULATEALPHAADDTOCOLOR, 0x19 MULTIPLYADD; D3DTA 0 DIFFUSE, 1 CURRENT, 2 TEXTURE, 3 TFACTOR, 5 TEMP, 0x10 COMPLEMENT:
| class / vf1 | observed pass setup |
|---|---|
| Color 0x42e4e0 | mode 0: if `flags+0x10 & 0x20000000 == 0` delegate to global ProjectedLight (DAT_005ee3b4); else p0 `FUN_00440b30(P+0x90, P+0x6c)`, TSS0 COLOROP+ALPHAOP = SELECTARG2, TFACTOR alpha = `P+0x80` × `C3DLow+0x38` × 255 << 24; p>0 TSS0 MODULATE. mode 3: `P+0xc != −1` → `FUN_00435500(p)`; else `P+0x28==0` → `FUN_0043ab60`, else `FUN_00436ab0`. mode 9: if `0x20000000` set → no output, else global ProjectedLight. |
| ColorSpecular 0x42f240 | p0 SPECULARENABLE(0x1d)=1; `FUN_00440ca0(P+0x6c, P, P+0x24, *(float*)(P+0x4c))`; TSS0 SELECTARG2; TFACTOR alpha = `P+0x14` × `C3DLow+0x38` × 255; p>0 SPECULARENABLE=0, TSS0 MODULATE. |
| Tex 0x434170 | p0 (mode 1): `FUN_00440b30(P+0xb4, P+0x90)`, `43d990(0, P+0x6c)`, TFACTOR alpha = `P+0xa4` × `+0x38` × 255; p>0 `43d930(0, P+0x6c)`. mode 3 helper triple: `P+0xc != −1` → `FUN_00436230`, else `P+0x28 == 0` → `FUN_00439350`, else `FUN_0043c9a0`. modes 5..8: `SetVertexShader(DAT_005ee370/374/378/37c)`, `FUN_00442300(0,1,0,0,0x443f40a4)`, `FUN_00442300(1, P+0x98..0xa4)`, bank ids (2,7),(6,9),(0x12,0xa),(0x13,0xb),(0x14,6), `43d990`, TFACTOR alpha; p≠0 unbind. mode 0/9 fallback as above. |
| MixTA 0x4304d0 | mode 0/1 (`DAT_005da09c` = 0/1/2): p0 binds stage0 `P+0x6c`, stage1 `P+0x90`, TSS0 SELECTARG1, TSS1 COLOROP=0xd BLENDTEXTUREALPHA, sampler1 MIPFILTER=`+0x338`; p1 `FUN_00440f50(p)` + vec4 `P+0xd8`, unbind both, TSS0 SELECTARG2, TSS1 DISABLE; p>1 `FUN_004406d0(p)` + TSS0 MODULATE. mode 2 shader path with TCI/TTF on stages 0/1/2; mode 3: `P+0xc != −1` → `FUN_00435b80`, else `P+0x28 == 0` → `FUN_004380e0`, else `FUN_0043ba70`; mode 9 global. TFACTOR alpha = `P+0xc8` × `+0x38` × 255. |
| MixVCA 0x433150 | mode 0/1 p0: `vf67 setLighting(0,0)`; stage0 `P+0x6c`, stage1 `P+0x90`; TSS0 COLOROP=0x14 MODULATEALPHAADDTOCOLOR, TSS1 COLOROP=0x19 MULTIPLYADD with COLORARG2=TEXTURE(4); TFACTOR alpha = `P+0xc8` × `+0x38` × 255. p1 `FUN_00440f50` + `FUN_00440b50(P+0xd8,1,1,1,1)`, unbind both, TSS0 SELECTARG2 with ARG2=CURRENT, TSS1 DISABLE ARG2=CURRENT; p>1 `FUN_004406d0` + MODULATE. mode 3: `P+0xc != −1` → `FUN_00435f60`, else `P+0x28 == 0` → `FUN_00438cc0`, else `FUN_0043c420`; mode 9/bit31 fallback. |
| ProjectedLight 0x4333d0 | vf0 always returns 4. If `DAT_005eecc4 == 0`: edi = `FUN_0043ed80(p)`, esi = 0; else edi = `FUN_0043ed80(2p)`, esi = `FUN_0043ed80(2p+1)`. If edi: `FUN_004409a0(1)`, ZENABLE=0, TFACTOR=0xffffffff, `FUN_00441450(edi, 0)`; if esi: `FUN_00441450(esi, 2)`, TSS2 SELECTARG1, TSS2 RESULTARG=TEMP(5), TSS2 ALPHAOP=SELECTARG1, TSS3 COLOROP=0x19 MULTIPLYADD COLORARG2=TEMP, TSS3 ALPHAARG1=TEMP, TSS3 ALPHAOP=ADD(7); returns 1. No texture (edi == 0): `FUN_00440620(1)`, ZENABLE=1, `FUN_00440780(0)`, TSS0 MODULATE; if `DAT_005eecc4 == 1` an extra stage2/3 combiner (this branch returns 0). |
| TexSpecular 0x434bd0 | p0 SPECULARENABLE=1; `FUN_00440ca0(P+0x90, P+0x24, P+0x48, float P+0x70)`; `43d990(0, P)`; TFACTOR alpha = `P+0x38` × `+0x38` × 255; p>0 SPECULARENABLE=0, restore sampler U/V WRAP, unbind stage0. |
| TexSpheremap 0x434ff0 | mode 0 (`DAT_005da0b8` 0/1/else): `FUN_00440b30(P+0xfc, P+0xb4)`, `43d990(0, P+0x90)`, `43d990(1, P+0x6c)`, `FUN_00441ad0(1)`; TSS1 COLOROP = ADD(7) or 0x19 MULTIPLYADD; TFACTOR alpha = `P+0xc8` × `+0x38` × 255; mode 3: `P+0xc != −1` → `FUN_00436610`, else `P+0x28 == 0` → `FUN_0043a130`, else `FUN_0043d1e0`; mode 9/bit31 fallback. |
| Water 0x435360 | mode 0: `DAT_005da060` 0 → `FUN_0042e220(p)`, 1/2 → `FUN_0042d860(p)`, else return 1 (all shader/water setup helpers; contents not read). |
| Weld 0x4353a0 | p0 `43d990` stage0 `P+0`, stage1 `P+0x24`; TSS0 COLORARG2=0x30, ALPHAARG2=0x10, TSS1 COLOROP=0x19, COLORARG2=0x20, ALPHAOP=0x19, ALPHAARG2=1 (raw values); p>0 `43d930` both, TSS0 ARG2=CURRENT, ALPHAARG2=TFACTOR, TSS1 DISABLE/ARG2=CURRENT/ALPHAOP DISABLE. |
| ShadowMap 0x433ee0 | 1 pass: `FUN_00440ec0`; D3DMATERIAL diffuse (0.76,0.84,0.87,1), ambient/spec/emissive `DAT_005eed6c..78`, SetMaterial; `SetTransform(0x10 TEXTURE0, C3DLow+0x3ac)`; sampler0 MIN/MAG=LINEAR, MIPFILTER=0, ADDRESS U/V/W=CLAMP; TSS0 TCI(0xb)=0x20000 (CAMERASPACEPOSITION), TTF(0x18)=3 (COUNT3), COLORARG2=0x12 (TEXTURE|COMPLEMENT); `SetTexture(0, C3DLow+0x3ec)`; p≠0 restores defaults + COLOROP MODULATE ARG1=CURRENT ARG2=TEXTURE. |
| ProjectedShadowTexture 0x4335f0 | if `C3DLow+0x3a0` table: entry = `*(table+0x8)` + (`C3DLow+0x3a8`)·0x4c; ZENABLE=0; tex0 = `(*(table+0x10))[entry+0x8]` (texture-record pointer array), tex1 = `DAT_005eeccc`; `SetTransform(0x10, entry+0xc)`; sampler filters; grey-blue material; TSS0/1 TCI=0x20000, TTF=2, TSS1 COLOROP MODULATE; p≠0 restores. Else fallback: tex0=`+0x3f8`, transform0=`+0x3fc`, TFACTOR=0xffffffff, material diffuse=`+0x38`; if `+0x48c == 0` texture1 matrix = inverse(view) scaled 0.25/`+0x43c`, offset 0.5 (else use `+0x404` row), `SetTransform(0x11, ...)`, TSS1 TCI/TTF/MODULATE. |
Remaining classes (ColorSpheremap, ColorMixTex*, MixTA*Spheremap, TexBump*, Holo, MixConst, DecalPreview, TexMulVC) are combinations of the same bind/helper bodies; their exact branch tuples and TSS enumeration are only partly decoded (**partial**; all addresses in `names.render.json`).

## 7. Meshes (`.scx` render loader) — `Resource_Mesh` → `CompatibleMesh_*`
Classes (`CRT_MESH_SCX` / `CRT_MESH_SCY` resource kinds): `CompatibleMesh_static` (vtable 0x599010: `[5]` load = `CompatibleMesh_static__load` 0x4515e0, `[3]` save `CompatibleMesh__saveScx` 0x450f00, `[4]` `makeStatic` 0x4561a0), `CompatibleMesh_ground` (0x59902c: `[5]` 0x451130), `CompatibleMesh_dynamic`, `CompatibleMesh_slot`. Mesh object: `+0x28` file name, `+0x38` state flags (|0x14 loaded, bit1 cleared), `+0x40/+0x44` file data/size, `+0x4c` load frame, `+0x50` total vertices, `+0x5c` bone count, `+0x60/+0x64/+0x68` section-group array (each `{material, sections[]}`, 0x18 bytes), `+0x6c..+0xa0` bounds (`+0x6c` kind 0 sphere/2 none, centre `+0x70`, radius `+0x7c..+0x84`, AABB centre `+0x8c`, half extents `+0x98`, in **metres**), `+0x104` first-vertex flag, `+0x108..+0x120` `.bon` bone table (0xa4-byte entries: name[32] at +4, 4×4 matrix at +0x24, translation ×0.1), `+0x124` skinned (bones ≥ 3), `+0x128` mesh material set (used by projected lights).
- File: `"INVO"` (0x4f564e49) + u32 version (5→3, 6→4 when `DAT_007a75e4`); else "Invalid mesh file! -> %s". v ≥ 4 → `Scx__loadRenderV4` 0x452960 (not read; chunked format of note 11). v1–3 → `Scx__loadRenderV1to3` 0x451b00; for v4+ files with flag `+0x3c&1`, a sibling `<name>.bon` text file (bone name, id, 4 rows `%f,%f,%f`) is parsed first (`fonts` in the path sets `DAT_005ef210`). Names ending in `y`/`Y` (`.scy`) set vertex flag 0x400.
- Per section (repeat until end of data; v3 stops at a zero dword): material record (§6.1); material = `vf52(rec, 0x80000000 unless the path contains "objects\\meshes")`; a `CM_Section_static` (0x54 bytes) with a vertex array (FVF/declaration 0x15003f, `FUN_00455210`) of `nv` vertices and an index array (u16, `3·nf`); `u32 nv`, nv vertices of `stride` bytes, `u32 nf`, nf × 3 u32 indices (written to the u16 index list).
  Vertex (v1/v2, stride 0x34): pos f32×3 (×0.1), normal f32×3, uv0 f32×2 (+0x18), uv1 f32×2 (+0x20), RGBA f32×4 → ARGB (v<3, multiplied by diffuse when the material is lit); v3: colour = u32 at +0x18-ish, then optional uv/extra fields depending on the stride (`+4/+8/+0xc/+0x14` beyond the base). Lit materials: diffuse vertex colour `0xff808080`, specular = vertex colour; unlit: diffuse = vertex colour|0xff000000.
- Ground meshes (`CompatibleMesh_ground__load`): same records, material `vf52(rec, 0x80000000)` and a 0x280-byte ground section (`FUN_0044fae0(0x80000001, 0x15003f, 0x400)`), plus a `CMaterial_ProjectedShadowTexture` (0xffffff) group with a FVF-3 section of 0x800 for shadow receiving; bounds kind 2.
- RPK cross-check: `Axis.rdb` type-5 (`res_mesh`) records hold `sourcefile cars\racers\Axis_data\…\*.scx` (58 scx), type-7 (`res_texture`) 31 `.png` — consistent with `CRT_MESH_SCX` and `CRT_TEXTURE_DX` (D3DX-loaded png/jpg/tga/bmp).

## 8. Textures — `Resource_Texture` → `CompatibleTexture_*`
Kinds: `CRT_TEXTURE_DX` (any D3DX-readable file), `CRT_TEXTURE_PTX` (engine format), `CRT_TEXTURE_DDS`. Object: `+0x28` name, `+0x38` flags (|0x14 loaded, 0x80000004 failed), `+0x40/+0x44` file data/size, `+0x50` VRAM bytes, `+0x64` device texture wrapper (`Texture`, 0x4c bytes: `+4` IDirect3DTexture9*, `+0x10` format slot, `+0x3c/+0x40` w/h, `+0x44` levels, `+0xd` valid), `+0x6c` format slot. Global stats `DAT_005f04f0/f4` (count/bytes), RT textures `DAT_005f04f8/fc`. Mip skipping: `DAT_005da338` levels dropped (config `texture_*`), min 4 px for DDS.
- **DX** (`CompatibleTexture_DX__load` 0x458050): `vf60 createTextureFromMemory(data, size, slot, 0)` → `TextureManager__createFromMemory` 0x44db00 = `D3DXCreateTextureFromFileInMemoryEx(dev, data, size, 0, 0, MipLevels = (arg4==0), Usage = slot∈{9,11} ? RENDERTARGET : 0, Format = DAT_005da278[slot], Pool = slot∈{9,11} ? DEFAULT : MANAGED, Filter = 0x80004 (TRIANGLE|DITHER), MipFilter = D3DX_DEFAULT, ColorKey 0)`. Note MipLevels is **1** for the arg4==0 call (as decompiled); if both sides ≥256 px a global LOD bias `DAT_005f0500 *= 0.95`.
  From file: `TextureManager__createFromFile` 0x44d9f0 (`D3DXCreateTextureFromFileExA`, same parameters); cube maps: `vf64` → `D3DXCreateCubeTextureFromFileExA` (0x44d760).
- **PTX** (`CompatibleTexture_PTX__load` 0x458120): header `+4` mip count, `+8` width, `+0xc` height, mip data from `+0x10`; each mip block `{u32 size, 0x10 more header bytes, data}` (next = p + size + 0x14); levels reduced by `DAT_005da338` with w,h shifted; texture created with `vf59 createTexture(w,h,levels,slot)`; uploaded one mip per call (`FUN_00456cf0`), streamed over frames.
- **DDS** (`CompatibleTexture_DDS__load` 0x457e90): standard 128-byte DDS: mips `+0x1c` (0 → 1), height `+0xc`, width `+0x10`, pixel format at `+0x4c` → `vf66` block bytes and `vf65` slot (DXT1→6, DXT3→13, DXT5→7, other→3); mips uploaded one per call (`FUN_00456c70`), so compressed data is converted when the device lacks DXT (slot fall-back to 565/4444).
- `TextureManager__create` 0x44d890 (`vf59`): levels 0 → full chain; reuses a freed texture of identical (w,h,levels,slot) from the free list `mgr+4`, else `new Texture(w,h,levels,slot)` (0x44cf50); VRAM accounting `mgr+0x14/+0x18`. `vf77 createRenderTexture(w,h)` = `vf59(w, h?:w, 1, slot 9)`.
- The `CompatibleTexture_*__duplicate` vf2 functions (0x457750/990/540) make a private, dynamic copy (texture capped at 256 or 512 px depending on `GetAvailableTextureMem` ≤64 MB / ≤128 MB) — used for paintable/decal textures (inferred: `PaintTexture`).

## 9. Script natives (java.render.*, java.util.resource.RenderRef) — behaviour
Convention: `obj → native handle` = `Native__instanceToHandle(obj, DAT_007a2620)`; a handle is `{+0 link, +4, +8 resource id (0 = none), +0xc Resource*}` (`ResRefLink__set` 0x40cac0 links it into the resource's ref list at `res+0x78`). Resource data is obtained by “make resident (`res->vf5(1.0)` if state `res+0x7c` ≠ 1) then `res->vf3(1.0)`” (the `Resource__getTyped(mask,…)` call in the pseudocode is that request; masks 0x80000000 = render instance, 0x80000001 = render-instance data, 0xa0000000/1 = type data). Resource type at `res+0x7c`: 0x12 res_viewport, 0xc render camera, 0xd light, 0xe render object, 0x14/0x15 other.
Errors use `vm_<class>_<method>: …` with script file/line as in note 06.
### GfxEngine
| native | impl |
|---|---|
| `forceRendering()` 0x4e5560 | 7 full frames (§2) |
| `flush()` 0x4dc840 | `PUSH 0; CALL 0x4e5560` = `forceRendering(0)`; then `FUN_005176a0(1)` on `DAT_005db008` (resource flush), then `0x422e40` |
| `renderOK()` 0x4dc900 | `return (u8)DAT_005fe0f8` — the result of the last `Game__renderFrame()` (§2), false when that frame was skipped (disasm) |
| `changeVideoMode(w,h,depth)` 0x4e5480 | `C3DLow vf5 requestVideoMode` (fullscreen only; picks the format like init, sets `+0x1b5` → Reset at next beginFrame) |
| `numDisplayModes` 0x4e5770 / `currDisplayMode` 0x4e54f0 | tail `JMP`s (disasm): num → 0x4264e0 = `MOV EAX,[DAT_005eb454]; RET`; curr → `FUN_00424110`, which searches the sorted mode list `DAT_005eb450` (0x14-byte entries) for w,h,depth == `C3DLow+0x1bc/+0x1be/+0x1c0` and returns the index (0 if none) |
| `displayModeName(i)` 0x4e5510 | `"%d %d %d"` (w h depth; OptionsDialog.java splits it as width/height/depth, card 068) from the sorted mode list `DAT_005eb450` (0x14 per mode, built by vf3 for X8R8G8B8, X1R5G5B5, R5G6B5, qsort `0x427880`) |
| `printScreen(name)` 0x4e5780 | `Game__printScreen`: lock backbuffer copy (vf20: `GetRenderTargetData` into a SYSTEMMEM surface), RGB → 24-bit buffer, compress (`FUN_00527060`, quality `DAT_005da334`) and write; vf22 is a plain TGA writer |
| `setGlobalEnvmap(res)` 0x4dc910 | `game+0xa4` link := res (used by `Game__renderFrame` step 2) |
| `openVideo(file, toTexture, loop)` 0x4dc870/0x4dc8b0 | `Video__open` 0x44e9b0 (4-arg form first sets `Video__setLoop`) |
| `closeVideo` 0x4dc830 / `isPlayingVideo` 0x4dc860 | bare `JMP` tail thunks → `Video__close` 0x44e780 / `Video__isPlaying` 0x44e9a0 |
### Viewport (res_viewport, type 0x12)
`create(parent, depth, x, y, w, h)` 0x4e6af0 → `Render__createViewport` 0x525120: resource "_viewport" type 0x12, data `+0xc` depth/order, `+0x10..+0x1c` x,y,w,h (0..1), registered with the game (`FUN_00420970`); the resource is linked into the parent's type list by `FUN_0040cac0` (same convention as RenderRef). `activate(i)` 0x4e6ab0 → `FUN_004200d0(handle, −1, −1)`: type-checks the resource (`+0x7c == 0x12`) then `FUN_00421e40` (move to the drawn list `+0x14c`). `deactivate` 0x4e6c50 / `destroy` 0x4e6c90 → `FUN_00420b20`, dispatching on the resource type: 0x12 → `FUN_00421f50`, 0xc (camera) → 1, 0x15 → `FUN_00421da0`, 0x14 → 1; `destroy` then calls `FUN_004e9450`. `getLeft/Top/Width/Height` read data `+0x10/+0x14/+0x18/+0x1c`; `resize(x,y,w,h)` writes them; `getAspect` 0x4e6db0 = `(vpW·w)/(vpH·w)` (quirk: uses `+0x18` for both — so it returns backbuffer W/H); `unproject(Vector3 screen, cameraId)` 0x4e6fb0: finds the camera with `cam+0xa98 == id` in the viewport, `FUN_00461c20`, returns `(x·0.1, −y·0.1, z·0.1)`.
### Camera (typ_render_camera, type 0xc)
`create(parent, viewport, id, fov, near, far, f4, f5, i1, i2)` 0x4d8de0 → `Render__createCameraType` 0x524dc0 ("_rendertype_camera"): data `+0xc` = aspect (vp w/h), `+0x10` fov, `+0x14` near·10, `+0x18` far·10, `+0x34` f4, `+0x38` f5, `+0x30` i1; instance `"s_renderref_camera"` via `Render__createInstance`, `+0x2c` = i2, placed at bone00 with position (0,0,3.0); `Game__attachCameraToViewport`, `FUN_00420080(viewport, inst, id, −1)`. `destroy` 0x4d8fc0. `setFog(colour, start, end)` 0x4d9140: camera data `+0x24 = 1.0` (enable), `+0x28` colour, `+0x1c = start·10`, `+0x20 = end·10`.
### Text (ins_render_text)
`create(parent, font, x, y)` 0x4e5b30 → `Text__create` 0x4e8f40: instance "r_text" at (x, y, 0), instance data `+0x16c |= 0x8000` (text), `+0x17c` colour = default `DAT_005dadac`, `+0x170` string, `+0x174` align = 2, `+0x178` scale = 1.0; bounds kind 2. `setDefaultColor` 0x4e5cd0 → `DAT_005dadac`; `getDefaultColor` 0x4e58d0 = `MOV EAX,[DAT_005dadac]; RET` (disasm); `changeColor` 0x4e5c40 → `+0x17c`; `changeAlign` 0x4e5bb0 → `+0x174`; `setPos(x,y)` 0x4e5d00 → bone00 matrix translation; `getPos()` / `getPos(v)` → bone translation ×0.1; `setScale(sx,sy)` 0x4e5d80 → font type data via `FUN_004d8710`; `update()` 0x4e5eb0 → `+0x170` = the script field string (or ""). `getWidthPixels(str, font)` 0x4e5ae0 → `Game__textWidth` = vf131 setFont(font tex), vf134 set scale, `vf133 textWidth(str, 1.0)`.
### RenderRef
| native | impl |
|---|---|
| `create(parent, type, name)` 0x4e2140 | `Render__createInstance(parent, type, name)` 0x524840 (default name "_renderinst"): new instance resource (kind 3) under the parent's package, `+0x84 |= 0x200`, link type, optional extra bones `bone%02d`; bounds from the type (sphere radius from type data) ; instance kind `+0x11c` from the type's resource type: sound 6→6, camera 0xc→1, light 0xd→2, render object 0xe→3 (also registers it with the scene), 0x14→5 |
| `getBoneId(name)` 0x4e2290 | `FUN_0051b940(name)` (bone lookup in the instance) |
| `setMatrix(Vector3 pos, Ypr ori)` 0x4e2c80 | `RenderRef__setBoneMatrixYpr(bone00, 0, pos, ori)`: Ypr → matrix (`FUN_00569b40` from yaw/pitch/roll fields), translation ×10 inside (`FUN_00567570`), marks `+0x1c0 = frame` |
| `setMatrix(bone, parentRef, pos, ori)` 0x4e2a50 | same for an explicit bone |
| `changeResource(a, b)` 0x4e2090 | instance data `vf2(a, b)` |
| `setColor(argb)` 0x4e2830 | instance data `+0x17c` (used by colour-override buckets) |
| `setLight(diffuse, specular?, ambient?)` 0x4e29f0 | light data: `+0x10..+0x18` = c1 RGB/256, `+0x30..+0x38` = c2, `+0x20..+0x28` = c3 (note **/256**, not /255) |
| `setFlare(res, i, f, f, i, i)` 0x4e2950 | `RenderRef__setFlare`: data `+0xd4/+0xd8/+0xe0/+0xe4/+0x100` + flare texture link |
| `setType(res)` 0x4e2eb0 | `RenderRef__setType` → `FUN_0051e090` (relink type) |
| `getTypeID()` 0x4e23b0 | for instance kind 3: type data `+0x110` |
| `getDetail()` 0x4e3330 | instance `+0x9c` (LOD detail) for kinds 1/3 |
| `lineCreate(parent, type)` 0x4e25d0 / `lineAdd(p0, p1, colour, width)` 0x4e24f0 | line-strip instance (`RenderRef__lineCreate`/`lineAdd` → `FUN_00478690`) |
| `plotRoute(...)` 0x4e2640 | samples the route spline `DAT_005fe450` (`FUN_004133f0`/`0x414a80`) into lineAdd segments |
`Animation.<init>(RenderRef, ResourceRef)` 0x4d8a40: creates an animation player on the render instance (`FUN_0051b8d0`, speed/fade via `FUN_0051c4e0/560/540`).

## 10. Video playback (DirectShow, `CTextureRenderer`)
`Video__open(file, toTexture, loop)` 0x44e9b0: requires COM (`DAT_005ef0c8`); closes any running video; `CoCreateInstance(CLSID_FilterGraph)` → `DAT_005eee9c`; fullscreen-window mode (`toTexture == 0`): `IGraphBuilder::RenderFile`; texture mode: a custom `CTextureRenderer` filter (0x170 bytes, `FUN_0044f390`) added as "TEXTURERENDERER", source filter "SOURCE", its "Output" pin rendered; queries IMediaControl/IMediaEvent(Ex)/IMediaPosition/IVideoWindow/IBasicAudio (`DAT_005eeea4..b8`); `SetNotifyWindow(hwnd, 0x40d)`; window mode: owner = hwnd, `put_WindowStyle`-like call with 0x46000000; run (`FUN_0044edc0`). `Video__drawFrameQuad` draws the texture `DAT_005ef0e4` with the VB `DAT_005eee98` (4 XYZRHW|TEX1 vertices). `DAT_005da2b0` = toTexture (so `Video__isFullscreenPlaying` is true only for window-mode playback).

## 11. Open questions / next steps
Status after the pass documented above; “closed” means the behaviour needed for a reimplementation is observed, not that every byte was read.
- **Closed — cache-entry vf48/vf49**: not section ranges; they upload the vertex-blend matrix palette (base/count) and the section's palette index array/count; exact rule at §4.3, with the dirty bits and `FUN_00444180/4430` transform handling.
- **Closed — shader “state-id” table**: `FUN_00441d20` maps **shader-constant bank IDs 1..0x10**, not D3D render states; queue/commit/immediate writer at §4.3.
- **Closed — shadow pass**: projected ground-shadow quads plus the optional `rg_shadow` shadow map (`vf83/84/85`); no stencil volumes. Full fade formula, matrices, states, draw order and the dead `vf81/86..92` alternates at §3.4.
- **Closed — gamma**: ramp is `min(65535, (i/255)^gamma · 65535)`, `DAT_005da034 = 1/video_gamma` when configured, default 1.3 (§2.2).
- **Largely closed — materials**: descriptor layout, per-class vf0 pass counts and the main vf1 pass/TSS bodies in §6.2. Still **partial**: exact branch tuples/TSS enumeration for ColorSpheremap, ColorMixTex*, MixTA*Spheremap, TexBump*, Holo, MixConst, DecalPreview, TexMulVC, and the Water helpers `FUN_0042e220/42d860`; mode meanings (2/3 shader paths, 5..8 Tex variants) are inferred.
- **Largely closed — flares**: emission, record rebuild, batch states, flush/draw and chunking at §3.5. Still **partial/inferred**: the 0x1c-byte ghost layout inside `drawLensFlare` (`rec+0x58/+0x5c…` spacing/size/colour), and the exact occlusion-query handling.
- **Outline only — skidmarks/particles**: batch slots and begin/submit/end states are mapped, per-point widths/UVs/ageing and per-particle quad layout are not (§3.5).
- **Open — font atlas**: `vf76` (+0x130) is *not* the atlas loader; it is the dynamic-vertex-buffer chunk allocator (0x14-byte chunk headers wrapping 0x4c-byte section records). Atlas load plus glyph metrics behind `vf131 setFont` / `FUN_004d8710` are still unread; the drawText path itself is mapped (§3.3).
- **Open — `Scx__loadRenderV4`** 0x452960 (chunked v4 render format; collision side in note 11) and the exact v3 vertex layout per stride (`FUN_00455210` declaration 0x15003f, `FUN_00455450` writer); verify against `cars/racers/Axis_data/meshes/*.scx`.
- Not traced: which scene code registers casters/receivers into `rg_shadow` and the `DAT_005f06d8` ground-shadow caster list (**inferred**: shadow-casting scene objects).

## Appendix: C3DLow vtable (0x593bcc)
Names are from names.render.json where available (prefix `C3DLow__` dropped), otherwise the role is from this note. Only slots referenced in this note are listed; `slot = offset/4`.
| slot | offset | function | role |
|---|---|---|---|
| vf5 | +0x14 | requestVideoMode 0x422ec0 | fullscreen mode change (§9) |
| vf11/vf12 | +0x2c/+0x30 | beginSkyView / endSkyView | sky pass (§3.2 step 2) |
| vf13/vf14/vf15 | +0x34/+0x38/+0x3c | setBlendMode / resetBlendMode / setBlendFactor | blend state |
| vf19 | +0x4c | bindTexture | raw texture bind used by vf100 |
| vf33 | +0x84 | setWorldMatrix | world matrix |
| vf44/vf45/vf46 | +0xb0/+0xb4/+0xb8 | drawSectionRange / drawIndexed / drawSection | mesh submission (§4.3) |
| vf48/vf49 | +0xc0/+0xc4 | setPaletteBase / setPaletteIndices | vertex-blend palette (§4.3) |
| vf50/vf51/vf52 | +0xc8/+0xcc/+0xd0 | createMaterial / createMaterialFromBlob / createMaterialFromScx | material factory (§6) |
| vf54 | +0xd8 | initMaterialShaders | pre-created shader materials (§6) |
| vf56/vf57/vf58 | +0xe0/+0xe4/+0xe8 | beginMaterial / setMaterialPass / endMaterial | material draw wrapper (§4.1) |
| vf59/vf60 | +0xec/+0xf0 | createTexture / createTextureFromMemory | texture manager (§8) |
| vf64/vf65/vf66 | +0x100/+0x104/+0x108 | createCubeTexture / DDS format→slot / DDS block bytes | textures (§8) |
| vf67/vf68/vf69 | +0x10c/+0x110/+0x114 | setLighting / setLightTable / setActiveLights | lights (§5) |
| vf70/vf71/vf72 | +0x118/+0x11c/+0x120 | setAmbient / cpuLightVertices / setFog | lighting/fog (§5) |
| vf74/vf76/vf77 | +0x128/+0x130/+0x134 | setGlobalEnvmap / VB chunk allocator / createRenderTexture | misc |
| vf80/vf82 | +0x140/+0x148 | ground-shadow projected-texture setup (0x44bb40 / 0x44b9b0) | §3.4 |
| vf83/vf84/vf85 | +0x14c/+0x150/+0x154 | shadow-map begin / end / draw caster (0x44a850 / 0x44b0d0 / 0x44b2f0) | §3.4 |
| vf93 | +0x174 | caps bit test `(+0x7c >> 10) & 1` | projected lights gate |
| vf100 | +0x190 | drawColorQuad 0x4289b0 | overlay quads (§3.3) |
| vf102/vf103 | +0x198/+0x19c | beginOffscreen / endOffscreen 0x427d30 / 0x428970 | flare RT (§3.2 step 11) |
| vf104..vf109 | +0x1a0..+0x1b4 | 0x446b00/0x4480f0/0x446e90/0x447170/0x4476f0/0x446d60 | quad batches (§3.5) |
| vf110..vf112 | +0x1b8..+0x1c0 | 0x448470/0x4488b0/0x4486e0 | skidmarks (§3.5) |
| vf113..vf116 | +0x1c4..+0x1d0 | 0x444e50/0x445600/0x445010/0x4451f0 | flares (§3.5) |
| vf129 | +0x204 | drawText 0x44c4a0 | text (§3.3) |
| vf131/vf133/vf134 | +0x20c/+0x214/+0x218 | setFont / textWidth / setTextScale | text (§3.3, §9) |
| vf138/vf139 | +0x228/+0x22c | drawLines3D / drawLine2D | debug lines (§3.2 step 13) |
| vf149 | +0x254 | reapplyBlendState | restores `+0x1c..+0x2c` |
