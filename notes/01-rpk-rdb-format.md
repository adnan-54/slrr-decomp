# RPK / RDB formats (SLRR 2.3.1)

Derived from the loader `FUN_0051e310` (0x51e310), `FUN_0051e6b0` (record parse), `FUN_0051e860` (id remap) and
validated against the sample pair `bin/system.rpk` (1836 B) + `bin/system.rdb` (9936 B).
Sample parses cleanly: 33 records, record blob ends exactly at its declared end, data area runs to EOF.
Parser: `tools/parse_rpk.py <file.rpk>`. Samples stay in `bin/` (gitignored).

## .rpk (binary, little-endian) — verified on system.rpk (33 records) and Axis.rpk (15 sub-packages, 340 records)
```
0x00 char[4] magic  "RPAK" (0x4b415052)
0x04 u32     version 0x200 (loader compares against this)
0x08 u32     count_a  } n = count_a+count_b external sub-package references
0x0c u32     count_b  }   (Axis.rpk: 15 + 0; the loader sums them, meaning of the split unknown)
0x10 n * 0x40-byte sub-package entries
       +0x00 u16 0
       +0x02 u16 package index (1..n) — the high 16 bits of ids that point into that package
       +0x04 char[56] relative path, NUL padded ("system.rpk", "cars\\racers.rpk", "parts\\engines\\X.rpk")
       +0x3c u32 0 (stored by the loader at +0x3e of its 0x42-byte in-memory entry; always 0 in samples)
0x10+n*0x40:
     u32 blob_size     size of the record table
     u32 record_count
     u32 ?  (0 in Axis, 1 in system)
     u32 ?  (0x154 in Axis, 0x20 in system)  — read, unused by the loader
then record table (blob_size bytes), then data area to EOF
```
Correction to the previous revision of this note: the sub-package entries sit **before** blob_size (starting at 0x10),
not after; with n=0 the table starts at 0x20 as in system.rpk. Every package loads each sub-package name through the
registry `FUN_00518940`, which is how cross-package ids get resolved at runtime (`FUN_0051e860`).

### Record (variable length, `FUN_0051e6b0`)
| off | size | meaning |
|---|---|---|
| +0x00 | u32 | parent/super id, same `pkg<<16 \| local` form (matches `superid` in .rdb; e.g. Axis `0x00030005` = package 3 `cars\\racers.rpk`, local 5) — verified on both samples |
| +0x04 | u32 | resource id: `pkg<<16 \| local`; remapped by `FUN_0051e860` to the runtime package index |
| +0x08 | u8 | type code (= `typeof` in .rdb; 8 game-object type, 9 physics body, 14 render object, 1 ins_game …) |
| +0x09 | u8 | flags; bit0 → a 48-byte block follows the name |
| +0x0a | f32 | 1.0 in most Axis records (type 9/8/14/5/7), 0.0 for many system records; likely a scale/weight — meaning unknown |
| +0x0e | u32 | data offset from file start (into data area) |
| +0x12 | u32 | data size |
| +0x16 | u8 | name length incl. NUL |
| +0x17 | n | name (NUL-terminated) |
| … | 48 | only if flags&1 — copied through to `FUN_00518880` (resource register). Sample `ins_game`: 3 floats 10.0 at +0x0c..+0x17 |

Note: my first decompile read said 24 bytes for the optional block; the pointer is `undefined8*` advanced by 6, i.e. **48 bytes** — confirmed by the sample (next record starts exactly 48 bytes later).
Record then calls `FUN_00518880(id, param2, &{type,flags,off,size}, param4, extra, 0)` to register it (not decompiled). A type-8 record with name length > 0 also duplicates up to 31 bytes of the name.
Flags seen: 0x00, 0x01 (ins_game), 0x04 (types 5 mesh, 7 texture, 6 sound/res_force_fx — the kinds that name source files). Axis.rpk type histogram: 9 physics body ×104, 8 game object ×80, 14 render object ×62, 5 mesh ×60, 7 texture ×33, 6 sound ×1.

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
- 67 blocks in system.rdb (`external_links` + 33 `.res` + 33 `.rsd`), 681 in Axis.rdb (1 + 340 + 340): one-to-one with the rpk records.
- `external_links` lists the sub-package paths in order, as directories with a trailing `\\` (e.g. `cars\\racers\\`) = the rpk sub-package names without `.rpk`. Sub-package index = line number (1-based). Axis.rdb's list equals Axis.rpk's 15 entries.
- `.res` fields: `typeof` = rpk type byte, `superid` = rpk record +0x00, `typeid` = rpk id, `alias` = rpk name.
- `rprop` type codes seen: 0, 1, 5, 82, 114 … (meaning not decoded; 114 is used for `texture`/`shd_texture`, 82 for `mesh`).
- This explains why the exe has no ".rdb" string: it is probably the editor/source form that gets compiled into the .rpk, and the game only opens `.rpk` (**inferred**; I did not find the exe reading .rdb).

## Resource-type names (from the sample)
`typ_game_object`(8) `typ_physics_body`(9) `typ_render_object`(14) `typ_render_light`(13) `typ_render_camera`(12) `typ_physics_constraint`(10) `typ_physics_particle`(11)
`ins_game`(1) `ins_physics`(2) `ins_render`(3) `ins_render_sprite`(15) `ins_render_text`(16) `ins_render_horizon`(17) `ins_render_sound`(20)
`res_mesh`(5) `res_texture`(7) `res_sound`(6) `res_force_fx`(4) `res_viewport`(18) `res_animation`(19)
Game-object subtypes: `Config`, `Dummy`, `game`, `native`(parent of `camera`, `Controller`, `map`, `trigger`), `career_event`, `track_script`, `track_script_hidden`, `gamemode`.
`.rsd` lines `script <java class>` / `native <name>` tie a resource to a script class (`java.util.Config`, `system\Scripts\io\Controller.class`) or a native handler — the link between the RPK resource system and the script VM described in 00-overview.md.

## Companion files seen next to Axis.rpk (from the same mod/part pipeline)
- `FL_door.cfg`: "Vehicle importer wizard generated part configuration" — `render/mesh/texture/click` resource ids (`pkg<<16|local`), `body` (pos, rot, mass, `.scx` collision shape, `;0x20` id comment), `slot` / `attach` lines (part-attachment graph), `stockpart`. This is the `.cfg` the exe refers to ("Missing .cfg: %s").
- `Axis_FL_door.java` / `.class`: part script class in package `java.game.cars`, `extends FrontDoor` (from `java.game.parts.bodypart`), sets `name`, `description`, `value`, `brand_new_prestige_value`. `.class` is the compiled form the VM loads (`system\\Scripts\\io\\Controller.class` in system.rsd uses the same extension). `Init.java/.class` also present (not examined).
- `.rsd` for type 9: `shape <path>.scx`, `mass <kg>` — matches the `body` line in the .cfg.

## Still open
- Header u32s after record_count (x1, x2) and the exact meaning of count_a vs count_b.
- The record float at +0x0a.
- `FUN_00518880` (registration) and how `typeof` selects the C++ `ResourceType_*` class.
