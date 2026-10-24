---
id: 132
status: implemented
branch: card/132
depends: [131]
---
# compiler stage 4a: TREE codegen basics + per-body comparison

## Goal
Stage 4 of notes/20-script-compiler.md: serialize body ASTs (src/script/compiler/ast.h,
body.cpp) into the TUFA TREE stream, and the constants they use into CONS. Format:
FORMAT.md section "TREE" (bin/tools/slrr-script-decompiler/FORMAT.md): instr = op:u8 line:u16
payload; opcode table + node-kind enum there; our VM (src/script/vm.cpp Thread::step) is the
executable reference for what each op means (e.g. 0x02 declare local, 0x06 type token, 0x07
node, 0x08 statement end, 0x09/0x0a/0x0b/0x0d/0x0e literals, 0x10 field-init store, 0x1b/
0x1c member refs, 0x25/0x1a/0x12 invokes, 0x27 call header, 0x28/0x29/0x2a returns,
0x03/0x2b epilogue runs).

## Scope of this card (first families; more cards follow)
1. Infrastructure: TREE writer, CONS entries created while emitting (remember the CONS ORDER
   rule is still open: emit constants in first-use order of the walk and report how close
   that gets), per-instruction source lines, method prologue/epilogue (0x03 runs, 0x2b,
   0x29/0x2a at the end of void methods/ctors - derive from pairs).
2. Field-initializer bodies (end with 0x10) and these statements/expressions: literals
   (int/float/bool/null/string/resource), locals (decl 0x02 + type 0x06 + init 0x2d/0x2c),
   parameters, `this` fields and static fields (0x1b/0x1c/0x19), assignment and compound
   assignment, unary/binary operators incl. && || with 0x04/0x05 + 0x14, return value/void,
   expression statements. Calls/new/control flow may be emitted best-effort or skipped.
3. `--compile-check`: add per-body comparison: `bodies: total T, identical I` and grouped
   first-difference reasons (op at first differing instr) with counts; also `CONS exact`
   (same entries in same order).

## How to work
Take 3 small pairs first (bin/Axis_FL_door.java vs bin/Axis_FL_door.class; then 2 from
cars/racers/Axis_data/scripts), get their bodies identical, then run the corpus once.
`src/build/classdump --disasm <file.class>` prints per-body op/operand (add line output to it
if needed). Do not explore the whole corpus by hand.

## Done when
- `make -C src clean check` green (compile-check stays report-only).
- Report the summary incl. `bodies: total T, identical I`, `CONS exact N`, and the top 10
  first-difference groups.
