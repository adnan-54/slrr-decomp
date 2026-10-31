---
id: 256
status: rejected
branch:
review:
depends: []
---
# resource: type-1 instance loadData 0x519940, its load caller 0x5195a0 and the streaming tick 0x517f60

## Goal
Decide whether src's "no streaming" stub can be replaced by the exe's load/mount path.
Traced: `ResourceInstance_Game__loadData` 0x519940 (parses one type-1 instance blob and
registers its embedded child records), its only caller `ResourceInstance_Game vf+0x28`
0x5195a0 (vtable 0x5a7d70 slot 10), and `Resource__streamTick` 0x517f60, which calls
vf+0x28 and then mounts the package selected by the node's own id high word.

**Decision (answers the card's question).** The mount is **table bookkeeping only**.
0x51e310 `Package::load` registers sub-package names (0x518940) and creates/links one
node per record (0x51e6b0 -> 0x518880 -> 0x518410); no typed-data parse, no render and
no physics body is created there. 0x519940 additionally parses the record's own blob
(PHYS faces/verts, EXTP precache + AABB union, RSD params/gametype, POLY) and registers
its embedded child nodes. The render instance and the ground body are created later on
the separate residency path (0x5195a0 tail -> native `vf3`, e.g. type_ground 0x4bade0),
and the ground triangle contacts come from the per-step `Collide__groundTree` 0x4fe6f0
walk (notes/19 §7). So a replacement needs bookkeeping only; **do not** put render or
physics walks inside the mount.

**Correction to notes/38 (important).** 0x519940 is not what mounts `maps\city2.rpk` for
the shipped city map: no city.rpk record or embedded child id selects sub-package entry 3
(city2). Checked with the shipped files: `rpkdump` lists city sub-package 3 =
"maps\city2.rpk"; every city.rpk record super is in sub-package 1 (system) or 4
(textures); a byte scan of city.rpk data for a child-record id with high word 3 and a
plausible type/flags tail finds none (one weak non-aligned hit at 0x378ea). The actual
first city2 reference therefore is one of the other package-slot mounts named below and
is **UNRESOLVED**. Keep this card draft until that trigger is named.

## Exe evidence (cleaned pseudocode, renamed fields)
`u32(p)`/`grow` are 0x567980 (read LE dword) / 0x567a20 (malloc); `line` helpers are
0x569350 (read C string) / 0x569380 (sscanf) / 0x57c11e (strcmp). `T` = typed data at
R+0x108. Field names: R+0x80 id, R+0x84 flags (= our `Resource::flags`, src comment
"RI+0x84"), R+0x8c data size, R+0x94 bit0 loaded (= our `script_class_checked`, src comment
"node+0x94 bit0 (0x51f790)").

### 0x519940 ResourceInstance_Game::loadData
`__thiscall`, ECX = T (R+0x108); stack args `[esp+4]=R`, `[esp+8]=data`, `[esp+0xc]=size`,
`[esp+0x10]=float` (1.0f at the 0x519636 call site, **never read** by this function:
UNRESOLVED purpose). RET 0x10. Returns 1.0f or 0.0f.
```
float loadData(T, R, data, size) {
  if (!data) return 0.0f;
  pkg = 0x517b80(R->id);                 // ECX = registry [0x5db008]; loads Package 0x51e310
  if (!pkg) return 0.0f;                 // abort: owning package not resolvable
  p = data;
  if (u32(p) != 0) p += 4 + 4*u32(p);    // leading dword count, 0 in every shipped map
  nChild = u32(p); p += 4;
  ok = 1.0f;
  0x51f790(1);                           // UNRESOLVED receiver: no ECX load in the stream
  for (i = 0; i < nChild; i++) {         // embedded child records (same format as .rpk table)
    rec = p;
    rec->flags(+9) |= 8;
    p += 0x51e6b0(rec, R->id, &child, R);         // ECX = pkg
    if (child && (R->flags & 0x80000)) {
      child->flags(+0x84) |= 0x80000;
      if (child->+0x7c != 1) child->vf14(1.0f);
      if (0x51f4f0(0x80000000, 0, 0) >= 0) child->vf12(1.0f);
    }
  }
  while (p - data < size) {
    tag = u32(p); sz = u32(p+4); q = p+8;
    if (tag == 0) {
      if (sz != 0) { p = q + sz; continue; }
      walk text (size - 12 - (p-data) bytes), one line each:
        key = first token;
        if (key == "gametype") 0x51fa40(0x51e860(value), 8, 0);   // precache type id
        if (key == "params")  { if (!T->+0x48) T->+0x48 = grow(0x801);
                                sscanf(line, "%*s %2048s", T->+0x48); }
      break;                                    // RSD is the blob tail
    }
    if (tag == 0x445352 /*'RSD\0'*/) { walk sz bytes of text exactly as above; p = q + sz; continue; }
    if (tag == 0x53594850 /*'PHYS'*/) {
      nf = u32(q); T->+0x18 = nf; T->+0x1c = grow(nf*0x20);
      for each face (file 28 B): u32 mat -> +0; u32 i0,i1,i2 -> +4,+8,+0xc; f32 n[3] -> +0x10..0x18;
                                  +0x1c = 0;
      nv = u32(..); T->+0x10 = nv; T->+0x14 = grow(nv*12); copy nv f32[3] (world metres);
      continue;
    }
    if (tag == 0x50545845 /*'EXTP'*/) {
      for each of sz/4 ids: 0x51e650(id);              // ECX = pkg; precache
      // grow R's AABB (+0xb0 kind=1, centre +0xb4, half +0xc0) to cover every node
      // of R's +0x30 child list: node+0xb4 centre, node+0xc0 half; follow node+0x4.
      continue;
    }
    if (tag == 0x594c4f50 /*'POLY'*/) {
      T->+0x30 = n = u32(q); T->+0x34 = grow(total);
      per record: {u32 a, u32 n14, u32 n3, n14*14 words, n3*3 words}; continue;
    }
    p = q + sz;                               // unknown tag: skip by size
  }
  DAT_00773ab4++;                             // loaded game-instance counter
  return 1.0f;
}
```
Immediate callees (Ghidra, 2 indirect vtable calls unseen): 0x517b80, 0x51e650, 0x51e6b0,
0x51e860, 0x51f4f0, 0x51f790, 0x51fa40, 0x567980, 0x567a20, 0x569350, 0x569380, 0x57c11e.

### 0x51e6b0 record -> node registration (the only 0x519940 helper that touches the tables)
```
int regRecord(rec, parentId, &outNode, R) {   // this/ECX = the Package to remap ids in
  // rec[0] low16 vs parentId low16 and parentId vs R+0x80: compared, no branch taken
  // (compiled-out assert; no behavioural effect observed)
  name = (rec->flags(+9) & 1) ? malloc(0x20)+copy(rec+0x17,31) : 0;   // debug name only
  gid = 0x51e860(rec->id(+4));                 // this = Package (ECX)
  desc = { type = rec->type(+8), flags = rec->flags(+9),
           off = u32(rec+0xe), size = u32(rec+0x12) };               // + 48-byte extra if flags&1
  outNode = 0x518880(gid, parentId, &desc, R, 0, 0);
  free(name);
  return bytes consumed (0x17 + rec->+0x16 nameLen + 48 if flags&1, 4-aligned);
}
```

### 0x51e860 id -> global id (already in src `ResourceManager::global_id`)
`this` = Package. hi = id>>16, lo = id&0xffff. hi == 0xffff -> return id; lo == 0 or
hi >= registry count -> 0; hi == 0 -> own registry index<<16|lo; 1 <= hi <=
P->+0x54+P->+0x58 -> entry hi's stored registry index (P->+0x5c + hi*0x42, ushort at +2)
<<16 | lo.

### 0x518880 preload + create
```
node = 0x518880(gid, parentId, &desc, R, 0, 0) {   // this/ECX = registry [0x5db008]
  hi = gid >> 16;
  if (hi != 0xffff && hi < registry.count(+0x166740))
      0x51e310(registry + 0x1667b4 + hi*0x70);     // mount the package the id belongs to
  node = 0x518410(parentId, &desc, 0, 0, R);       // per-type ctor, link
  if (node) 0x51f790(node, 0);                     // loaded bit 0
  return node;
}
```
`0x518410` switch(type): cases 1,2,3,5,6,7,8,9,10,0xc,0xd,0xe,0x12,0x13,0x14,0x15 each
allocate and build a node; unknown type -> 0 and log "Node registration error". On success:
`node+0x24 = parentId_resolved; tail-insert into parent+0x58 list via node+0x4/+0x8`.

### 0x517b80 registry slot (already in src `ResourceManager::registry_package`)
`hi == 0xffff -> registry+0x166744; hi < count -> 0x51e310(registry+0x1667b4+hi*0x70);
return slot if load succeeded else 0; else 0`.

### 0x51e310 Package::load (the mount; pure bookkeeping)
```
if (P->state(+0x50) == 0) {
  if (0x565200(P+0xc) == 0) return P->state;        // file missing: stays 0, retried later
  P->state = 1;
  f = 0x565760(P+0xc);                              // open archive
  if (f) {
    read "PRAK"/0x200; read count_a -> P+0x54, count_b -> P+0x58;
    P->+0x5c = grow((count_a+count_b+1)*0x42);      // sub-package/record entry table
    table[0].+2 = P->registryIndex(+0x8); table[0].+0 = 0; table[0].+0x3e = 0; table[0].+4 = 0;
    for i in 1..count_a+count_b:
      read 0x40-byte entry; table[i].+2 = entry[2..3] (index, then overwritten)
      copy 0x100-byte name to table[i]+4;
      table[i].+0x3e = entry dword;
      table[i].+2 = 0x518940(table[i]+4);            // register name -> registry slot
    read the record blob (length/counts, malloc, read);
    for each record: 0x51e6b0(rec, 0x51e860(own id, this=P), &node, 0);
    free(blob); P->+0x4c = 0;
  }
  P->state = 2;
}
return P->state;                                      // 2 = loaded
```
So `Package::load` registers every sub-package name and every record node. It does **not**
open sub-package files and does **not** parse record data.

### 0x5195a0 ResourceInstance_Game vf+0x28 (vtable 0x5a7d70 slot 10; only xref 0x5a7d98)
This is the only caller of 0x519940 (call at 0x519636). `__thiscall`, ECX = R; `[esp+4]=data`,
`[esp+8]=float ratio` (0x517f60 passes typed+0x9c). RET 0x8.
```
float load(R, data, ratio) {
  c = R->+0x24;
  if (c && !(c->+0x94 & 1) && c->+0x7c != 1) {
    c->vf14(1.0f);
    if (0x51f4f0(c, 0, 0) >= 0) c->vf12(1.0f);
  }
  if (!(R->+0x94 & 1)) {                     // first residency only
    r = 0x519940(R+0x108, R, data, R->+0x8c, ratio);   // 4 stack args, see 0x519940
    0x51f790(R, 1);                          // mark R loaded (bit0)
    if (R->+0x130 == 0 && R->+0x24) { v = R->+0x24->vf12(1.0f); if (v) 0x40cac0(R+0x128, v+0x2c); }
    if (!(R->+0x94 & 1)) return R->+0x98;
  }
  if (R->+0x154 == 0 && R->+0x148 != 0) { /* create native/script instance (notes/19 §7) */ }
  return 1.0f;
}
```
Exact run condition for 0x519940: `data != 0`, `R->+0x94 bit0 == 0`, and inside 0x519940
`0x517b80(R->id) != 0`. The tail (instance/native creation) is the render/physics side of
0x5195a0 and the reason 0x5195a0 itself must not be conflated with the mount.

### 0x517f60 render/streaming tick: the caller that hands the data in and mounts the package
Called from the per-frame 7-call loop (src `src/script/natives.cpp` ~line 6361). Relevant slice:
```
for (n = queueHead; n; n = n->next) {
  typed = n->+0x3c;
  ...
  typed->vf28(n->+0x48, typed->+0x9c);       // RI vtable +0x28 = 0x5195a0(R, data, ratio)
  typed->+0x84 |= ...;                       // (flags follow-up, not part of the mount)
  if (n->+0x48 != 0) {
    hi = u16(typed + 0x82);                  // high word of the RI id at +0x80
    if (hi != 0xffff && hi < registry.count(+0x166740)) {
      slot = registry + 0x1667b4 + hi*0x70;
      if (0x51e310(slot) != 0) 0x51e900(n+0x48);    // free the loaded blob
      else                     /* leave queued */ ;
    } else /* slot registry+0x166744 */;
  }
}
```
So the tick is the call site that supplies the blob pointer (queue node +0x48) and the
ratio (typed+0x9c), and immediately after the load it mounts the package named by the
node's own id high word. If such a node's id is in city2's registry slot, this is what
mounts maps\city2.rpk — but that node must already exist/resolve, so this does not
explain the first city2 reference either (see UNRESOLVED).

## Files
- read only now (decision): src/game/runtime.cpp (`Runtime::mount_package`, `resolve`,
  `gather_ground_soup`), src/resource/manager.cpp (`registry_package`, `global_id`),
  src/resource/rpk.h (`RpkRecord`/`RpkInstanceData`).
- when promoted, edit at most: src/game/runtime.cpp (port the 0x519940 embedded-child
  registration/preload at the instance-data path), src/resource/manager.cpp (only if the
  named city2 trigger turns out to be `registry_package`), src/script/natives.cpp
  (~6361, a streaming-tick stub calling vf+0x28 + the slot mount).

## Done when
- The decision is recorded and used: the mount is bookkeeping only; no render/physics is
  added to `mount_package`/the 0x519940 port (they stay with 0x5195a0's tail and 0x4fe6f0).
- The first reference that mounts `maps\city2.rpk` is named (this card stays draft until
  then; candidates in UNRESOLVED).
- After promotion: each ported function has a `// 0xADDR Name` comment, untraced logic is
  `INVENTED:` / `shortcut:`; `make -C src check` green; no new unit tests or hand-made
  expected values; frontmatter status implemented + branch name.

## UNRESOLVED
- Receiver of `FUN_0051f790(1)` at 0x5199af: no ECX load in the instruction stream; Ghidra
  uses the incoming this (T), so the object being marked loaded is unclear.
- 4th stack arg (float 1.0f) at the 0x519636 call is never read by 0x519940.
- Which reference first mounts maps\city2.rpk. Not a city.rpk record/embedded id (scans
  above); not a shipped .java/.class string ("city2" absent from scripts). Remaining
  candidates: 0x517f60 node id hi-word mount, the other 0x51e310 callers 0x5171be /
  0x5183e7 / 0x518906, or the script-constant resolver 0x5189e0 (would load it if any
  script held `maps.city2:NNr`).
- The weak non-aligned byte-scan hit at city.rpk file offset 0x378ea (value 0x00037970,
  type 0x14, flags 1) may or may not be a real embedded child id.
- 0x518410's per-type node construction (node id / typed+0x82 source) not traced; only the
  parent link (`node+0x24`, tail insert at parent+0x58) is known.
- 0x51e6b0's two header comparisons (rec[0] low16 vs parent id, parent id vs R+0x80) have
  no observable effect in the decompile (likely compiled-out asserts).
