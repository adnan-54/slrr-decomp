# Tooling review and strategy (what we got in `bin/the game/` and `bin/tools/`)

Both folders are under the ignored `bin/`, so none of this is committed. Everything here was read, not executed
(this box has python3 and java; **no dotnet, mono or wine**).

## 1. What is available

| Item | What it is | Value for the exe reconstruction |
|---|---|---|
| `the game/` (31,206 files, 2.8 GB) | Full 2.3.1 install: 157 `.rpk`, 118 `.rdb`, 7,452 `.scx`, 3,194 `.cfg`, 2,921 `.class`, 6,908 `.java` | Complete runtime data set = test corpus for every exe subsystem |
| `.java` + `.class` pairs | **2,819 matched source/bytecode pairs** (102 `.class` have no `.java`). Stamps: 2,744 × `0x00024F9D`, 159 × `0x00024F5C` (not in FORMAT.md), 4 × `0x00011AE1`, 14 empty files | Ground truth for the **script VM**: source + compiled form + known behaviour |
| `tools/SlrrLib` (C#, 179 files, BSD-3) | Reader/writer for RPK (direct + dynamic), SCX v3/v4, CFG, class (crude), TRC, SPL, VRML export | Field-level format spec for resource files; our RPK findings match it (see §3) |
| `tools/slrr-script-decompiler` (C#, own git repo) | `.class` → `.java` decompiler for SLRR (TUFA/postfix AST), CRC 2005 (FLZD + stack bytecode), Monster Garage and SL1 dialects. `FORMAT.md` documents all of them | Complete spec of the TUFA bytecode = the instruction set of the exe's `JavaMachine`. Reported accuracy: 382/382 CRC files parse, ~48% line recall on 52 CRC pairs; SLRR format "109/109 chunks parse exactly". Has a prebuilt `publish/SlrrDecompiler.exe` (win-x64 only) |
| `the game/_src/ModernSlrrPatch` | C++ source of an ASI plugin hooking this exe: `ModernSlrrPatch.cpp` (2,170 lines), `Sound.cpp` (2,566 lines), signature checker `verify_sigs.py`, engine declipper | Real hook sites with **byte signatures and struct offsets in this binary** |
| `modding_tools/Resource tools` | `resdecode.exe`, `resconvert.exe`, `resgui.exe` (+ readme) | RPK ⇄ RDB converter = oracle for our parser (Windows-only; need to run elsewhere) |
| `changelog.txt`, `readme*.txt` | Build 908… history of the Steam/ImageCode release | Build/feature context; the exe is build 940 |

Caution: `slrr-script-decompiler/` contains its own `CLAUDE.md` and `.claude/settings.json`. I read the CLAUDE.md as
documentation of that repo only; it is not applied to this project.

## 2. New facts about the exe that fall out of this material

1. **RTTI gives ~330 real C++ class names** (`.?AV…` strings 0x5e3a28–0x5e64b4, found with `search_strings`). Names cover:
   - Script VM: `JavaMachine`, `Thread`, `Frame`, `JTree`, `Tree`, `Block`, `Class`, `Variable`, `Serializable`, `Field_Info`, `Method_Info`, `NativeInfo`, `ConstantPool` + `ConstantPoolNode{String,Int,Rid,Float,Class,_NameType,_FieldRef}`, `CompileClassData`, `StringBuffer`, `vm_file_libentry`, `vm_file_resentry`, `Instance`, `Value`.
   - Resource engine: `ResourceEngine`, `ResourceRef`, `ResourceNode`, `ResourceInstance_{Game,Physics,Render}(_Data)`, `ResourceType_{Game,Physics_Body,Physics_Constraint,Render_Camera,Render_Light,Render_Microphone,Render_Object}(_Data)`, `Resource_{Animation,Mesh,Sound,SoundMixer,Texture,Viewport}(_Data)`.
   - Render: `rg_*` render groups (`rg_object`, `rg_ground`, `rg_transparent`, `rg_prelittransparent`, `rg_particle`, `rg_skidmark`, …), `RI_*`/`RT_*` render instances/templates, 30+ `CMaterial_*` classes, `CompatibleMesh/Texture(_DX/_PTX/_DDS)`, `Texture`, `CubeTexture`, `cLight`, `CameraNode`, `ViewPortNode`, particle system (`ParticleSystem`, `PA_*` actions, `PD_*` domains).
   - Game objects (the `typeof` codes): `_type_{car,part,player,BOT,object,building,button,camera,cursor,ground,ped,road,trigger,trlight,walk}`, `gametype`, `part_slot`, `slot_def`, `slot_bolt`, `stock_part`, `moving_slot`, `wing_inst`, `bucking`, `controller`, `constraint`, `c_collision`, `rigidbody_type`, `trigger_coll`.
   - Traffic/AI: `ct{Lane,Road,Cross,CrossEq,CrossLight,CarContainer}`, `cWalker`, `cWBoidObstacle`, `cWArea{Wall,Spline}`.
   - Containers: `TList`, `DoubleList(2)`, `DList<>`, `T_List<>`, `T_Tree<>`, `T_Stack<>`, `HashtableU`, `HashKey`.
   This is the skeleton of the source tree. Each RTTI type has a vftable; naming vftables and their methods names most of the 6,630 functions.
2. The VM is a **tree-walking interpreter** (`JTree`/`Tree`/`Block`/`Frame`), matching the decompiler docs: TUFA bodies are a postfix **AST** stream (opcode 0x07 "AST node kind" with an operator enum), not a stack machine.
3. **The ModernSlrrPatch signatures partly match our exe.** `0x46B5E0` (`55 8b ec 53 8b d9`) and `0x468490` (`55 8b ec 83 ec 2c 53 8b d9 c7 05 c4 07 5f 00`) match the Steam-build signatures in the patch, which the patch labels as the `rg_transparent` flush and its push helper (label taken from the patch's comments, not independently verified in the decompile); `0x468490` stores to the global `0x5f07c4`. The 2.2.1-build addresses (e.g. `0x50A5D0` light gather) do **not** match our 2.3.1 exe. So the patch is a source of ground truth only where its Steam-branch signatures validate; it already ships a `verify_sigs.py` for this.
4. Header of RPK confirmed by SlrrLib with field names: `BinaryRpkEntriesHeader` = {EntriesSize, EntriesCount, **Type1Count, NonType1Count**} — my "x1,x2" (1/32 in system, 0/340 in Axis) are exactly these (type-1 = `ins_game`-kind entries). `BinaryRpkHeader` calls our `count_b` "ExternalReferencesUnknownZero". `BinaryResEntry`: +8 TypeOfEntry (byte), +9 AdditionalType (byte), **+10 IsParentCompatible (float)** (= `.rdb isparentcompatible`, solves my +0x0a unknown), +14 RSD offset, +18 RSD length, +22 alias length (max 32), +23 alias.

## 3. How this improves the process

| Problem | Before | Now |
|---|---|---|
| Naming 6,630 stripped functions | Guess from callees/strings | Name vftables from RTTI, then methods; `vm_*`, `Missing .cfg`, ModernSlrrPatch hook names add ~100 more |
| Script VM semantics | Reverse the interpreter blind | Read FORMAT.md node/opcode tables, then match them to the `JTree` evaluator; use 2,819 java↔class pairs as a conformance suite |
| File formats | Decompile loaders | Field names and offsets from SlrrLib; verify with the exe loader and our `tools/parse_rpk.py` |
| Struct layouts | Offsets only | Cross-check with SlrrLib classes + `.rdb` `rprop` schemas (property index/type/name per resource type = member layout of `ResourceType_*_Data`) |
| Verifying a rewritten function | No oracle | Hook-and-compare via the ASI mechanism in `_src/ModernSlrrPatch` (run original vs reimplementation in-process) |

Concrete next steps, in order of payoff:
1. **RTTI → class map**: walk each `.?AV` TypeDescriptor → COL → vftable; create `notes/classes.md` and apply names in Ghidra (`set_addresses_names`). Cheap, mechanical, and unlocks everything below. (Need to check vftable layout in Ghidra first.)
2. **Script VM**: find `JavaMachine` and `JTree` evaluator from RTTI; map node-kind enum from FORMAT.md; write a Python TUFA parser (port of `Format/*.cs`) and run it over all 2,921 `.class` files as a validity test.
3. **Resource engine**: `ResourceEngine`/`ResourceType_*` from RTTI + `.rdb` `rprop` tables → struct definitions. Then `FUN_00518880`.
4. **Build a signature database** from the ModernSlrrPatch checks (offset ⇄ meaning) and keep it in `notes/`.
5. Port nothing yet; first get a runnable environment (see §5).

## 4. The end goal: "fully functional project of the game exe"

Honest scope: the exe is 1.6 MB of x86 code, 6,630 functions, built with VS2015+ (CFG, `/GS`), no PDB. "A project that
builds a functional exe" means ~300 classes of C++ re-created from decompiler output. That is realistic only as a
staged, verified process, and the definition of "functional" must be fixed first:

- **Level A – behaviour-equivalent rebuild.** Compiles with a modern compiler, loads the unmodified game data, plays.
  Verified by running scripts/resources through both exes (hook-and-compare) and by playing.
- **Level B – byte-matching.** Each function compiles to the same bytes (decomp.me / reccmp style). Needs the exact MSVC
  version and flags; much slower; only worth it if modding-by-patching the original is the goal.

Recommended approach, because the ASI hooking infrastructure already exists: **hybrid DLL replacement**. Build the new
project as a library; reimplement subsystem by subsystem (resource/RPK → script VM → render → physics → audio) and
swap them into the original exe through hooks, comparing outputs, until nothing of the original code is left.
Dependencies to preserve: d3d9, d3dx9_32, DirectInput 8, DirectSound (DSOAL-compatible), `steam_api.dll`, `libcall.dll`.

## 5. Environment gaps and decisions needed

- The C# tools can't run here (no dotnet). Options: install dotnet (then `dotnet build` works on Linux since SAC does not exist here), or port the TUFA parser to Python. Installing needs your OK (package manager/network).
- Compiling the target needs a Windows-compatible toolchain: MSVC (VS2015+) via wine/`msvc-wine`, or `clang-cl`/mingw. Level B requires MSVC.
- Running the game for dynamic validation needs wine + a display, or your Windows machine.
