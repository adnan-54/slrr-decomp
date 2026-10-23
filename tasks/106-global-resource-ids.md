---
id: 106
status: draft  # needs a Claude design pass before DeepSeek: touches every id consumer
branch:
depends: [105]
---
# resource: global runtime ids (registry index << 16 | local) and lazy package mount

## Goal
Make src's runtime ids behave like the exe's (notes/16 "Runtime ids are GLOBAL"):
1. A name registry in ResourceManager (find-or-append by canonical name, lazy .rpk mount on first
   use; registry index = id high word). Every package's sub-package table maps to registry indices.
2. Records resolve by global id: hi = registry index, lo = local.
3. VM resource constants (opcode 0x400e, CONST_RESOURCE) become `Value::make_int(global id)` at the
   point the exe does it (class load, 0x54b310) or at first execution (equivalent result).
4. Natives/Runtime that currently interpret ids relative to a base package switch to global ids.

## Open design questions (Claude)
- Registry index of system.rpk etc.: the exe's order is "first referenced first"; src must not
  depend on exact numbers except where scripts can observe them (System.openLib returns the index).
- Which existing checks hard-code relative ids (resman 0xD2 lookups, Axis chains) and how to keep
  them meaningful (they test the record layer, which stays relative inside a package).
- Inventory of Runtime/natives call sites that pass a `base` package.
