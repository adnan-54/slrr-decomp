# Thread::run (0x562d70): instruction encoding and opcode dispatch

From the decompiled evaluator (`FUN_00562d70`, 1,231 lines; read lines 60–690 so far). Everything below is observed in the pseudo-code unless marked.

## In-memory instruction format
- `Thread` fields: `+0x10` = JavaMachine/VM ptr, `+0x20` = current Frame, `+0x24` = **instruction pointer**, `+0x2c` = state flags (`4` = running, `0x40` = terminated), `+0x24` points into a stream of **8-byte instructions**:
  `int16 op; int16 srcLine; int32 operand` (the loop sets `[vm+0x70] = srcLine` for error messages, and `ip += 8` to advance).
- Frame: `+0x18` locals table, `+0x28` operand stack (`T_Container`: `[0]=capacity, [1]=data, [2]=count`; "T_Container: underflow" on pop), `+0x2c` saved ip, `+0x30` this-instance (0 for static methods), `+0x34` class.
- Helpers: `FUN_00562a30` = **pop value**, `FUN_00562c40` = **push value**, `FUN_00562ac0` = pop operand-slot/variable, `FUN_0047e4f0` = allocator for `Value`, `FUN_0054b970` = constant-pool string lookup, `FUN_0054b660` = constant-pool class lookup. Values are ref-counted objects with vftable `Value` (`[0]=vftable, [4]=type descriptor string ("I", "[…", class), [8]=payload`); `(**(vtbl+4))(1)` is their release/delete.

## The opcode numbers are the same ones as in the `.class` file
The dispatcher's `op` equals the file opcode from the community FORMAT.md table, with a prefix for operand-carrying ops:

| runtime op | file op (FORMAT.md) | meaning in the evaluator |
|---|---|---|
| `0x1001` | `0x01` load local | push `frame.locals[operand]` |
| `0x1002` | `0x02` declare local | creates variable; optional `0x402e` name follows (ip += 8) |
| `0x1007` / `0x1008` | `0x07` AST node / `0x08` statement end | inner `switch(operand)` on **node kind 0…0x2e** = FORMAT.md enum (`0x23…0x2e` = assignment family `= *= /= %= += -= <<= >>= … |=`); kinds 0…0x13 binary/unary ops via pop-pop-eval; `0x17/0x19` and `0x18/0x1a` call `FUN_0055da70` / `FUN_0055f210` (++/-- family); `0x1c` const wrapper (skip), `0x1e` new object, `0x1f` new array (walks `[` in descriptor) |
| `0x1011` | `0x11` call marker | `Thread::evalName` (0x562280) resolves the target; error "name cannot be parsed" |
| `0x1014` | `0x14` jump | `ip += operand*8` |
| `0x1015` / `0x1016` | `0x15` branch-if-false / `0x16` branch-if-true (loop) | pop, `JVM::booleanConversion` (0x555460), `ip += operand*8` |
| `0x1017` | `0x17` switch case | `FUN_0055e970` compares; jump if equal |
| `0x101b` | `0x1b` member ref | field lookup via constant pool; flag `0x40000000` = static; error "(JT_FIELD_REF) instance field access from static method" |
| `0x101e` | `0x1e` array creation | multi-dim not supported ("Thread::run: multi-dimensional arrays not supported!") |
| `0x101f` | `0x1f` push N nulls | allocates `operand` null `Value`s |
| `0x1027` | `0x27` call header / arg count | creates int `Value` holding operand |
| `0x401c` | `0x1c` member of explicit object | pop object, `FUN_0054ca70` finds field, errors "null.%s", "unknown fieldname: %s.%s" |
| `0x4025` | `0x25` invoke on this | `FUN_0054c550(this, name)` |
| `0x4026` | `0x26` invoke on super | `Thread::callMethod` (0x561700) on class `+0x1c8` |
| `0x04`, `0x05` | `&&`, `||` truth test | `FUN_0055ed90` truth test, skip 2 instrs on result |
| `0x10` | store to field-init slot / end | sets state `0x40`, restores frame ip, **returns elapsed time** (`float10`) — this is thread termination |
| `0x18` | switch default | pops/pushes through |
| `0x1d` | super field access | "unknown fieldname: super.%s" |
| `0x20` | array index | "(JT_ARRAYACCESS)" errors: negative index, non-array, bad index type |
| `0x21`, `0x22`, `0x23` | finalize `new` / `this(...)` / `super(...)` | `FUN_0054c550(.., "<init>")`, `FUN_0054c530`, `FUN_0054c570` |
| `0x24` | non-ctor call | `Thread::evalName` variant (0x562540), "script error" if unresolved |
| `0x00,0x13,0x42,0x48,0x4a,0x4d` | invalid | "fatal script error" |

