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
- The exe has no ".rdb" string and never reads it: `.rdb` is the modding text form (made from an .rpk, edited, converted back). A stock install ships only `.rpk` (user, 2026-10-23). Everything src needs is in the .rpk: alias = record name, super id = record +0x00, type names = the `typ_`/`ins_`/`res_` record names (identical to the .rdb aliases on system.rpk), .rsd text = record data. src opens `.rpk` only; `.rdb` is an optional tool cross-check (31 of the 111 shipped .rdb files don't parse with our reader: hand edits).

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

## Init.java / Init.class (script entry point; read from the sample, not the exe)
- `package java.game; class Init extends GameType` (author comments mark it as the RAXAT v2.3.1 modded source). Constructor order: `Frontend.init()` → loading screen with resource literal `frontend:0x000003EBr` → precache 3 menu `SfxRef`s → `Sound.init()` → `Input.initControllers(this, "save/controls/active_control_set")` → apply `Config.restart_*` video settings (same `restart_apply` / `restart_video_*` keys `FUN_004ed680` reads natively) → `new GameLogic()` (loads MainMenu.class) → optional debug OSD. `finalize()` tears down Input/Frontend. Likely what the exe's `GameInit` call (`FUN_00523c80(..., "GameInit")` in the main loop) ends up instantiating (**inferred**, not traced).
- Script syntax notes: Java-like with resource literals `pkgname:0xHEXr` (package-name prefix, hex local id, `r` suffix), `String.cut`, `Integer.getHex`, `System.trace`, `runDLL(module, fn, sig, args)` (matches the `libcall.dll` string in the exe).
- The commented-out "simple RPK generator" confirms the .rdb conventions: `typeof 8`, `superid` hex, `typeid` hex, 8-digit zero-padded filename, `script <path>.class` in the .rsd.
- `Init.class` is **not** a JVM class: magic `54 55 46 41` ("TUFA"), u32 4, u32 0x24f9d (size?), then a `CONS` section (u32 len 0x836, u32 count 0x7b = 123 constants, strings like `java.game.Init`). A custom bytecode format for the exe's own VM; not decoded.

## Refinements verified by the C++ parser

The parser in `src/resource/` (`rpkdump --selfcheck`, both sample pairs) confirmed the above and refined these points:

1. `.rdb` `.res` filenames are **not** resource ids. system.rdb mostly uses the 8-digit lowercase-hex typeid (28/33; some differ), Axis.rdb mostly uses sequence/copy-pasted numbers (1/340 equals the typeid; 14 duplicate names, e.g. five `00000900` blocks). Resolve by the `typeid` field.
2. `rprop` type codes are signed and can be negative: -30 `rendertype` (`ins_render`), -126 `gametype` (`ins_game`), besides 0/1/5/82/114.
3. `.rdb` contains banner/comment and blank lines outside `<FILE ...>` blocks (Axis.rdb has a commented CUSTOM DOOR section between blocks). Ignore all text outside blocks.
4. `external_links` lines are directory paths with a trailing backslash (`cars\\racers\\`), not `.rpk` names: strip `.rpk` from the rpk entry and add `\` (case-insensitive) to compare. Sub-package index = line number (1..n); `z` and `tail` are 0.
5. Binary payloads do not round-trip through the text `.rdb`: system.rpk id 4 (`ins_game`) carries 8 NUL bytes in the data area while its `.rsd` block is empty. All other text `.rsd` contents equal the rpk data bytes (32 system + 340 Axis), including CRLF.
6. Zero-size records may point at EOF (offset == file size; 9 in system.rpk). Bounds must allow `off == size` when `size == 0`; non-empty records require `table_end <= off` and `off+size <= file size`.
7. Both sample data areas are contiguous (no gaps, no overlaps) from `table_end` to EOF, and `table_end = 0x10 + n*0x40 + 16 + blob_size`. Header `x1`/`x2`: system 1/0x20, Axis 0/0x154 (still unused); every sub-package `index` is 1..n in order and `tail` is 0.

Self-check: `rpkdump --selfcheck` opens both pairs, asserts counts/bounds/no overrun, cross-checks `.res`/`.rsd` against the archive, and asserts part `0x000000D2` resolves to the "FL door" part (`.rdb` alias is `FL`; "FL door" is the label from the referenced `FL_door.cfg`).
