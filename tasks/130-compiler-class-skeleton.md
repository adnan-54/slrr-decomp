---
id: 130
status: merged  # partial: see notes/20 numbers
branch:
depends: [129]
---
# compiler stage 3: class skeleton writer (header, CONS, FILD, MTHD, CLSS) vs shipped pairs

## Goal
Stage 3 of notes/20-script-compiler.md. From the stage-1/2 parse (src/script/compiler/decl.*,
body.*, ast.h) produce each class unit's TUFA header + CONS + FILD + MTHD + CLSS exactly as
the exe's compiler did, and measure that against the shipped pairs. TREE is stage 4: emit
an empty/placeholder TREE for now and exclude it from the comparison. Format: FORMAT.md
"SLRR TUFA" (bin/tools/slrr-script-decompiler/FORMAT.md) and our reader src/script/classfile.*
(reuse its structs; add a writer next to it if it fits there).

## Scope (narrowed after a first attempt ran out of turns)
- This card: header (magic, version, stamp = the value shipped 2.3.1 .class files carry),
  FILD, MTHD, CLSS, and a CONS that contains the right constants. Compare SEMANTICALLY:
  resolve every constant index to its value (string text / class name / member
  class+name+desc / resource a,b) on both sides, so CONS ORDER does not matter yet.
  FILD: per section, same fields in the same order with same flags, name, type, and
  init (-1 vs has-initializer; exact body index only if easy). MTHD: per section same
  methods/order/flags/name/sig, localSlots, treeBodyIdx (exact if easy, else just
  -1/0 vs present). CLSS: same fields. CONS: same SET of constants (report missing/extra).
- Do NOT explore the corpus broadly. Pick 3 small pairs first (e.g.
  `bin/the game/cars/racers/Axis_data/scripts/src/*.java` vs `../*.class`; also
  bin/Axis_FL_door.java + bin/Axis_FL_door.class) with `src/build/classdump <file>`, get
  them identical, then run the whole corpus once and report.
- Exact CONS order = next card.

## Changes
- new: `src/script/compiler/emit.h/.cpp` (unit writer; TREE written as zero bodies).
- `src/tools/classdump.cpp`: `--compile-check <dir>...` walking pairs (`<dir>/src/X.java` +
  `<dir>/X.class`), compile, compare as above; print
  `skeleton: P pairs, U units, identical: header H, FILD F, MTHD M, CLSS S, CONS-set C`
  plus the first 10 mismatches (file, section, first differing entry).
- Makefile `check`: run it as a REPORT only (no failure on mismatches).

## Done when
- `make -C src clean check` green; report the skeleton summary line. Aim: all sections
  identical for most pairs; for the rest, the mismatch reasons grouped by kind in your report.
- No unit tests / hand-made expected values; all checks against shipped pairs.
