# Script compiler (.java -> TUFA .class) — plan

Why: 635 shipped scripts exist only as `<dir>/src/Name.java` (418 car parts under cars/racers,
135 wheels, 38 running gear, 28 engines, 16 package-root base classes; see "Compile on load").
The exe compiles them at class load (JVM::compileSource 0x5598f0, notes/03). Without it most
car parts cannot run.

Verification (real data only): 2,742 shipped `src/Name.java` + `Name.class` pairs. The target is
behaviour-equivalent output; byte equality with the shipped .class is the strongest check and
the first goal (the exe's compiler wrote those .class files). Format: FORMAT.md section
"SLRR TUFA" in bin/tools/slrr-script-decompiler (CONS/FILD/MTHD/CLSS/TREE, TREE = postfix AST
stream), our reader src/script/classfile.*, decompiler sources in the same tool dir.

Stages (one card each, src/script/compiler/):
1. 128 lexer + declaration parser (package/import/class/fields/method headers; bodies kept as
   token ranges). Corpus: every shipped .java parses.
2. statement/expression parser into an AST. Corpus: every body parses.
3. class skeleton writer: header, CONS order, FILD, MTHD, CLSS. Corpus: pairs equal except TREE.
4+. TREE codegen per node family (literals/locals/fields, calls, operators, control flow,
   new/arrays/casts, switch/try...). Corpus: count of byte-identical bodies, rising.
Then: hook into packages.cpp load_class_at where the .class is missing (0x5598f0 rules).

## Stage 3b results (card 131, `classdump --compile-check "bin/the game"`)

`skeleton: 2769 pairs, 2809 units, identical: header 2806, FILD 2809, MTHD 2809,
CLSS 2809, CONS-set 1213; old-stamp 152`

- TREE body indices follow source declaration order, field initializers and methods
  interleaved (Gamemode: instance fields declared before the statics get 0..10, the
  statics 11..25, fields declared later 26..30, then methods in source order).
- MTHD locals = max simultaneous local slots: `this` slot 0, parameters and block
  locals at declaration, freed at block exit. A method whose body has no statements
  gets 0 (Set.build, TrackData.setTrafficLight); a constructor does not, because the
  implicit ctor chain still needs its slots (TrackData.<init> = 1, Part.<init>(I) = 2).
  Each `switch` case body is its own slot scope (Gates.osdCommand redeclares the same
  names per case; shipped locals = the largest case).
- FILD flags: `abstract` = 0x10 (Track.posStart/oriStart are `abstract` fields).
- Header stamp: compiler writes 0x24F9D. 152 pairs carry the older compiler's 0x24F5C
  and are reported as `old-stamp` instead of mismatches. 3 paired files
  (parts/neons/scripts/neontube_{L_Aqua,L_Blue,R_Aqua}.class) carry 0x11AE1 and stay
  reported; INVENTED: which source-comparison in 0x5557c0 selects 0x11AE1 was not
  traced (the exe default path writes 0x24F9D).
- Remaining CONS-set differences are codegen order (stage 4), not skeleton.

## Stamp rule (Claude agent trace of 0x5557c0, partial)
0x55927f default 0x11ae1; 0x55929a ECX = [ebx+0x28] (a string of a node in a sentinel list built
near 0x55582f, walked around 0x556c40/0x55925a); inlined strcmp against "java.io", "java.lang",
"java.render", "java.sound", "java.util", "java.net" (0x5b5100..0x5b5138): any match -> 0x24f9d
(0x5593c9), none -> 0x11ae1; written at file offset 8 (0x5593dc). Probably the import list
(normal sources import java.util.*), but the node type is not decoded and the 3 neontube files
(+ system/scripts/util/Rank.class) are not explained by it: likely stale older output.

## Stage 4a results (card 132, `classdump --compile-check "bin/the game"`)

`bodies: total 7096, identical 6381; CONS exact 2542 of 2809 units` (bodies compared with
const operands resolved to their values, so CONS order does not hide body matches;
`--compile-check --show <path-substr> <dir>` prints both disassemblies + CONS side by side).

TREE rules derived from the pairs (emit.cpp):
- Float literals: lexer 0x5523f0 (token 0x14c) -> sscanf 0x5693c0 -> the exe's own scanf
  0x527140 -> 0x527530: int part as uint32 -> float, then per fraction digit
  `v += d * p; p *= 0.1f` in single precision (SSE). Implemented exactly. ~180 bodies in the
  shipped corpus disagree by 1 ulp (e.g. 0.04, 0.45 come out correctly rounded there); a
  `p = p / 10` variant matches more of them, so those classes were probably written by an
  older build of the function. We keep the exe's algorithm.
- Binary/unary operators: operands then `07(kind)`; as a statement root the `07` is replaced
  by `08(kind)` (assignments, `x++;`).
- Calls: `[args] 27(argc) 24 ...`; 27/24 carry the line of the closing `)`.
  Bound call `1a(memberref)` only when every argument type is known at compile time and
  matches: literals, locals/params, `this`, array elements; int literal fits `F`, null fits
  any reference, object types are not compared. Operator/call/`new`/field-access arguments
  make it a by-name call `12(name)` (canister_1 tHUF2USD(66+2), Axis_200S
  addElement(new Integer(39)), Axis_VT addColorIndex(GameLogic.X)).
  Implicit this by name: `24 11(1) 12`. Simple receiver chain: `24 11(n) [chain] 1a|12` where
  n = number of dotted segments incl. the method (drag_matrix.pos.toString() -> 3); the same
  `11(n)` prefixes field chains `11(n) [chain] 1b`. Computed receiver: `[recv] 25(name)`
  without 24.
- Static fields name their declaring class in the memberref (Bot: GameType.EVENT_COMMAND ->
  GameRef::EVENT_COMMAND); instance fields the receiver's static type. Array length:
  `[java.lang.String::length I`.
- if: `15` and the else-skip `14` carry the lexer line at reduction: with else, the last token
  of the else branch; without else, the lookahead token after the statement (passed outward
  through nested ifs).
- Statement end `08` uses the line of the statement's `;`.

CONS order (2542/2809 exact):
1. self class, super class, interfaces;
2. type strings, walking declarations in source order: field types; per method parameter
   types, return type, then the body's local-declaration and `new`/`instanceof` types;
3. names, same walk: field names; method signature then name;
4. a dry codegen walk adding member/class refs in postfix emission order (methods in MTHD
   order, static first, then field initializers in FILD order); a by-name call through a
   simple receiver also adds the receiver's class right after the receiver refs;
5. the real codegen walk adds the rest: string/resource literals and by-name names
   (12/25/26/1c).

Open after 4a (card 133 below): loops, switch, break/continue, ternary, array init,
resource-literal package casing, uninitialized locals, `this.x` receivers, string concat.

## Stage 4b results (card 133, `classdump --compile-check "bin/the game"`)

`bodies: total 7096, identical 6821; CONS exact 2681 of 2809 units` (4a: 6381 / 2542).

TREE rules derived from the pairs (emit.cpp), each with the pair it came from:
- while (InventoryItem_Folder.installToCar): `top: [cond] 15(end) body 14(top)`.
- for (Set.getMass, Catalog line 2566): `[init] top: [cond] 15(end) body upd: [update 08]
  14(top) end: 03(for-init locals)`; the init declaration lives in its own scope around the
  loop, its 03 run is at the loop's jump line after the back edge. Update statements end
  with `08` at the update expression's line.
- do-while (RaceSetup line 184): `top: body [cond] 16(top)`, 16 at the line of the closing
  `;`. 0x16 is only the do-while back edge; while/for go back with 0x14.
- break / continue: 03 run (descending) for the locals declared inside the target
  construct, then `14` at the statement's line; break -> loop end (after the back edge,
  before the for 03 run) or switch end; continue -> for update / while top / do-while
  condition, skipping enclosing switches (Catalog line 2569 `if(!whl) continue;`:
  `03 10, 14 -> update`).
