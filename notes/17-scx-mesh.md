# 17 — SCX mesh format and loaders

Target: `bin/the game/StreetLegal_Redline.exe` (SLRR 2.3.1 build 940, 32-bit PE, image base 0x400000).
All addresses below are image VA. "observed" = read from shipped code/data; "inferred" = reasoned,
not proven by a sample.

Status: the on-disk format and every loader path that matters for shipped content are decoded.
Versions 1–6 code paths verified; shipped samples exist for 3, 4, 5, 6. v1/v2 have no samples in
this install, so their material-field semantics stay inferred.

## 1. Common header

```
u32 magic       "INVO" = 0x4F564E49 (little-endian)
u32 version     1..6
```

Only the first 8 bytes are common; the rest depends on the container family.

Loader entry points (render):
- `CompatibleMesh_static__vf5` 0x4515e0 — magic/version check, `.bon`, dispatch.
- `FUN_00451b00` 0x451b00 — sequential container (v1/v2/v3/v5).
- `FUN_00452960` 0x452960 — chunked container (v4/v6).

The loader checks `**data != 0x4F564E49` and reports `"Invalid mesh file! -> %s"`
(string 0x598e88; `FUN_00569530`). Version is read from `data+4`. If the global byte
`DAT_007a75e4` is set, version 5 is remapped to 3 and version 6 to 4 **before** dispatch
(so 5 uses the sequential walker with the v3 layout, 6 uses the chunked walker with the v4
layout). `DAT_007a75e4` is set by `Native__util_Steam__getPackage_4e4c10` 0x4e4c10 when the
Steam package query helper `FUN_00564ac0` returns non-null. Final dispatch in
`CompatibleMesh_static__vf5` is `if (3 < version) FUN_00452960(); else FUN_00451b00();`.
Consequence: without the Steam flag a raw v5 file is sent to the chunked walker and misparsed;
a raw v6 file still works because its layout equals v4.

## 2. Sequential container — v1, v2, v3, v5

The file is a chain of self-contained records (one material + one group + one static section).

### 2.1 v3 (and v5) record layout — observed

```
u32 recordSize                 // >= 0x20; normally 0x88; legacy 0x38 and 0x68 exist
u8  materialRecord[recordSize] // body starts at +4; see 2.2
u32 nv
u8  vertices[nv * stride]      // stride from materialRecord+0x3c
u32 nf
u32 indices[nf * 3]
```

The next record starts immediately after the index data. v3/v5 stop when the next `u32` is 0
(the four terminator bytes are consumed); the other versions loop until EOF. There is no
directory, chunk table, or per-file section count.

`stride` is read from the copied material struct at +0x3c. Default is 0x2c. The record decoder
`FUN_004583f0(dest, record)` first applies defaults (`FUN_00458390`), then copies
`min(recordSize, 0x88)` bytes — or `min(u16@record+0x42, 0x88)` when `flags@+0x28` has bit 3
(0x8) — into the 0x88-byte in-memory material. Fields past `recordSize` therefore stay at
their defaults. This is how the legacy 0x38-byte record (`cars/meshes/slider.scx`) still gets
stride 0x2c and texture slot −1: only 0x38 bytes are copied.
`FUN_00458330` resets a material; `FUN_00458390` writes the defaults.

### 2.2 v3 material record — observed

The file record is copied verbatim into the in-memory material, so file offsets = material
offsets. Mapping (also §6.1 of `notes/12-render.md`):

| off | type | meaning |
|---|---|---|
| +0x00 | u32 | record size (copied) |
| +0x04..+0x10 | f32×4 | diffuse RGBA (A < 0.99 → transparent) |
| +0x14..+0x1c | f32×3 | ambient/emissive RGB |
| +0x20 | f32 | ambient scale |
| +0x24 | f32 | specular power |
| +0x28 | u32 | flags; bit0 transparent, bit1 → material flag 0x80000000, bit3 (0x8) = has @0x42 copy cap, 0x40 → MixTA, 0x1000 → MixVCA, (flags&0x101)==0x100 → ColorMixTex, 0x800 alpha-test |
| +0x2c..+0x3a | s16×8 | texture indices; §6.1 names +0x2c base, +0x30 no-specular, +0x32 spheremap, +0x34 second/mix, +0x36/+0x38 bump; +0x2e and +0x3a are unnamed (observed) |
| +0x3c | u32 | vertex stride (default 0x2c) |
| +0x40..+0x44 | s16×3 | UV-set/channel indices; all three 0 → set to −1 |
| +0x46 / +0x48 | s16 | base / second texture UV set; +0x46 clamped to ≤2 |
| +0x5c..+0x64 | f32×3 | specular colour |
| +0x68 | char[32] | material name (embedded, NUL-padded) |

