---
id: 217
status: todo
branch:
depends: []
---
# Resource: identity of map-placed objects (embedded type-1 nodes), replace resman's raw byte scan

## Goal
Map packages place objects (roads, walk areas, scripted props) as **embedded type-1 child records**
inside type-1 instance blobs (0x519940). Each child's own blob carries an RSD text chunk with
`gametype <hex id>` and `params <text>`. src already parses the blob (`RpkArchive::parse_instance`:
`children`, `rsd`, `faces`) but nothing reads a child's RSD. resman counts the road/walk params with an
INVENTED raw byte scan over the whole file. Add one shared RSD helper and a recursive walk, and make
resman use them. No new runtime behaviour. This is the data side for later cards (notes/31 §6).
Nothing map-specific: the walk is generic over every type-1 record of any package.

## Exe evidence
### 0x519940 ResourceInstance_Game__loadData (notes/19 §7), only the parts used here
```
p = data + 4 + 4*u32[data]; nChild = u32 p; p += 4
repeat nChild: child record (same layout as the .rpk record table, flags&1 -> 48-byte bounds block)
while (p - data < size) { tag = u32 p; sz = u32 p+4
  tag 'RSD\0' : sz bytes of text lines; key "gametype" -> type id; key "params" -> params text (whole value)
  tag 0, sz 0 : the rest of the blob is RSD text
  'PHYS' / 'EXTP' / 'POLY' / other : skipped here }
```
`parse_instance` already implements all of this and concatenates the RSD text into `inst.rsd`.
### Line rule, as src already applies it for the ground record (src/game/runtime.cpp `ground_rsd`, ~line 156)
```
for each line of rsd (split on '\n', drop a trailing '\r'):
    key = text up to the first whitespace; skip whitespace
    key == "gametype" -> gametype = strtoul(rest, nullptr, 16); has_type = true
    key == "params"   -> params = rest (to end of line)
return has_type
```
### 0x4fe6f0 Collide__groundTree child walk order (notes/24): children in record order, type 1 recurse.

## Corpus facts (scratch parse of maps/city.rpk, notes/31 §1)
- 755 nodes with gametype 0x15 (`_type_road`), 1,799 with 0x352 (`_type_walk`); every 0x15 params
  string is unique.
- Every 0x15 node has the bounds block (`has_extra`), no PHYS faces and no children.

## Files
- edit: src/resource/rpk.h, src/resource/rpk.cpp
  - add `bool rpk_rsd_fields(const std::string &rsd, uint32_t *gametype, std::string *params);`
    (the line rule above, same code as `ground_rsd`'s loop; do NOT edit runtime.cpp in this card)
  - add a walker:
    ```
    struct RpkMapObject { RpkRecord rec; uint32_t gametype; std::string params; bool phys; size_t children; };
    // Every type-1 record reachable from rec (rec itself first, then children in record order,
    // depth first) that has an RSD gametype line. false only when parse_instance fails.
    bool RpkArchive::map_objects(const RpkRecord &rec, std::vector<RpkMapObject> *out, std::string *err) const;
    ```
    `phys` = `!inst.faces.empty()`, `children` = `inst.children.size()`.
- edit: src/tools/resman.cpp (the city.rpk block, ~lines 500-620): delete the raw scan (`raw`,
  `fs::read_file`, the `for (size_t i = city.rpk.table_end ...)` loop, `is_hex`). Instead, for every
  record of `city.rpk.records` with `type == 1`, call `map_objects` and feed each object with
  gametype 0x352 / 0x15 into the **existing** walk/road param checks (keep their check texts and
  counts; replace the `at byte N` context with the record name). Add checks:
  - `map_objects` succeeds for every type-1 record
  - every 0x15 object has `rec.has_extra`, `!phys` and `children == 0`
  - the 755 road params strings are pairwise distinct
  Print one summary line like the current `walk/road:` line.

## Done when
- each ported piece has a `// 0xADDR Name` comment (0x519940 for the walker/RSD); untraced logic is `INVENTED:`
- the old "INVENTED: scan the data area" comment and code are gone
- `make -C src clean check` green with the same 1799/755/484-53-218 results; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch name