- switch (PowerInTheDark, AmateurDriftCup): `[selector]`, per labelled case `[label]
  17(case body)`, then `18 14(default body or end)`, then the case bodies in source order.
  17/18/14 carry the closing `}` line. Each case is a local scope whose 03 run comes at the
  lookahead that ends it: the next label's line, the `}` for the last case (City
  PoliceDialog line 3645, City.handleMessage CMD_DEBUG line 3342).
- Jump lines: the 15/14 of an if or loop carry the line where the enclosing *block item*
  is reduced (code is generated then), not the nested statement's own end:
  Racer.calcPrestige `if (a) if (b) {..} else {..}` and Chassis.updatevariables
  `if (x) for (..) {..}` tag the inner jumps with the outer else-less if's lookahead line.
- Operator lines: a binary node (and the 04/05/14 of && ||, instanceof's 07/06) carries the
  lookahead line after its right operand (Duhen_D18V_block line 97, Duhen_146er line 45,
  Gamemode line 1150). `new`: 06 at the `(` after the type, 07(30)/27/21 at the closing
  `)` (Painter line 875). Return 28/29/2a + runs at the `;` line (Bot.getName 556..560).
  Computed-receiver by-name call 25 at the `(` line (Track line 2242); simple receivers
  keep the method name's line (CarMarket line 746).
- Ternary (EventList line 238): `cond then else 07(0)`, no jumps.
- Primitive cast: `07(33) 06(type string)` (Catalog line 2486 `(int)`); class casts keep 19.
- Call binding (0x1a vs by-name 0x12/0x25): array elements are never typed arguments
  (tyre_195_50_16_7_5_LOD_CATALOG_GARAGE setRenderType(render_types[i]), City
  alignToRoad(alignedPos[0])). Argument fits: int<->float both ways (AmateurDriftCup
  checkMass(int field) -> (F), Vehicle addStockParts(float..) -> (IFF)), int -> String
  (GameLogic dayTxt.changeText(day)), null -> any reference (DebugMenu createButton(..,
  null, null) picks the (..String;String) overload), subclass/implementor -> class, any
  reference or array -> Object (Chassis partLists.addElement(int[] field)); an Object
  local does not fit GameRef (Garage line 1151 goes by name). Overloads: the cheapest
  fitting signature wins, scan order (last declared first) on ties (City
  raceBot.createCar(map, 1) with a GroundRef field -> (GameRef;I)). INVENTED: the cost
  ranking itself, and "class not in the index fits any object parameter".
- CONS: `this` as a value adds the self type string in the ref walk (CareerGoalsDialog
  `osd.globalHandler = this;`); after the declaration types, a class that is not the first
  of its source file adds its own type string, and every class adds its interfaces' type
  strings (Transit.java Portal, Bot.java botCard, Navigator/CarMarket
  `Ljava.lang.Runnable;`). INVENTED: which compiler step adds these was not traced; the
  rule fits every pair.
- Unchanged: uninitialized locals (`02 06` without `2c`), `this.x` and string
  concatenation already matched; array initializers do not occur in the paired corpus.

Remaining body differences (275), none a codegen rule:
- 189 float literals 1 ulp off (kept: exe algorithm, see 4a).
- 68 resource literal package casing (`cars.racers.baiern` -> shipped `Baiern.rpk`; only
  the file name is re-cased, dirs stay as written): needs an exe trace of how the compiler
  resolves the literal's package (likely the open package registry's stored name).