Loader extras: `FUN_004583f0` defaults +0x38 (a texture slot), +0x40/+0x42/+0x44 (UV
channels) and +0x4e to −1, stride to 0x2c, specular to 0, +0x46 to 0; after the copy it
clamps u16@+0x46 to ≤2 and, if +0x40/+0x42/+0x44 are all zero, writes −1 to all three.
Material type selection from these fields is §6.1 of `notes/12-render.md`.
`"_uvw"` + `N`/`F` inside the name at +0x6c adds a UV-transform texture parameter (notes/12).

### 2.3 v3 vertex layout — observed (assembly-verified)

| off | type | field |
|---|---|---|
| +0x00 | f32×3 | position (file cm) |
| +0x0c | f32×3 | normal (not normalised by the loader) |
| +0x18 | f32×2 | UV0 |
| +0x20 | f32×2 | UV1 |
| +0x28 | u32 | diffuse colour |
| +0x2c | u32 | specular override (only when material flags bit 2 = 0x4) |
| +0x30 | f32 | blend weight B (stride > 0x30) |
| +0x34 | f32×2 | UV2 (stride > 0x34; requires stride ≥ 0x3c) |
| +0x3c | f32 | blend weight A (stride > 0x3c) |

Minimum stride is 0x2c. Every shipped v3/v5 file uses 0x2c or 0x40 (one 0x68-record file,
`waterplane.SCX`, uses 0x40).

The sequential loader always builds an engine vertex buffer with the fixed engine FVF
0x15003f = position + normal + diffuse + specular + three 2-float UV sets (engine stride 0x38,
see §7). Material texture records (max 4) select source UV 0/1/2 from +0x18/+0x20/+0x34;
other source indices copy zero. For "lit" materials (material status bit 0x2) the engine
diffuse is 0xff808080 and the engine specular is the vertex colour | 0xff000000; for unlit
materials engine diffuse is the vertex colour | 0xff000000 and specular 0 (notes/12).

### 2.4 v1 and v2 — code-verified, no samples

Both loop record by record to EOF; there is no zero terminator. Headers are fixed, vertices
fixed-stride 0x34, indices are u32 triples.

- v1: header 0x38. Five u16 at +0x00..+0x09 are copied into the material's texture slots;
  floats at +0x14..+0x34 are copied into the material colour/power fields; `nv` u32 at +0x38;
  vertices at +0x3c; `nf` after `nv*0x34`; then `nf*3` u32 indices.
- v2: header 0x34. Five u16 at +0x00..+0x09; floats at +0x0c..+0x30; `nv` at +0x34;
  vertices at +0x38; `nf` after `nv*0x34`; then indices.
- Material-field semantics (which float is diffuse/ambient/specular) are inferred from the v1/v2
  conversion code; the exact v1/v2 vertex field order is not confirmed (no files to check).
  Both are converted into the same 0x88 in-memory material as v3 (`FUN_00458390` defaults).

## 3. Chunked container — v4 and v6

### 3.1 Header and chunk table — observed

```
u32 magic, u32 version
u32 nchunks
{ u32 type, u32 absoluteOffset }[nchunks]     // directory, starts at +12
chunks...                                     // stream starts at 12 + 8*nchunks
```

Each chunk is `{ u32 type; u32 size }` where `size` includes the 8-byte header; `size >= 8`.
Chunks are contiguous and the chain ends exactly at EOF. The loader skips the directory without
reading it; shipped directories mirror the actual chunk types/offsets. There is no per-file
version check inside the walker: any effective version > 3 uses it.

### 3.2 Chunk types — observed

