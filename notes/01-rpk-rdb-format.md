# RPK / RDB formats (SLRR 2.3.1)

Derived from the loader `FUN_0051e310` (0x51e310), `FUN_0051e6b0` (record parse), `FUN_0051e860` (id remap) and
validated against the sample pair `bin/system.rpk` (1836 B) + `bin/system.rdb` (9936 B).
Sample parses cleanly: 33 records, record blob ends exactly at its declared end, data area runs to EOF.
Parser: `tools/parse_rpk.py <file.rpk>`. Samples stay in `bin/` (gitignored).

## .rpk (binary, little-endian) — observed + verified on sample
```
0x00 char[4] magic  "RPAK" (0x4b415052)
0x04 u32     version 0x200 (loader compares against this)
0x08 u32     count_a   } loader allocates (count_a+count_b+1)*0x42 for the in-memory
0x0c u32     count_b   } sub-package table, and reads (count_a+count_b) 0x40-byte records
                         (both 0 in system.rpk; layout of those 0x40-byte records is
                         only partly known: u16 id at +2, name from +4, u32 at +0x3c)
0x10 u32     blob_size   (0x4dc)   size of record table
0x14 u32     record_count (33)
0x18 u32     ?  (1)    read, not used by the loader
0x1c u32     ?  (0x20) read, not used by the loader; equals offset of record table in sample
0x20 ...     record table (blob_size bytes), then data area to EOF
```
Caveat: the sample has count_a = count_b = 0, so I placed the record table at `0x20 + (a+b)*0x40`. That
is untested for non-zero counts (the loader reads the 0x40-byte records *before* blob_size, so the
real order may put them before 0x10). Needs a second sample to confirm.

### Record (variable length, `FUN_0051e6b0`)
| off | size | meaning |
|---|---|---|
| +0x00 | u32 | parent/super id (matches `superid` in .rdb) — **inferred** from sample |
| +0x04 | u32 | resource id: `pkg<<16 \| local`; remapped by `FUN_0051e860` to the runtime package index |
| +0x08 | u8 | type code (= `typeof` in .rdb; 8 game-object type, 9 physics body, 14 render object, 1 ins_game …) |
| +0x09 | u8 | flags; bit0 → a 48-byte block follows the name |
| +0x0a | 4 | unknown |
| +0x0e | u32 | data offset from file start (into data area) |
| +0x12 | u32 | data size |
| +0x16 | u8 | name length incl. NUL |
| +0x17 | n | name (NUL-terminated) |
| … | 48 | only if flags&1 — copied through to `FUN_00518880` (resource register). Sample `ins_game`: 3 floats 10.0 at +0x0c..+0x17 |

Note: my first decompile read said 24 bytes for the optional block; the pointer is `undefined8*` advanced by 6, i.e. **48 bytes** — confirmed by the sample (next record starts exactly 48 bytes later).
Record then calls `FUN_00518880(id, param2, &{type,flags,off,size}, param4, extra, 0)` to register it (not decompiled). A type-8 record with name length > 0 also duplicates up to 31 bytes of the name.
Flags seen: 0x00, 0x01 (ins_game), 0x04 (res_mesh/texture/sound/force_fx — resource kinds that point at source files).

### Data area
Each record's `[off, off+size)` is its **.rsd text** (CRLF, `key value` lines) — e.g. typ_game_object → `lod_amp 1.000`,
`Config` → `script java.util.Config`, `res_texture default_transparent.png` → `sourcefile Data\Textures\default_transparent.png`,
`typ_render_light` → directional light parameters. Records with size 0 have empty .rsd. Several records share an offset when size is 0.
So the .rpk embeds, for each resource, the same content as the matching `<id>.rsd` block of the .rdb.

## .rdb (text, CRLF) — observed on sample
A concatenation of pseudo-files:
```
<FILE external_links >
</FILE>
<FILE 00000001.res >      resource definition
typeof<TAB>8
superid<TAB>0x00000000
typeid<TAB>0x00000001
alias<TAB>typ_game_object
isparentcompatible<TAB>0.00
rpropcount<TAB>4
rprop 0 1 script           rprop <index> <type-code> <name>   (property schema)
...
</FILE>
<FILE 00000001.rsd >      resource data (same bytes as in the .rpk data area)
lod_amp 1.000
</FILE>
```
- 67 blocks in the sample: `external_links` + 33 `.res` + 33 `.rsd`, matching the 33 rpk records one-to-one by id.
- `.res` fields: `typeof` = rpk type byte, `superid` = rpk record +0x00, `typeid` = rpk id, `alias` = rpk name.
- `rprop` type codes seen: 0, 1, 5, 82, 114 … (meaning not decoded; 114 is used for `texture`/`shd_texture`, 82 for `mesh`).
- This explains why the exe has no ".rdb" string: it is probably the editor/source form that gets compiled into the .rpk, and the game only opens `.rpk` (**inferred**; I did not find the exe reading .rdb).

## Resource-type names (from the sample)
`typ_game_object`(8) `typ_physics_body`(9) `typ_render_object`(14) `typ_render_light`(13) `typ_render_camera`(12) `typ_physics_constraint`(10) `typ_physics_particle`(11)
`ins_game`(1) `ins_physics`(2) `ins_render`(3) `ins_render_sprite`(15) `ins_render_text`(16) `ins_render_horizon`(17) `ins_render_sound`(20)
`res_mesh`(5) `res_texture`(7) `res_sound`(6) `res_force_fx`(4) `res_viewport`(18) `res_animation`(19)
Game-object subtypes: `Config`, `Dummy`, `game`, `native`(parent of `camera`, `Controller`, `map`, `trigger`), `career_event`, `track_script`, `track_script_hidden`, `gamemode`.
`.rsd` lines `script <java class>` / `native <name>` tie a resource to a script class (`java.util.Config`, `system\Scripts\io\Controller.class`) or a native handler — the link between the RPK resource system and the script VM described in 00-overview.md.

## Still open
- Meaning of the unknown bytes at record +0x0a..0x0d, and header u32s at 0x18/0x1c.
- Layout/position of the `count_a+count_b` 0x40-byte sub-package records (need a non-trivial .rpk, e.g. `cars.rpk`).
- `FUN_00518880` (registration) and how `typeof` selects the C++ `ResourceType_*` class.