- Stale shipped classes (compiled from older sources/classes): 7 transmissions with
  `type = TT_AUTO` compiled to the literal (the 34 other TT_ users and every other final
  static, e.g. Part.PACKAGE, load the field, so the sources were edited after compiling;
  not a constant-inlining rule); Center_Differential getBlock ()Part vs shipped Chassis
  ()Block; 4 edited string literals ("platsic"); 3 neontube line shifts; Garage
  httpRequestAsync missing from the shipped NetworkEngine.class; ROCTrack/City
  show(RaceDialog) binding show(LoadingDialog) although RaceDialog is not a LoadingDialog
  in the shipped classes (CONS only).

## Resource literal casing (traced 2026-10-24)

The stored package name of a resource constant is not derived from the source text: the
compiler resolves the literal to a global id through the resource name registry and writes
back the registry slot's name. Chain (JVM::compileHook 0x5557c0 calls 0x54b310 for every
method/field-init tree after codegen, with the parse-time pool as `old`):
- 0x54b310 (constant-pool rebuild walk): per 0x100e node, old CONS entry type 3
  {+8 local, +0xc name string idx} -> `gid = 0x55cad0(name, local)` (= 0x5189e0), then
  `0x54b170(gid)`. Every other constant is re-added the same way, so the source spelling
  (`cars\racers\baiern.rpk`) never reaches the written class.
- 0x5189e0(name, local): `idx = 0x518940(name)`; slot idx must load (0x51e310, else return 0);
  return `P.0x51e860(local)` = `idx << 16 | local` (local 0 or out of range -> 0).
- 0x518940(name): linear scan of the registry, match = `_stricmp(slot+0xc, name) == 0`
  (0x51e2f0 -> CRT 0x57c11e); no match -> append (0x51e2a0 copies `name` as given, 0x40
  chars). So a slot keeps the spelling of whoever registered the package first.
- 0x54b170(gid): `pkg = 0x517b80(gid)` (slot gid>>16), adds the string `pkg+0xc` (registry
  spelling) via 0x54b260, then a type-3 entry {local = gid & 0xffff, name = that string}
  (deduplicated by 0x54b560).
- Missing package: the slot is appended but does not load -> gid 0 -> slot 0 = `system.rpk`,
  local 0. TimeAttackTest `sl.Scripts.game.CareerEvents.old_stdpack_231:0x0009r/0x0005r`
  (no such file) are both shipped as `system.rpk#0`.