| type | payload | effect |
|---|---|---|
| 0 | material blob (§3.3) | creates a new **group** and its material; payload consumed exactly |
| 1 | u32 reserved (0) | creates a new section, mode 0; size 12 |
| 2 | u32 reserved (0), u32 palette0, u32 palette1 | creates a new section, mode 1; size 20 |
| 3 | u32 reserved (0), u32 count, u32 palette[count] | creates a new section, mode −1 (derived later); size 16+4*count |
| 4 | u32 nv, u32 flags, nv×stride vertex bytes | vertex data for the current section; `stride = (size−16)/nv` |
| 5 | u32 count, u16 index[count] | index data for the current section; `count % 3 == 0`; size 12+2*count |

Section chunks 1/2/3 attach to the most recently created group (type 0). Vertex chunk 4 and
index chunk 5 attach to the most recently created section. A typical file is
`[0,1,4,5]` repeated; type 2/3 appear only for skinned/palette sections (51 type-3 chunks,
3 type-2 chunks in the install). Type-3 palette entries and the type-2 pair are passed to the
section palette vtable slot; their exact meaning (bone indices into the `.bon` skeleton) is
**inferred** — see §5.

Section mode: type 1 sets 0, type 2 sets 1, type 3 sets −1. After the vertex chunk, if the mode
is still −1 it is derived from vertex flags: bit1 (0x2) → 2, bit2 (0x4) → 3, bit3 (0x8) → 4,
bit4 (0x10) → 5, otherwise left −1. Modes 2–5 are **inferred** to be D3D9 VERTEXBLEND codes
(1–4 blend weights); mode getter/setter are section vtable +0x44/+0x40.

### 3.3 Material blob (chunk type 0) — observed

```
u32 type          // material class id, same numbering as notes/12 §6 (0 = Tex, 0x0d = MixTASpecularSpheremap, ...)
u32 flags
u32 nparams
params...
```

Each parameter starts with `u32 id = (decoder << 24) | subtype`. Its total size (including the
4-byte id) is fixed by the decoder:

| decoder | total size | payload |
|---|---|---|
| 0 | 8 | one u32 packed colour 0xAARRGGBB |
| 1 | 8 | one f32 |
| 2 | 0xc | two dwords |
| 3 | 0x10 | three dwords |
| 4 | 0x14 | four dwords |
| 5 | 8 | one dword |
| 6 | 0x20 | seven dwords — texture / UV record |
| 7 | 8 | one dword (no in-memory decode observed) |
| 8 | 0x24 | 32-byte NUL-padded name |
| other | 4 | id only |

Decoded by `FUN_00440fe0` 0x440fe0 into 0x24-byte in-memory slots:

- decoder 0 (colour): slot[1] = raw dword, slot[5] = A, slot[2] = R, slot[3] = G, slot[4] = B,
  each `byte * 1/255` (`FUN_00441140`). Packing order is 0xAARRGGBB, verified: file bytes
  `64 64 64 ff` = 0xff646464 = opaque grey 100.
- decoder 1: slot[1] = f32.
- decoder 6 (texture): after the id, dword0 = textureIndex, dword1 = sourceUV, dword2 = unknown
  (usually 3), dword3 = scaleU, dword4 = scaleV, dword5 = offU, dword6 = offV. In-memory:
  slot[3] = textureIndex, slot[4] = sourceUV, slot[2] = unknown, slot[5..8] = scaleU, scaleV,
  offU, offV, slot[1] = 0.
- decoder 8 (name): slot[0] = id, name (0x1f bytes + NUL) at slot+4.
- decoders 2/3/4/5 copy 1–4 dwords into slots; semantics unknown.
- decoder 7 returns 8 but the decompiled body does not write the slot; semantics unknown.

