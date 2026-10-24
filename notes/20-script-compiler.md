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
