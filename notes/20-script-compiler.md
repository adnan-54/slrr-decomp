# Script compiler (.java -> TUFA .class) — plan

Why: 621 shipped scripts exist only as `<dir>/src/Name.java` (418 car parts under cars/racers,
135 wheels, 38 running gear, 28 engines, multibot track_data for every freeride map). The exe
compiles them at class load (JVM::compileSource 0x5598f0, notes/03). Without it, freeride
(`CareerEvent.init` -> `track_data` create) and most car parts cannot run.

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

Open (next cards): loops (while/do/for, 0x16), switch (0x17/0x18), break/continue, ternary,
array init, `this(...)`/`super(...)` details, resource-literal package casing (source
`cars.racers.baiern` -> shipped `Baiern.rpk`: resolved against the real package path),
uninitialized local decl shapes (`02` without `2c`), `this.x` receivers, string concatenation.