Observed subtype use: colour 0x00xxxx00 diffuse, 0x00xxxx01 ambient, 0x00xxxx02 specular
(inferred from value patterns and the Color material's own records); float 0x01xxxx00 power,
0x01xxxx01 spheremap strength (inferred); texture 0x06xxxx00 base, 0x06xxxx01 second/mix,
0x06xxxx03 spheremap; 0x06xxxx02 unknown. Shipped v4/v6 blobs use only decoders
0,1,5,6,7,8 (counts in §8).

Every blob is self-delimiting: 12 + Σ(param sizes) == chunk payload length. Validated on all
4,858 shipped blobs with zero mismatches.

### 3.4 Vertex chunk (type 4) — observed

Payload `{u32 nv; u32 flags; nv * stride bytes}` with `stride = (chunkSize−16)/nv`.
Vertex field order and meanings of `flags`:

| bit | field(s) |
|---|---|
| 0x0001 | position f32×3 |
| 0x0004 | one f32 blend weight (W0) |
| 0x0008 | two f32 blend weights (W0, W1) |
| 0x0010 | three f32 blend weights (W0, W1, W2) |
| 0x0020 | one f32 extra beta |
| 0x0040 | normal f32×3 |
| 0x0080 | diffuse u32 |
| 0x0100 | specular u32 |
| 0x0200..0x8000 | one UV pair per set bit, in order; `nuv = popcount(flags & 0xfe00)` |

Only one of 0x4/0x8/0x10 applies. Field order in memory is exactly the table order:
position, blend weight(s), extra beta, normal, diffuse, specular, then `nuv` UV pairs.
Thus `stride == 12*(pos?1:0) + 4(or 8/12 for blend) + 4*(beta) + 12*(normal) + 4*(diffuse)
+ 4*(specular) + 8*nuv`. Both this identity and `popcount(flags & 0xfe00) == nuv` held for
every parsed section (0 mismatches).

Engine custom FVF built by `FUN_00454940`:
- bit0 position; bit1 normal (file 0x40); bit2 diffuse (0x80); bit3 specular (0x100);
- OR 0x300 when any of file bits 0x1c (blend weights); OR 0x800 when bit 0x20 (extra beta);
- OR `nTextureRecords << 4` (material UV records, capped at 4);
- OR `1 << (16 + 2*i)` for each material UV record i (size code 1 = 2 floats).
`FUN_00455210` 0x455210 turns this into a D3D9 vertex declaration (VB descriptor +0x1c, second
declaration +0x64) and the engine stride (+0x60). Sequential sections use the fixed value
0x15003f = the same custom FVF for position+normal+diffuse+specular+3 UV sets (engine stride 0x38).

Per-vertex conversion to the engine vertex is `FUN_00455450` 0x455450, fed from globals:
position ×0.1; blend weights (defaults 1,0,0); extra beta; normal; diffuse **with alpha forced
to 0xff** (`| 0xff000000`); specular raw bits; UVs after the transform in §4. The loader does
not normalise normals.

### 3.5 Index chunk (type 5) — observed

Payload `{u32 count; u16 indices[count]}`; `count` is a multiple of 3, indices must be
`< nv` of the current section (checked by the parser; the loader trusts them). The loader
installs an index-buffer descriptor of `{count, alloc(count*2), elemSize 2, stride 2}` through
section vtable +0x14, sets the mesh maximum index count (`mesh+0x100`), and copies the u16
values through section vtable +0x1c. Sequential files store u32 indices and the same vtable
converts them to u16 (values must fit 16 bits; verified on samples).

## 4. UV records and transform (chunked path) — observed

`material vf2` (0x42c840) hands the loader up to four texture/UV records, each
`{sourceUV, scaleU, scaleV, offU, offV}`. For every vertex and every record:
`pfVar8` points at the first UV pair. The loader computes start offsets
`off[0]=0`, `off[k] = off[k−1] + 8` if UV bit k is present, else `off[k−1]`.
Then:

- `sourceUV == −1` (0xffffffff): planar `u = pos.x`, `v = −pos.z − 0.5`.
- otherwise `u, v = UV pair at byte offset off[sourceUV]`; the file `v` is pre-flipped:
  `v = 0.5 − file_v`.

Final engine UVs:

```
u' = ((u − 0.5) − offU) * scaleU + 0.5
v' = 0.5 − (v − offV) * scaleV
```

With scale 1 / offset 0 the double flip cancels for a normal pair (`u'=u`, `v'=file_v`);
the planar mapping then gives `u'=pos.x`, `v'=pos.z+1` (intent inferred). A negative
`sourceUV` other than −1 (notably −2, 453 shipped records) indexes the offset table out of
bounds; the loader has no bounds check or fallback, so those UVs read unrelated stack data.
This is a loader quirk, not a format feature.

## 5. Skin / bones / `.bon` — partly inferred

- v4/v6 palette chunks (types 2 and 3) hand palette indices to section vtable +0x3c;
  mode is set through +0x40. The indices are inferred to select `.bon` skeleton entries.
- For raw versions ≥4 the render entry point may load a sibling `<mesh>.bon` text file
  (`FUN_00564d70`/`FUN_00565610`) when `mesh+0x3c` bit0 is set and the mesh state flag
  `mesh+0x38` is not 0x20000000. With the Steam remap active, a v5 file remapped to 3 skips
  this path. Shipped v3/v5(sequential) files have no `.bon`.
- `.bon` entries are 0xa4 bytes at `mesh+0x120` (count `mesh+0x11c`): +0x00 id, +0x04 name[32],
  +0x24 4×4 matrix (row 3 translation ×0.1, w = 1.0). Text grammar: `name`, `id`, then four
  lines of `%f,%f,%f`. (Entry layout observed from the loader; text grammar inferred from the
  parsing calls.)
- Finalisation sets `mesh+0x5c` bone count, `mesh+0x124` skinned (`bones < 3 ? 0 : 1`), and on
  each section `+0x48` skinned / `+0x4c` bone count. How the section bone count is derived from
  the palette is not traced (inferred: palette count).

## 6. Units, bounds

- File positions are centimetres.
- Render loader: positions ×0.1, giving the engine render unit (1 unit = 0.1 m). Bounds are
  computed in that same unit: `centre = (min+max)*0.05` (`mesh+0x8c/+0x90/+0x94`),
  `half = |max−min|*0.05` (`+0x98..`), `radius = sqrt(max(|min|²,|max|²))*0.1`
  (`+0x7c..+0x84`); bounds kind `mesh+0x6c = 0`; no vertices → kind 2 with zero bounds.
- Collision loader: positions ×0.01 → metres; AABB and radius computed directly in metres.
- No bounding volumes are stored in the file; they are always recomputed. No tangents exist in
  the format, and no texture filenames are embedded — texture slots are resource indices.
  Material names are embedded (v3: 32 bytes at +0x68; v4/v6: decoder-8 parameter).

## 7. Engine binding — render

- Group object 0x18, linked from `mesh+0x60`; material created from the v3 record through
  renderer vtable +0xd0 (`C3DLow__vf52 createMaterialFromScx` 0x42ba10), or from a v4/v6 blob
  through renderer vtable +0xcc.
- `CM_Section_static`, size 0x54, vtable 0x598ecc. Draw sub-object at `section+0x24`:
  `+0x00` VB descriptor ptr, `+0x0c` index-buffer holder, `+0x10` u16 NumVertices,
  `+0x12` u16 PrimitiveCount, `+0x14` s16 BaseVertexIndex, `+0x16` s16 StartIndex.
- VB descriptor (0x68): `+0x04` vertex count, `+0x08` vertex data, `+0x0c` custom FVF source,
  `+0x1c` D3D9 vertex declaration, `+0x60` engine stride, `+0x64` secondary declaration.
- `vf46 drawSection` 0x443ea0: SetFVF, SetStreamSource, SetIndices, then
  `DrawIndexedPrimitive(TRIANGLELIST, BaseVertexIndex, 0, NumVertices, StartIndex, PrimitiveCount)`
  (exact argument order in notes/12 §4.3).
- Per-record finalisation sets `mesh+0x38 = (flags & ~2) | 0x14`, `mesh+0x4c` frame, and the
  material flag `0x80000000` when the mesh path contains `objects\meshes` (sequential path).
- v3 sequential path allocates group + section + material per record; the engine FVF is fixed
  0x15003f and vertex conversion uses the `FUN_00455450` writer.

## 8. Collision binding — shape kind 6 (`.scx` phys mesh)

`Shape__loadScx` 0x4f5b90 reads the file (missing → `"Fatal! Missing mesh file:\r\n %s"`,
0x5a71df+; string at 0x5a7200 is `"Fatal! Invalid mesh file:\r\n %s"`). It does **not** check
the "INVO" magic; it reads the version at +4 and accepts:

| version | without `DAT_007a75e4` | with flag |
|---|---|---|
| 0 | invalid | invalid |
| 1–3 | sequential | sequential |
| 4 | chunked | chunked |
| 5 | rejected | remapped to 3, sequential |
| 6 | rejected | remapped to 4, chunked |

Sequential: v1 header 0x38/stride 0x34; v2 header 0x34/stride 0x34; v3 header = u32 record size,
stride = +0x3c (or 0x2c if the header size is exactly 0x38), positions at header+4 ×0.01,
`nf` after `nv*stride`, then `nf*3` u32 indices. Chunked: skips the directory; type 4 gives
`nv`, `flags`, positions ×0.01; type 5 gives `nf` triangles as `{3, i0+base, i1+base, i2+base}`
in 0x20-byte face records; types 1/2/3 are ignored. `verts < 1` or `faces < 1` →
`"Fatal! Invalid mesh file:\r\n %s"`.

Post-pass: per-face normal = normalised cross product (zero-length left zero) at face+0x14;
AABB half extents at `shape+0x14/+0x18/+0x1c`, centre at `+0x40/+0x44/+0x48`,
`radius = sqrt(max|v|²) + 0.05` at `+0xc`. The resulting polyhedron is the V-Clip feature shape.
Sample `cars/racers/Axis_data/meshes/_phys_FL_door.scx`: 16 vertices, 28 triangles.
The cfg `body` slot (0x20) links the part to this type-6 mesh (notes/11, notes/13).

`CompatibleMesh_ground__vf5` 0x451130 (ground meshes) uses the same magic check
(`"Invalid mesh file!"` 0x598eb4) and only the **sequential** walker — v4/v6 ground meshes would
not load correctly. It creates a 0x280-byte ground section, skips vertex/index payloads by
pointer arithmetic, and adds a projected-shadow group/material (0xffffff) with FVF 3.

## 9. Cross-check against shipped content

Scratch parser under `/tmp/scxwork/` (not in the repo): `parse.py` mirrors the loader
(record walk, chunk walk, directory skip, material blob consumption, per-vertex field parse,
index range checks, exact EOF) and `corr.py` correlates material UV records with section
UV counts.

Inventory under `bin/the game` (7,451 `.scx` with INVO magic):

| version | files | parsed OK | notes |
|---|---|---|---|
| 3 | 3,532 | 3,532 | record 0x88 ×11,186, legacy 0x38 ×28, 0x68 ×1; stride 64 ×11,187, 44 ×28; 2 zero-vertex sections; one file with 548 sections |
| 4 | 2,500 | 2,495 | 4,325 vertex + 4,325 index chunks; 4,175 material chunks; 4,271 sections; chunk types 2 ×3, 3 ×51 |
| 5 | 775 | 775 | all v3 layout: 1,829 sections, every record 0x88/stride 64; only under `dlc/redline_cars_remake_pack` |
| 6 | 644 | 644 | chunked; 643 material chunks; flags 0x41 ×637 (pos+normal, stride 24), 0x241 ×6 (stride 32), 0x1 ×1 (stride 12) |

v4 section details: flags 0x241 ×1,622, 0x3c1 ×1,415, 0x41 ×970, 0x1 ×115, 0x7c1 ×66,
0xa41 ×55, 0x265 ×26, 0x263 ×20, 0x1c1 ×10, and 9 rarer values; UV sets 0 ×1,095,
1 ×3,096, 2 ×130, 3 ×4; strides 32 ×1,636, 40 ×1,501, 24 ×970, 12 ×115, 48 ×69, 36 ×23,
44 ×7, 56 ×4. Normals are frequently not unit length (up to 88 per file) — the loader does not
normalise.

The five v4 parse failures are structurally valid, genuine **NaN UV** assets — the loader never
validates UV finiteness:

- `misc/garage/g2/garazs2_targyak.scx` (UV NaN, section @393270, vertex 9431)
- `misc/garage/g3/garazs3targyak.scx` (UV NaN, @110080, vertex 34)
- `objects/meshes/area_41_egyedi_b/area_41_egyedi_b.scx` (UV NaN, @11078, vertex 33)
- `objects/meshes/shop_01_egyedi/shop_01_egyedi.scx` (UV NaN, @3688, vertex 18)
- `objects/meshes/area_41_egyedi/area_41_egyedi.scx` (UV NaN, @26202, vertex 33)

One command also found `modding_tools/Map tools/maps/sample/meshes/sample.scx` as 0 bytes —
not a mesh, not counted. The sole v6 file with no material chunk
(`dlc/.../Baiern_data/meshes/_phys_F_bumper_CR.scx`, 292 B, chunks 1,4,5) is a collision-only
asset; the render loader would build a group-less section, so it is not rendered (inferred).

Material blobs: 4,858 decoded, 12+Σparams consumed exactly in every one. Parameter decoder
counts: 8 ×3,668, 0 ×5,167, 1 ×2,240, 6 ×5,163, 5 ×10, 7 ×2. Texture-record `sourceUV`:
0 ×4,585, −2 ×453, 2 ×68, 1 ×92. Section FVF/stride identity held for every parsed section.

Largest-mesh sanity checks (parser, loader-exact):

- v3 `multibot/maps/UKRallyStage1/Meshes/UKRallyStage1.SCX`: 36,696,236 B, 554,244 verts,
  184,748 tris, 53 sections; UKRallyStage3 30,208,764 B / 444,132 verts / 148,044 tris;
  LagunaSeca 27,759,540 B / 408,078 verts / 136,026 tris.
- v5 largest `dlc/.../SuperDuty_data/meshes/chassis.scx`: 5,635,956 B, 82,854 verts,
  27,618 tris, 13 sections; Duhen chassis 80,295 verts/26,765 tris; Prime chassis
  71,376 verts/23,792 tris.
- v4 largest `misc/garage/g4/roc_g_egyeb.SCX`: 1,356,194 B, 32,218 verts, 25,631 tris,
  36 sections; `parts/wings/meshes/SL_Tuners_Euro.scx` 17,746 verts/28,886 tris.
- v6 largest mesh `Enula_data/meshes/dual_cold_air_intake.scx`: 236 verts/254 tris;
  intercooler 160 verts/188 tris (the DLC v6 meshes are small).

Concrete v3 sample (`cars/racers/Axis_data/meshes/FL_door.scx`, v3, 13,560 B): first record
@0x08 size 0x88, stride 64, nv 129 @0x90, vertices @0x94, nf 43 @0x20d4, indices @0x20d8,
second record @0x22dc, terminator 0 @13556, exact EOF. Material diffuse ~0.314 grey,
ambient 0.392, ambient scale 2.0, power 16.2, flags 0x40, texture slots {0,−1,−1,2,1,−1,−1,0},
name "ajto kulso".
Concrete v4 sample (`parts/running_gear/meshes/RL_control_arm.scx`, 4,312 B): directory
{(0,0x2c),(1,0x8c),(4,0x98),(5,0xe68)}; type-4 size 0xdd0, nv 110, flags 0x241, stride 32;
type-5 306 u16 indices (102 tris); material type 0, flags 0, 3 params: name "running gear",
grey 150, texture record (index 0, sourceUV 0, unknown 3, scale 1/1, offsets 0/0).
Concrete v4 spheremap sample (`cars/racers/Duhen_data/meshes/FR_quarterpanel.scx`):
material type 0x0d, 8 params: name "F quarterpanel", color 0xff646464, ambient 0xffffffff,
power 21.0, three texture records (base 0, second index 1, spheremap index 2) and sphere
strength 0.3; payload consumed exactly.

## 10. Remaining unknowns / inferred

- v1/v2: no samples; material-field semantics and vertex field order inferred from the
  conversion code.
- `sourceUV == −2` (453 records): out-of-bounds offset-table read in the loader; intended
  meaning unknown.
- Texture-record dword2 (in-memory slot[2], observed value 3): unknown.
- Decoder 7 (2 records) and decoders 2/3/4 (no shipped v4/v6 samples): semantics inferred from
  the decoder switch only.
- Material subtypes for colour/float/texture beyond the observed ones, and material type ids
  0x38/0x39, are unknown.
- Palette indices (chunk types 2/3) → `.bon` bone binding and the source of `mesh+0x5c` bone
  count are not fully traced; modes 2–5 are inferred to be D3D9 VERTEXBLEND codes.
- The `.bon` text grammar is inferred from the loader's parsing calls; entry layout is observed.
- The `DAT_005ef210` flag set when a mesh path contains "fonts" (`CompatibleMesh_static__vf5`)
  has no identified consumer; the `.scy` last-character test (`local_8c = 0x400`) was observed
  but its effect is not decoded.
- The SCX writer `FUN_00450f00` (saveScx, 0x450f00) was not reviewed — this note describes the
  reader only.
- Exact renderer vtable address of `createMaterialFromBlob` (slot +0xcc) not recorded; slot
  number is observed, class address not.

## 11. Address index

| address | name/role |
|---|---|
| 0x4515e0 | `CompatibleMesh_static__vf5` render entry (magic/version/.bon/dispatch) |
| 0x451b00 | sequential render loader (v1/v2/v3/v5) |
| 0x452960 | chunked render loader (v4/v6) |
| 0x454940 | vertex chunk decode + engine vertex build (v4/v6) |
| 0x4547e0 | index chunk decode |
| 0x455210 | FVF → D3D9 declaration builder |
| 0x455450 | engine vertex writer |
| 0x4583f0 | v3 material decode (defaults + min(size,0x88) copy) |
| 0x458390 / 0x458330 | material defaults / reset |
| 0x42ba10 | `C3DLow__vf52 createMaterialFromScx` |
| 0x42c840 | `CMaterial_Tex__vf2` (UV record list) |
| 0x440fe0 | material blob parameter decoder |
| 0x441140 / 0x4411c0 | colour unpack / pack |
| 0x441cd0 / 0x441c90 | texture record copy / planar UV record |
| 0x443ea0 | `vf46 drawSection` |
| 0x4f5b90 | `Shape__loadScx` (collision, shape kind 6) |
| 0x451130 | `CompatibleMesh_ground__vf5` (ground/shadow, sequential only) |
| 0x4e4c10 | `Native__util_Steam__getPackage` (sets `DAT_007a75e4`) |
| 0x450f00 | SCX writer (not reviewed) |

Strings: 0x598e88 `"Invalid mesh file! -> %s"`; 0x598eb4 `"Invalid mesh file!"`;
0x5a7200 `"Fatal! Invalid mesh file:\r\n %s"`; missing-file message starts at 0x5a71df
(`"Fatal! Missing mesh file:\r\n %s"`, leading byte interpreted as adjacent string data).

## Corrections verified by the C++ loader

The `src/mesh/scx.*` reference parser was checked against all 7,451 shipped non-empty
`.scx` files (v3 3,532; v4 2,500; v5 775; v6 644). It corrects or pins the following
points above:

1. **Sequential vertex fields (§2.3).** `+0x2c` and `+0x30` are blend weights W0/W1
   (`DAT_005ef1a8`/`DAT_005ef1ac`), not the specular override; `+0x3c` is the specular
   override and is read only when material flags `& 4`. The note's §2.3 assignment of
   `+0x2c` to specular and `+0x3c` to blend weight A is swapped.
2. **Sequential EOF.** A file may end exactly at EOF with no zero terminator; both the
   terminator and exact-EOF endings are valid, and any bytes after the terminator are an
   error. Accepting EOF recovers 32 shipped files (v3 totals: 3,532 files, 11,215
   sections, 9,549,639 vertices, 3,744,926 triangles).
3. **Sequential indices stay u32.** The exe writes indices through a u16 buffer
   (truncating values >0xffff; three huge maps contain 0x10000), but the canonical mesh
   keeps the on-disk u32 values without range rejection.
4. **v1/v2 colours are inferred.** No v1/v2 samples ship. Vertex RGB floats are packed as
   `AARRGGBB` with `x255` truncation and alpha from the material diffuse, with no
   lit-material multiply; this is inferred from the assembly paths, not observed in files.
5. **Non-finite UVs are wider than v4.** Fourteen shipped files contain NaN/inf UVs (v3
   and v5 too, not only the five known-bad v4 assets); all are structurally valid and the
   loader does not reject them. The selfcheck's known-bad set is restricted to the five
   v4 files to keep the expected 2,495 + 5 split.
6. **Empty sample.** `modding_tools/Map tools/maps/sample/meshes/sample.scx` is 0 bytes and
   is skipped by the selfcheck, not counted or treated as a malformed mesh.

