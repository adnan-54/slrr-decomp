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
