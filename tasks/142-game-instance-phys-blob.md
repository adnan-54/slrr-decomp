---
id: 142
status: merged
branch: card/142
depends: [141]
---
# Parse game-instance (type 1) data blobs: embedded children, PHYS triangle soup, RSD text

## Why
The woken freeride car falls forever: src has no map collision. The exe's map collision is not
in the `.scm` (materials list only) nor in a `.scx` shape: it is the `PHYS` triangle soup in the
binary data of the map's `ins_game` records (top map record + its embedded child records),
parsed by `ResourceInstance_Game__loadData` 0x519940. src's rpk reader only parses the record
table. This card adds the blob parser and proves it on every shipped map; card 143 turns the soup
into the ground collider. RE: notes/19 §7 (format, corpus numbers), notes/01 (record format).

## Exe functions (2)
- `ResourceInstance_Game__loadData` 0x519940 (ECX = typed data T = R+0x108; args R, data, size):
```
p = data + 4 + 4*u32[data]                  // skip n leading dwords
nChild = u32 p; p += 4
repeat nChild: p += 0x51e6b0(p, ...)        // child record, same layout as the .rpk record table:
               // super u32, id u32, type u8, flags u8, f32, off u32, size u32, nameLen u8, name, [48 if flags&1]
               // off/size index the same .rpk file
while (p - data < size) { tag = u32 p; sz = u32 p+4; q = p + 8
  tag 0 && sz 0 : rest of blob (from p+0xc) is RSD text -> stop chunk walk
  0x445352 'RSD\0': sz bytes of text ("gametype %d", "params %2048s"); continue after it
  0x53594850 'PHYS': nf = u32 q; q += 4
        nf x { u32 mat, u32 i0, u32 i1, u32 i2, f32 nx, ny, nz }      // 28 bytes each in the file
        nv = u32; nv x f32[3] verts                                 // metres, world frame
        p = end of parsed data (exe ignores sz)
  0x50545845 'EXTP': sz/4 u32 ids (precache)        // keep the ids, no precache in src
  0x594c4f50 'POLY': skip (p = q + sz)              // INVENTED: exe parses it into T+0x30/+0x34, not needed yet
  other : p = q + sz }
```
- record layout = `Package__parseRecord` 0x51e6b0 (already implemented by `RpkArchive::parse`
  for the table; reuse that code for the embedded records, do not duplicate it).

## Files (3)
- src/resource/rpk.h: `struct RpkPhysFace { uint32_t mat; uint32_t v[3]; float n[3]; };`
  `struct RpkInstanceData { std::vector<RpkRecord> children; std::vector<RpkPhysFace> faces;
  std::vector<float> verts /*xyz*/; std::vector<uint32_t> extp; std::string rsd; int phys_chunks; }`
  and `bool RpkArchive::parse_instance(const RpkRecord &rec, RpkInstanceData *out, std::string *err) const`.
  Multiple PHYS chunks in one blob append (offset the indices; shipped blobs have at most one,
  assert that in the check rather than assuming it).
- src/resource/rpk.cpp: the parser (strict: every read inside `[data_offset, data_offset+data_size)`,
  child data ranges inside the file, face indices < nv of their chunk, PHYS `sz` equals the parsed
  length (log + fail otherwise); the walk must end exactly at the blob size). Factor the existing
  record-table record reader into a helper used by both.
- src/tools/rpkdump.cpp `--selfcheck`: corpus check over `bin/the game/maps/{city,city2,test_track,
  ROC_track}.rpk` (skip a missing file with a note): parse every type-1 record of the table and,
  recursively, every embedded type-1 child; hard-check the totals (from notes/19 §7):
  | map | type-1 instances | PHYS | faces | verts |
  |---|---|---|---|---|
  | city | 58210 | 23976 | 197286 | 236064 |
  | city2 | 56689 | 19040 | 251902 | 276107 |
  | test_track | 12224 | 6420 | 66825 | 77132 |
  | ROC_track | 10894 | 5147 | 55040 | 61777 |
  plus: 0 parse failures, every embedded child id unique per map, city max `mat` 107 < 108 and city2
  max `mat` 74 < 75 (the `.scm` `materials N` header).

## vmboot check (src/tools/vmboot.cpp, no driver change)
After the existing `Track.map is a non-null GroundRef` check: resolve `Track.map`'s handle target
(`boot.runtime()->handle(mv.obj)->target`, as done for the chassis) and parse its record plus all
embedded type-1 children recursively; print the map name and totals; hard check faces > 0 and the
totals equal the table row of that map's package.

## Done when
- `make -C src -j4 check` green (rpkdump selfcheck prints the 4 map rows, all ok).
- `(ulimit -v 4000000; timeout 300 src/build/vmboot --selfcheck)`: `selfcheck: PASSED`, the new
  check ok; report the freeride map name and its face count.

## Not this card
Ground body and contacts (card 143); mounting the embedded children as runtime resources; POLY
records; `mat` meaning when the map has no `.scm` (UNRESOLVED, notes/19 §7).
