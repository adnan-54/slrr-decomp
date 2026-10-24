---
id: 130
status: todo
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

## How to derive the rules (data, not guesses)
Use the pairs: every `<dir>/src/Name.java` with a shipped `<dir>/Name.class` (2,742; each
source also ships as `<dir>/Name.java` - use the src/ copy). Dump shipped classes with
`src/build/classdump <file>` to see the CONS order, which constants exist (names,
descriptors, class refs, member refs, nameType, string/resource literals used by bodies),
flags per modifier, field/method ordering, localSlots, body indices. CONS also holds
constants only TREE uses (literals, member refs): collect them in the order the exe does
(find the rule from the pairs: likely first-use order during a walk of declarations and
bodies). The `stamp` to write: the value the shipped .class files of the 2.3.1 install carry
(check; 0x00024F9D or 0x00011AE1; notes/03 says the loader accepts both).
localSlots and treeBodyIdx need the body AST (count locals; init bodies/method bodies order).

## Changes
- new: `src/script/compiler/emit.h/.cpp` (class unit writer; TREE placeholder).
- `src/tools/classdump.cpp`: `--compile-check <dir>...`: for each pair, compile the src
  .java, compare unit count, header, CONS, FILD, MTHD, CLSS with the shipped .class; print
  `skeleton: P pairs, U units, identical: header H, CONS C, FILD F, MTHD M, CLSS S` and the
  first 10 mismatches with a one-line reason (section + first differing entry).
- Makefile `check`: run `--compile-check "bin/the game"` as a REPORT (do not fail the build
  on mismatches yet; fail only on a crash or unreadable file).

## Done when
- `make -C src clean check` green; report the skeleton summary line. Aim: all sections
  identical for most pairs; for the rest, the mismatch reasons grouped by kind in your report.
- No unit tests / hand-made expected values; all checks against shipped pairs.
