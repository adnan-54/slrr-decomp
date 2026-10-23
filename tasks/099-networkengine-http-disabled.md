---
id: 099
status: todo
branch:
depends: []
---
# java.net.NetworkEngine: httpRequest / httpRequestAsync (disabled in the exe)

## Goal
Replace the two explicit registry stubs (currently `nullptr, true`) with the exe
behaviour. Both natives are registered to the same 3-byte function that ignores its
arguments and returns 0, so port them as "null reference" and "int 0". No networking
service exists in the exe and none must be added.

## Exe evidence (cleaned pseudocode, renamed fields)
### 0x461910 — shared by both natives (also `System.runFinalization`)
Raw bytes at 0x461910: `33 c0 c3` = `xor eax,eax; ret`; the function is 3 bytes,
followed by `cc` padding up to 0x461920. Ghidra:
```c
undefined4 FUN_00461910(void)
{
  return 0;
}
```
No callees. `analysis/natives.tsv` rows 417/418 and `notes/06-native-methods.md`
("Several registrations share one stub ... NetworkEngine.httpRequest[Async] →
0x461910 (return 0, i.e. disabled)") confirm this address is the implementation for
both NetworkEngine natives.

Dispatch (notes/06 §Dispatch): the result kind is chosen by the *return descriptor*,
and `I`/`L`/`[` all take EAX; `L` means a pointer, so 0 → null reference. Arguments
are popped/converted by the dispatcher but the function body never reads them; there
is no validation or error path.

Consequently:
- `httpRequest(S,S,S,S)Ljava.lang.String;` always returns **null**.
- `httpRequestAsync(S,S,S,S,GameRef,S)I` always returns **0**.

## Changes (src/script/natives.cpp only)
1. Add after `zero_int` (line ~469), next to the System helpers:
   ```cpp
   // ---- java.net.NetworkEngine ----------------------------------------------
   // 0x461910: xor eax,eax; ret. Shared with System.runFinalization; the exe's
   // networking is disabled, so httpRequest's L return becomes a null reference.
   bool network_http_request(Vm &, const Args &, Value *out, std::string *)
   {
       *out = Value::make_null();
       return true;
   }
   ```
   (Reusing `noop_void` would also write null, but the dedicated name keeps the
   non-`V` return descriptor visible.)
2. Registry rows (currently lines 7289-7290) — replace `nullptr, true`:
   - `httpRequest` → `network_http_request, false`
   - `httpRequestAsync` → `zero_int, false` (same helper as `System.runFinalization`,
     whose comment already cites 0x461910)
   Do not change the class/method/descriptor strings.

## Files
- edit: src/script/natives.cpp (one helper, two registry rows)
- read only: src/script/natives.h, notes/06-native-methods.md

## Done when
- each ported function has a `// 0xADDR Name` comment; untraced logic is `INVENTED:`
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
