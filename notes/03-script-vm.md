# Script VM ("JVM") in the exe — first map

Found through RTTI (`notes/classes.md`) plus static string xrefs (`tools/strxref.py` → `analysis/func_strings.json`).
Address range of the VM code: roughly **0x548c40–0x565000** (~360 functions). Names below are applied in `analysis/names.json`
(`Class__method`, `_<addr>` suffix when several functions share a name). All "observed" unless marked.

## Architecture
- The game ships both `.java` and `.class`: `JVM::compileSource` (0x5598f0) compiles `.java` at runtime if the class is missing/stale
  ("recompilation needed of … but source not exists", "CompileAll: begin file scan…", `\*.class`, `\src\*.java` globs in `Package`).
  The front end is a **flex lexer + parser** (strings "yy_create_buffer", "Unrecognized character", "NUMERIC CONSTANT BODY INVALID",
  "lexer: octal values not supported"). The parser output is a tree (`JTree`, `Tree`, `ATree`), which `JVM::compileHook` (0x5557c0) post-processes
  (type checks: numeric promotion, assignment/boolean/string conversion, "missing 'return (expr)'", "illegal 'break'", "local variable redefinition").
  This is why `.class` is a serialised AST (postfix), as the community decompiler's FORMAT.md describes.
- Classes loaded by `JavaMachine::loadClass` (0x55bec0); `JVM::getClass` (0x55add0), `getArrayClass` (0x55a690), `getFQN` (0x55b1d0), `addClass` (0x554ee0).
- Package roots (strings 0x5b44dc…): `java.lang`→`system\scripts\lang`, `java.net`→`…\net`, `java.io`→`…\io`, `java.util`→`…\util`,
  `java.render`→`…\render`, `java.sound`→`…\sound`, `java.game`→`sl\scripts\game`, `java.game.parts`→`parts\scripts`,
  `java.game.parts.accessories`→`parts\accessories\scripts`. "Package conflict: multiple packages placed at the same location" is checked.
- Class model: `Class` (0x549770 ctor, `isInheritedFrom` 0x549d80, `getMethod` 0x54a150), `Field_Info`, `Method_Info`, `Instance`, `Value`, `Variable`,
  `ConstantPool` + typed nodes (`getClass` 0x54b660, `getNameType_Name/Type` 0x54b820/0x54b890, `ioRead` 0x54ba70), `Signature::buildCache` 0x5604f0, `StringBuffer`.
- Execution: `Thread` objects. **`Thread::run` = 0x562d70** is a ~1,230-line decompiled interpreter (81 KB of pseudo-code, one function). It is a tree
  walker: a `switch` over node types `0x1002…0x1027` (the `JT_*` tags; e.g. `JT_FIELD_REF`, `JT_ARRAYACCESS` appear in its error strings) with an inner
  `switch` on operator kinds `0…0x4d`. Helpers: `Thread::callMethod` (0x561700, 0x561830), `Thread::evalName` (0x562280, 0x562540), `wait` 0x564950, `notify` 0x5629a0,
  dtor 0x5615d0. `Instance::notify` 0x54d290, `Instance::getArrayElement` 0x54c880, `Instance::initialize` 0x54cab0.
- Errors: reporter `FUN_00559d20` formats `!Script error\n\nThread: %s\nError: ` + `%s\n%s:%s (line:%d)`; natives missing → "native implementation missing for "%s.%s"" (native class `java.lang.Native`).
- `JTree::ioWritePure` (0x55d830, "not a runtime opcode"): serialisation of the tree, i.e. the `.class` writer — a reference for the TUFA TREE chunk layout.

## Cross-check with the community decompiler (`bin/tools/slrr-script-decompiler`)
- FORMAT.md's SLRR TREE: `instr := op:u8 srcLine:u16 payload` with node-kind enum 0 `?:` … 46 `|=` (operator kinds). The interpreter's inner switch has cases
  0…0x4d, covering the same range → the enum can be bound to the C++ `switch` case by case (next task).
- Built and ran the decompiler on this machine: .NET 10 SDK installed to `/opt/dotnet` (≈0.6 GB), built from a copy of the sources with
  `dotnet build -c Release -r linux-x64 --self-contained false`. On `cars/racers/Baiern_data/scripts`: **111 files parse OK, 0 failed; mean recall 74.5 % / precision 74.4 %** against the shipped `.java`.
  (The repo's csproj defaults to a win-x64 self-contained build, which is why a plain build did not run.)

## Next steps
1. Walk `Thread::run` in slices; map each `case 0x10xx` to a `JT_*` name and each operator case to the FORMAT.md enum; write the result as a table.
2. Locate the lexer/parser entry points (`yylex`, grammar actions) — the parser is generated code (flex/bison-like) and can be treated as a black box: rebuild with flex/bison from the grammar recovered from `.java` samples, or hand-write a parser for the Java subset.
3. Use `.java`/`.class` pairs (2,819) as a conformance suite: our compiler must produce a `.class` the exe accepts, and our interpreter must give the same results.

## Class load path and compile fallback (verified 2026-10-23)
- Record loader 0x51fcc0 (rsd text of a type record): `script <dir>\Name.class` -> 0x55aca0(dir, Name):
  look the class up by dotted name in the VM's class table; if absent -> 0x55bec0(dir, Name).
  `script <FQN>` (no `.class`) -> 0x55add0(FQN). Empty -> "Missing class: %s".
- 0x55bec0 = `JVM::compileSource` 0x5598f0(dir, Name), then read `<dir>\Name.class` (File__readAll
  0x565610; < 1 byte -> "JavaMachine::loadClass: failed to load <path>"), parse 0x554ee0, package check
  0x55f780 ("Package conflict: multiple packages placed at the same location (").
- 0x5598f0: src = `<dir>\src\Name.java`, cls = `<dir>\Name.class`. Recompile when cls is missing, or src
  exists and the file times (0x564ef0) differ, or the 12-byte header is not `"TUFA", 4, 0x11ae1` (build
  0x24f9d in the third word is also accepted). Needed but no source -> "JVM::compileSource: recompilation
  needed of \"...\" but source not exists" and the old .class is used. After compiling it sets the
  .class time to the source time (0x5658d0). Syntax error -> fatal message box (0x569570).
- Other compiler entry: System.compileAll 0x555510 (GameLogic, DEBUG_MODE only).
- src: no compiler (SERVICE MISSING). Shipped data: 4 Axis part scripts exist only as scripts/src/*.java.
