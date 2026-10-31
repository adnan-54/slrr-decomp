# 41 — City2 mount: what 0x519940 actually is, and where sub-packages get registered

Corrects the assumption recorded in notes/38 and CHECKPOINT 0y ("the real blocker is mounting
city2 via 0x519940"). Traced 2026-10-31 (card 256).

## 1. `0x519940` is not the mount entry

`0x519940` parses an **instance blob** and registers the embedded children. Its only caller is
the streamed-node vtable slot +0x28 `0x5195a0` (vtable `0x5a7d70` slot 10). `0x5195a0` is
invoked by the streaming tick `0x517f60` (`data = node+0x48`, `ratio = typed+0x9c`), and only
**after** that does the tick mount the package named by the node id's high word
(`typed+0x82`) through `0x51e310`. Render/physics are *not* in `0x519940`; they live in
`0x5195a0`'s tail (native vf3 `0x4bade0`) and the per-step `0x4fe6f0`.

Package loading (`0x51e310 Package::load`) is where the table bookkeeping happens:
`0x518940` registers sub-package names, `0x51e6b0 -> 0x518880 -> 0x518410` links record
nodes. This is pure table work — no render, no physics.

## 2. Nothing in the shipped city selects city2 through that path

`city.rpk` sub-package 3 is `maps\city2.rpk`, but a scan of the shipped `city.rpk` finds **no
record and no embedded id that selects entry 3** (rpkdump supers are `0x0001`/`0x0004`; a
strict byte scan found no child id with hi = 3 and a type/flags tail; one weak unaligned hit
at file offset `0x378ea` is probably not a real child id).

So the earlier "the exe mounts city2 and src does not" framing is unsupported by the data we
have: in the shipped map nothing references city2 that we can see. `city.rpk` merely *declares*
the sub-package.

## 3. Open questions (in order)

1. Which reference first mounts `maps\city2.rpk`. Candidates: the `0x517f60` node-id hi-word
   mount, the `0x51e310` callers `0x5171be`/`0x5183e7`/`0x518906`, or the script constant at
   `0x5189e0` — but no shipped `.java`/`.class` contains the string "city2", which argues
   against the script path.
2. Receiver of `FUN_0051f790(1)` at `0x5199af` (no ECX load; Ghidra assumes the incoming
   `this`).
3. The 4th stack argument (float `1.0f`) at the `0x519636` call is never read by `0x519940`.
4. `0x518410` per-type node construction (the source of the node id and `typed+0x82`) is not
   traced.
5. `0x51e6b0`'s two header comparisons are no-ops in the decompile (likely compiled-out
   asserts).

## 4. Consequence for src

- Do **not** chase a `city2` mount to make the city bigger: the shipped data shows no
  reference to it, so a src-side mount would be inventing behaviour the exe does not perform
  on this map.
- The ported traffic work keeps running on the resident half of the city (`z <= 328.5`) via
  the harness fixture from card 243, which is the honest stand-in while src has no streaming
  or render walk.
- If streaming is ever ported, the portable part is the **table bookkeeping**
  (`0x51e310`/`0x518940`/`0x51e6b0`/`0x518880`/`0x518410`); the node create and per-step
  wires (`0x5195a0` vf3, `0x4fe6f0` residency) are already ported as
  `Runtime::map_node_touched`.

## 5. Card

`tasks/256-resource-instance-loaddata-streaming-mount.md` (status `draft`: evidence only, not
promotable until question 1 is answered).