Who registered first when the shipped classes were compiled: GameLogic (GameLogic.java
345-360) calls `System.rpkScan(dir)` for `parts\engines\`, `parts\`, `cars\racers\`,
`cars\fake_racers\`, `maps\`, `sl\Scripts\game\CareerEvents\` and every `multibot\maps\<dir>\`.
rpkScan (script, System.class body 38) = `FindFile.first(dir + "*.rpk")` loop with
`openLib(dir + name)`; openLib 0x4e0470 registers that string with 0x518940 unchanged. FindFile
returns the on-disk file name, the dir is the script's spelling: hence "dirs as written, file
name re-cased". Corpus check (all 136 literal packages vs disk, case-insensitive):
- differing only in casing and under a scanned dir -> shipped uses disk spelling: cars\racers
  baiern/duhen/einvagen/enula/focer/mc/nonus/prime/superduty (the 68 bodies; `mc` -> `MC`,
  so not a first-letter rule);
- differing but not scanned -> shipped keeps the written spelling: `maps.ROC_Track.smallmap`
  (disk `ROC_track`), `cars.traffic.CivilVan_2/_3` (disk `Civilvan_2/_3`) — rules out a
  plain filesystem lookup;
- no literal id has hi bits set or local 0 (sub-package remap of 0x51e860 unused).
Card 134 ports this.

## Stage 4c results (card 134, `classdump --compile-check "bin/the game"`)

`bodies: total 7096, identical 6889; CONS exact 2749 of 2809 units` (4b: 6821 / 2681).

- emit.h `NameRegistry` + compile_unit's optional `reg`; the ResourceLit codegen matches the
  literal's package against the slots with strcasecmp, appends it on a miss, and a `loads`
  failure (Package::load 0x51e310) yields slot 0 `system.rpk` local 0
  (0x54b310 -> 0x5189e0 -> 0x518940 -> 0x54b170). Not applied in the dry walk.
- classdump --compile-check builds the base registry GameLogic's rpkScan created: the scan
  dirs as written + the on-disk `.rpk` names, then `multibot\maps\` subdirs sorted
  (INVENTED: FindFile order); `loads` = case-insensitive file exists under the game dir;
  each unit gets a fresh copy (INVENTED: cross-file compile order unknown).
- All 68 casing bodies now match (Baiern/Duhen/Einvagen/Enula/Focer/MC/Nonus/Prime/
  SuperDuty, TimeAttackTest's missing `old_stdpack_231` -> `system.rpk#0`). The remaining
  207 body differences are the known stale/float set: 189 float 1-ulp, 5 getBlock returns
  (Center_Differential `()Part` vs `()Block`), 5 transmissions with `TT_AUTO`/`TT_MANUAL*`
  inlined, 4 edited strings, 3 neontube line shifts, 1 Garage httpRequestAsync call.

## Compile on load (traced 2026-10-24, Ghidra)

Callers: record `script <dir>\Name.class` 0x51fcc0 -> 0x55aca0 (dir\Name -> dotted, scans the
loaded-class hash; absent -> 0x55bec0); getClass 0x55add0 -> 0x55c0c0 findDir -> 0x55bec0.
JavaMachine::loadClass 0x55bec0 always calls JVM::compileSource 0x5598f0(dir, Name) first
(return value ignored), then reads `<dir>\Name.class` (File__readAll; < 1 byte -> log
"JavaMachine::loadClass: failed to load <path>", return 0), JVM::addClass 0x554ee0, package
register 0x55f780.

