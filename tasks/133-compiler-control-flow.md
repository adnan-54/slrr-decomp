---
id: 133
status: implemented
branch: card/133
depends: [132]
---
# compiler stage 4b: control flow + remaining first-difference groups

## Goal
Extend the TREE codegen (src/script/compiler/emit.cpp) so more shipped bodies match:
loops while/do/for (0x16, 0x15/0x14 offsets), switch (0x17/0x18), break/continue,
ternary, array initializers, uninitialized locals, `this.x` receivers, string concat.
Start: `bodies: total 7096, identical 6381; CONS exact 2542 of 2809`.

## How
`classdump --compile-check --show <path-substr> "bin/the game"` per pair; work the
first-difference groups by size; record each rule with its pair in notes/20 "Stage 4b
results". Don't chase float 1-ulp differences.

## Result
`bodies: total 7096, identical 6821; CONS exact 2681 of 2809 units`. Rules: notes/20
"Stage 4b results". Remaining 275 differences: 189 float ulp, 68 resource-literal package
casing (needs an exe trace), the rest stale shipped classes.

## Done when
- `make -C src -j4 check` green (compile-check stays report-only).
