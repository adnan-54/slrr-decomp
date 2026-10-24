---
id: 128
status: merged
branch: card/128
depends: []
---
# compiler stage 1: lexer + declaration parser for the script Java dialect

## Goal
First stage of the script compiler (plan: notes/20-script-compiler.md). Read every shipped
script source and split it into declarations. No code generation yet.

## Dialect facts (from shipped sources; check them, don't assume more)
- Files: `package java.game;` (+ dotted), `import java.util.*;` / `import java.render.osd.*;`.
- One or more top-level classes per file (`public class X extends Y implements A, B`), also
  `abstract`, `final`, `static` modifiers; nested classes may exist - check the corpus.
- Fields with initializers, incl. multi-declarators `int a, b = 3;`, arrays `int[] x` and
  `int x[]`, `final static`.
- Methods/ctors with bodies; `native` methods end with `;`.
- Literals: ints (decimal, `0x..`, `0X..`), floats (`1.0`, `.5`, `1.0f`?), strings with
  escapes, chars?, AND resource constants: `name.part.part:0x0000ABCDr` (e.g.
  `cars.racers.Whisper:0x00000158r`, `frontend:0x9C05r`, `misc.dealer:0x0001r`) - one token.
- Comments `//` and `/* */`. Encoding: bytes (Latin-1/CP1250 comments), never fail on them.

## Changes
- new: `src/script/compiler/lexer.h/.cpp` (Token {kind, text, line}; kinds: ident, keyword,
  int, float, string, char, resource, op/punct, eof), `src/script/compiler/decl.h/.cpp`
  (parse a token stream into: package, imports, classes {modifiers, name, extends,
  implements, fields {modifiers, type, declarators {name, array dims, init token range}},
  methods {modifiers, return type (none for ctors), name, params {type, name}, throws?,
  body token range or native}}). Bodies are matched by braces only.
- `src/tools/classdump.cpp`: add `--parse-java <file|dir>...` that lexes+parses each `.java`
  (recursing dirs), prints failures (file:line: message) and a summary
  `java parse: N files, M classes, K methods, F failures`.
- `src/Makefile`: compile the new files into the engine library; add to `check`:
  `cd .. && src/$(BUILD)/classdump --parse-java "bin/the game"` (fails on any failure).

## Done when
- `classdump --parse-java "bin/the game"` parses all shipped `.java` files (expect ~3,400:
  2,742 with a .class + 621 java-only + others) with 0 failures; report the summary line.
- `make -C src clean check` green. No unit tests or hand-made inputs (CLAUDE.md).