0x5598f0 compileSource(dir, Name):
- src = `dir + "\src\" + Name + ".java"`, cls = `dir + "\" + Name + ".class"` (0x100-char
  buffers). Existence 0x565200 (game file system: `/`->`\`, packed-file lookup 0x527bf0 when
  DAT_007a5b50, else disk 0x545490).
- recompile = cls missing, OR (src exists AND time(cls) != time(src)), OR header bad. Time
  0x564ef0 -> 0x5453b0: GetFileTime last-write (UTC) -> FileTimeToDosDateTime ->
  `(dostime >> 1) | (dosdate << 15)` (lossy, overlapping bits; missing file -> 0). Header:
  first 12 bytes; bad = `(magic != "TUFA" || ver != 4 || build != 0x11ae1) && build != 0x24f9d`
  (so build word 0x24f9d alone is accepted whatever the magic/version; old stamp 0x24f5c is bad).
- src missing: not needed -> return 1; needed -> log `JVM::compileSource: recompilation needed
  of "<src>" but source not exists` (0x5695c0, log only) and return 1: the old .class (if any)
  is still read by 0x55bec0.
- not needed -> return 1 (no work).
- compile: delete cls first (0x564fa0: close cached handle 0x565980, DeleteFileA 0x545420);
  open src (0x565760; fail -> log `JVM::compileSource: file not found: "<src>"`, return 0);
  this+0x70 line = 0, this+0x74 = src path, this+0x48 = dir, new ConstantPool 0x54ac80 ->
  DAT_007a25c0; DAT_007a2518 (parse root) = 0; yyparse 0x54db20; close. Root set ->
  JVM::compileHook 0x5557c0.
- after: if cls still does not exist -> `JVM::compileSource: Syntax error in "%s" at line %d`
  -> 0x569570(-1) -> 0x544e80: MessageBox + `_exit(-1)` (the game quits). Else the cls time is
  set to the src time (0x5658d0 -> 0x545cb0 SetFileTime), so the next start does not recompile.
- Result IS written to disk, beside the source dir: compileHook 0x5557c0 opens
  `dir + "\" + <first class's simple name> + ".class"` for writing (0x5657b0, at the first class
  of the unit), writes header (stamp rule above) + CONS/FILD/MTHD/CLSS/TREE per class via
  0x565950, closes at 0x559723. If the first class name differs from the file name, 0x55bec0
  then fails to read `<dir>\Name.class` (log only).
- Semantic errors inside compileHook ("cannot find parent class", "no such class",
  "field name reused", "missing 'return (expr)'", "local variable redefinition", ...) are
  logged via 0x5695c0 and compilation continues: the .class is still written.
- What it compiles against: no separate index. compileHook registers the unit's package with
  dir (0x55f780, "java.unnamed" when none), names resolve via getFQN 0x55b1d0 (imports +
  0x55c7a0 package tree) and members via 0x55aa20 -> 0x55b130 (loaded classes) else
  JVM::getClass 0x55add0, which loads (and recursively compiles) the referenced class. Resource
  literals go through the live global name registry 0x518940 (see "Resource literal casing").

Shipped data (bin/the game, case-insensitive pairing `<dir>\src\X.java` <-> `<dir>\X.class`):
- 635 java-only sources: 418 cars/racers part scripts, parts/wheels 135, parts/running_gear 38,
  parts/engines 28, parts/scripts 15 (package-root base classes: enginepart Driveshaft,
  IgnitionModule, bodypart RollCage, rgearpart.suspension FL/FR/RL/RRMcPhersonStrut,
  FLDoubleWishbone, DrumBrake, Block_Vee_OHV, OHV/SOHC Camshaft/CylinderHead,
  Transaxle_Differential), parts/accessories 1 (NOS_canister_48pds). Correction to the intro:
  all 60 multibot `maps/*/data/track_data.class` exist, so freeride does not need the compiler.
  Source dir spelling varies (`src`, `SRC`, `Src`).
- 2819 pairs: 166 headers fail the check (152 old stamp 0x24f5c + others) and in this copy
  1126 pairs have differing DOS times (files copied 2026-07-05): the exe would recompile and
  overwrite all of those here.
- src today: packages.cpp load_class_at prints "SERVICE MISSING (no compiler)" and fails.
  Card 135 hooks the compiler for the missing-.class case, in memory.

## Compile on load results (card 135, vmboot --selfcheck)

Hook: packages.cpp load_class_at, when `<dir>\Name.class` is missing, parses
`<dir>\src\Name.java` (case-insensitive SRC/Src; list_names 0x55fd90 resolves the src dir the
same way), registers the unit's package with dir (0x5557c0), builds a ClassIndex over the loaded
classes and sets ClassIndex::load -> vm.get_class (0x55aa20 -> 0x55add0) for misses, compiles,
and adds the TUFA bytes through ClassPath::add_data. INVENTED: the exe writes
`<dir>\<first class>.class` + copies the source time (0x5557c0/0x5658d0); src keeps the bytes in
memory, so a run never writes into the game dir. INVENTED: parse errors fail the load (exe quits
the process, 0x569570) and compile_unit errors fail it (exe logs semantic errors and still
writes). shortcut: only the missing-.class case, not the stale-time/bad-header recompiles.

- package roots: 16 java-only sources, 16 resolve through get_class
  (e.g. java.game.parts.enginepart.Driveshaft,
  java.game.parts.rgearpart.suspension.FLMcPhersonStrut).
- Axis record scripts: 4 refs, 4 load through load_class_at
  (Axis_L_taillights_dark, Axis_R_taillights_dark, kit_200XT_body, kit_ZX360_body).
- whole game dir: 635 java-only sources, ok 635, failed 0 (report only).
- Nothing is written to `bin/the game` (all compiles stay in memory).