Run loop returns a `float10` — the time (ms) the thread ran; `Thread::run(float timeslice)` takes a time budget (`local_64 = now + timeslice`), i.e. scripts are **cooperative threads with time slices** (the main loop's `FUN_0047e050`/`FUN_00563xxx` presumably call it).

## Why this matters
- The in-memory instruction stream is the `.class` TREE body with 8-byte slots (op:u16, line:u16, operand:u32); the loader expands the file's variable-size instructions to this. This fixes the VM design for our rebuild: parse `.class`, expand to 8-byte tokens, and run a loop identical to the above.
- The FORMAT.md opcode tables are therefore trustworthy for the interpreter and can be used 1:1 as the specification, with this note giving the runtime semantics.

## Node-kind switch (op `0x1007`/`0x1008`, operand = kind) — complete (lines 777–1229)
Binary operators pop `b` then `a` (`FUN_00562a30` twice) and call the operator function with `(a, b)`; result pushed back by the callee:

| kind | FORMAT.md | function | | kind | FORMAT.md | function |
|---|---|---|---|---|---|---|
| 0 | `?:` | inline (pop 3, `booleanConversion`, push chosen) | | 0x11 | `%` | 0x55ee10 |
| 1 | `\|\|` | 0x55ec80 | | 0x12 | `/` | 0x55e700 |
| 2 | `&&` | 0x55ea90 | | 0x13 | `*` | 0x55efc0 |
| 3 | `\|` | 0x55e540 | | 0x14 | `~` | 0x55e460 (unary) |
| 4 | `^` | 0x55e620 | | 0x15 | `!` | 0x55eba0 (unary) |
| 5 | `&` | 0x55e380 | | 0x16 | unary `-` | 0x55f110 (unary) |
| 6 | `!=` | 0x55f0d0 | | 0x17 | `x++` | 0x55da70 (+ copy old value 0x564a00) |
| 7 | `==` | 0x55e970 | | 0x18 | `x--` | 0x55f210 (+ copy) |
| 8 | `>=` | 0x55e9b0 | | 0x19 | `++x` | 0x55da70 |
| 9 | `<=` | 0x55ea50 | | 0x1a | `--x` | 0x55f210 |
| 0xa | `>` | 0x55ea10 | | 0x1c | const | literal types `0x100b` int, `0x100d` bool, `0x100e` string, `0x200a` float, `0x4009` resource, `0xc` null; else "unhandled literal type" |
| 0xb | `<` | 0x55edb0 | | 0x1e | `new` | `JVM::getClass` + `Instance` creation |
| **0xc** | `>>>` | **not handled → "script error"** (unsigned shift is unsupported at runtime) | | 0x1f | `new[]` | single dimension only |
| 0xd | `>>` | 0x55df40 | | 0x20 | `instanceof` | 0x549d60 |
| 0xe | `<<` | 0x55de60 | | 0x21 | cast | 0x55d1b0 (primitive) / 0x55d190 (class); "cannot cast %s" |
| 0xf | binary `-` | 0x55f310 | | 0x23–0x2e | `= *= /= %= += -= <<= >>= (>>>=) &= ^= \|=` | 0x55e160 `assign(target, value, kind)` |
| 0x10 | `+` | 0x55db70 (also string concat) | | | | |

The operator functions are exactly the ones whose error strings are "non-integral operands (&)", "unsupported type (+)", "division by zero" … (addresses 0x55db70–0x55f310, string table 0x5b5494–0x5b57d0). So **`Thread::run` + this operator family is the complete expression semantics** of the language, and each kind matches the FORMAT.md enum exactly (the only gap: kind 12 / `>>>`, which the runtime rejects — consistent with FORMAT.md's "unobserved").

Other runtime facts: the time-slice check happens every 256 instructions (`local_38 & 0xff`), returning when the budget is exhausted; calls to `wait`/`notify` use `Thread::wait`/`notify` (0x564950/0x5629a0).

## To do
- Name helpers: `FUN_00562a30` pop, `FUN_00562c40` push, `FUN_00562ac0` pop-variable, `FUN_0054b970` const string, `FUN_0054b660` const class, `FUN_0054ca70` find instance field, `FUN_00549750` find static field, `FUN_0055e160` assign, `FUN_00559d20` script error, `FUN_00564a00` copy value, `FUN_0047e4f0` Value allocator.
- Decompile the operator functions (0x55db70…0x55f310) for exact numeric promotion rules (int/float/String/boolean).
- `Thread::evalName` (0x562280/0x562540) and `callMethod` (0x561700/0x561830): method resolution and native dispatch (`java.lang.Native`, `vm_*` table at `FUN_004d5fb0`).

## Value model and binary-operator semantics (op_add 0x55db70, JVM::numericPromotion 0x55c3c0 decompiled)
- **Value object** (vftable `Value`, 0x59a438): `+0` vftable, `+4` pointer to the **type-descriptor string** (`"I"` int, `"F"` float, `"L…;"` class/String, `"[…"` array), `+8` payload (int32 / float32 / object pointer). Vf slot 0 returns a "kind" (2 = ref-to-variable) used by `numericPromotion` to unwrap variables.
  Runtime typing is by the first character of the descriptor, only `I`, `F`, `L` are valid for arithmetic. Booleans are ints.
- `op(a, b)`: both args are `Value*`; **the left value is modified in place and is the result** (`a.payload = a.payload ⊕ b.payload`), then the evaluator pushes it.
- `op_add`: `JVM::stringConversion(a,b)` (if either side is a String, the other becomes a String), then `JVM::numericPromotion(a,b)`; then by `a`'s type: `F` → float add, `I` → int add, `L` → must be exactly `Ljava.lang.String;` else "unsupported class type (+)" → concatenation: reads the `char*` payload of each String (a String instance's first field, via `Instance__findField`), allocates `len1+len2+1`, concatenates, `JVM__newString(ptr)` (0x55b980) and stores into `a`. null strings are treated as empty ("null" is **not** printed). Anything else: "unsupported type (+)".
- `numericPromotion(a,b)`: if either is `F` and the other `I`, the int one is converted with `Value__convertTo` (0x55d1b0) → both float; int/int stays int; `L` with non-`L` (or int/float with other types) → error "cannot promote X to Y". There is no long/double/short/byte: the language has **int, float, boolean(int), String, objects, arrays** only.
- Errors go through the common formatter: message + ` at file "<script file>" at line <n>` using `[vm+0x74]` (file) and `[vm+0x70]` (line) — set at each instruction fetch.
