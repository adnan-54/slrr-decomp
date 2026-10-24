---
id: 129
status: implemented
branch: card/129
depends: [128]
---
# compiler stage 2: statement/expression parser (method bodies + field initializers -> AST)

## Goal
Stage 2 of notes/20-script-compiler.md. Parse every method body and field initializer token
range from stage 1 (src/script/compiler/decl.*) into an AST that stage 4 can serialize into
the TUFA TREE stream. Read FORMAT.md section "TREE" (bin/tools/slrr-script-decompiler/FORMAT.md,
from `## TREE` on) first: the AST node kinds should map 1:1 onto that stream (node-kind enum
0..46: operators, call, const, new, new[], instanceof, cast, ctor-chain, assignments;
statements: local decl, if/else, while/for/do, switch/case/default, break/continue, return,
this(...)/super(...), expression statements, blocks).

## Dialect notes (verify on the corpus)
- Java expression precedence; `instanceof`; casts `(Type)expr`; `new X(args)`, `new X[n]`,
  maybe array initializers `{...}` - check; ternary `?:`; compound assignments; `++/--` pre/post.
- Ints used as booleans (`if (x)`, `while (loop)`, `!obj`), `obj` null tests.
- Resource constants as primary expressions (lexer token Resource).
- Local declarations with multiple declarators and `int x[]`.
- Labels? try/catch/throw/synchronized? Check the corpus: support what appears, report counts.

## Changes
- new: `src/script/compiler/ast.h` (node types), `src/script/compiler/body.h/.cpp`
  (recursive-descent parser over a token range; keep each node's source line, the TREE
  stream needs per-instruction lines; also the line of a block's closing brace).
- `src/tools/classdump.cpp --parse-java`: also parse every body/initializer; extend the
  summary: `java parse: N files, M classes, K methods, B bodies, F failures` and list the
  first 20 failures (file:line: message). Count unsupported constructs separately if any
  are deliberately skipped (there should be none at the end).
- No codegen yet.

## Done when
- `classdump --parse-java "bin/the game"` parses all bodies of all shipped .java files with
  0 failures (make check already runs it). Report the summary line.
- `make -C src clean check` green. No unit tests / hand-made inputs.
