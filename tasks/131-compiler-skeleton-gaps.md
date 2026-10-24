---
id: 131
status: merged
branch: card/131
depends: [130]
---
# compiler stage 3b: close the skeleton gaps (stamp, MTHD locals/body index, FILD init)

## Goal
`src/build/classdump --compile-check "bin/the game"` (card 130) reports
`skeleton: 2769 pairs, 2809 units, identical: header 2654, FILD 2788, MTHD 2727, CLSS 2809, CONS-set 1213`.
Fix the header/FILD/MTHD mismatches (CONS-set waits for TREE codegen). Grouped reasons:
155 header (stamp), 51 MTHD locals, 21 FILD init, 14 MTHD body.

## Exe evidence for the stamp (Claude, bytes of the compiler 0x5557c0)
```
0x55927c  c7 45 dc e1 1a 01 00   mov [ebp-0x24], 0x11ae1     ; default stamp
...
0x5593c0  1b c0 83 c8 01 85 c0   sbb eax,eax / or eax,1 / test eax,eax   ; strcmp-style result
0x5593c7  74 07                  jz  +7
0x5593c9  b8 9d 4f 02 00         mov eax, 0x24f9d            ; strings differ
0x5593ce  eb 03                  jmp +3
0x5593d0  8b 45 dc               mov eax, [ebp-0x24]         ; equal -> 0x11ae1
```
Shipped stamps: 2,744 x 0x24f9d, 159 x 0x24f5c (older compiler: not reproducible, count
them separately as "old stamp", not as mismatches), 4 x 0x11ae1. Write 0x24f9d;
`INVENTED:` which string comparison yields 0x11ae1 is not traced (report the 4 files).

## Changes
- `src/script/compiler/emit.cpp`: stamp as above; fix localSlots counting and treeBodyIdx /
  FILD init body numbering from the mismatching pairs (look at the first few of each group
  with `src/build/classdump <shipped .class>` vs the source; derive the rule from them).
- `src/tools/classdump.cpp --compile-check`: print `old-stamp N` in the summary and do not count
  those as header mismatches; group mismatch reasons with counts at the end of the report.

## Done when
- header/FILD/MTHD identical on all pairs except old-stamp and genuinely unexplained ones
  (list those with the reason). Report the new summary line.
- `make -C src clean check` green. No unit tests.
