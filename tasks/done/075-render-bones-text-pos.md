---
id: 075
status: merged
branch: card/075
depends: []
---
# render/script: render-instance bone table; Text.setPos/getPos + RenderRef.getBoneId

## Goal
Replace the INVENTED raw `Runtime::Resource::text_pos` with the exe's bone model:
a per-instance list of named bones (sorted by name, case-insensitive, ids made by
midpoint), each with a 4×4 local matrix. Port `Text.setPos(FF)V`, `Text.getPos()Vector3`,
`Text.getPos(Vector3)Vector3` and `RenderRef.getBoneId(String)I` on top of it, and make
`tx_create` (card 067) write bone00 instead of `text_pos`. Full trace: notes/12 §9
"Render-instance bones". Do **not** touch the GfxEngine natives (cards 068/069 are open in
the same file) or any other RenderRef row (setMatrix etc. stay stubs).

## Exe evidence (cleaned pseudocode; all ECX plumbing verified in assembly)
Mapping to src: the exe's script handle `{+8 id, +0xc RI}` = `Runtime::Handle` (`h->id`,
`h->target`). The exe's typed block `T = RI->vf[+0xc]` (via `Resource__getTyped` 0x51f4f0)
has no src layer: as in cards 062/067 the bound `Runtime::Resource` stands in for T
(`INVENTED:` mapping). Residency calls (`RI+0x7c != 1 → vf[+0x14]`) have no src
counterpart (skip). Constants: `0x41200000` = 10.0f, `0x3dcccccd` = 0.1f, `0xffff`, `0x8000`.

### 0x51b940 RenderInstData__boneId(name) — ECX = T; returns id, creates on demand
    lo = 1; hi = 0xffff; n = first bone (T+0x1f4); before = null;
    if (list empty) goto make;                       // first->next == 0
    loop:
        c = _stricmp(n->name, name);                 // 0x57c11e, case-insensitive
        if (c == 0) return n->id;                    // found
        if (c > 0) { hi = n->id; before = n; goto make; }   // insert before n
        lo = n->id; n = n->next;
        if (n is the end) { hi = 0xffff; before = null; goto make; }  // append
        goto loop;
    make:
        id = (uint32)(lo + hi) >> 1;                 // no collision check
        node = new RenderController(id, name);       // 0x51b050; alloc fail -> return 0
        insert node before `before` (or at the tail when before == null);
        T+0x16c |= 0x1800;
        return id;
First bone of an instance gets id 0x8000. `_stricmp` = MSVC C-locale: compare
`tolower(a[i])` vs `tolower(b[i])` as unsigned chars, result = difference at the first
mismatch (or at the terminating NUL).

### 0x51b050 RenderController ctor(id, name) — the fields this card needs
    +0x44 id = id;
    +0x24 name = first 31 chars of name + NUL     // Str__copyN 0x528230(dst, src, 0x20)
    +0x64..+0xa0 local matrix = identity          // Matrix4__identity 0x422070
(Other fields: `+0x48 = -1`, `+0x4c = 2`, `+0x54..+0x60` bone_ref handle = 0, `+0xbc = 0`,
`+0xc0` second matrix identity, `+0x100 = 1` — not modelled, see Changes.)
Note: the stored name is truncated but the lookup compares it with the full argument, so a
name of ≥32 chars never matches again and makes a new bone each call. Port that as is.

### 0x51c450 RenderInstData__findBone(id) — ECX = T
    for (n = first; n is not the end; n = n->next) {
        if (n->id == id) return n;
        if (n->id > id) return 0;                  // ids ascend in list order
    }
    return 0;

### 0x4e77f0 RenderInst__getBoneId(name) — ECX = handle
    RI = handle->+0xc;                             // no handle id (+8) check here
    if (RI == 0) return 0;
    (make resident; typed T; T == 0 -> return 0)
    return RenderInstData__boneId(T, name);

### 0x4ec470 RenderInst__setBoneMatrix(id, boneRef, pos, rot) — ECX = handle
    if (handle->+8 == 0) return 0;
    RI = handle->+0xc; if (RI == 0) return 1;     // (typed fail -> return 1 too)
    n = findBone(T, id);
    if (n) {
        if (rot && pos) {
            m = Matrix4__fromRotPosX10(rot, pos);  // 0x567570, see below
            n->local = m;                           // 0x4ec0f0 -> 0x4ec160 (+ dirty list, +0x16c |= 0x1800)
        }
        RenderInst__setBoneRef(handle, T, n, boneRef);   // 0x4e84c0; boneRef = 0 here -> no-op
    }
    T+0x1c0 = DAT_005dadbc;                        // frame counter
    return 1;
0x567570 `Matrix4__fromRotPosX10(rot, pos)`: `m[0..2] = rot[0],rot[4],rot[8]`;
`m[4..6] = rot[1],rot[5],rot[9]`; `m[8..10] = rot[2],rot[6],rot[10]`;
`m[3] = m[7] = m[11] = 0`; `m[12] = pos[0]*10.0f; m[13] = pos[1]*10.0f; m[14] = pos[2]*10.0f`; `m[15] = 1.0f`.
All callers in this card pass `rot` = `Matrix4__setRotIdentity` 0x5663c0 (3×3 identity),
so `m` = identity rotation + translation `pos × 10`.

### 0x4ea7f0 RenderInst__getBoneMatrix(id, outRef, outMat) — ECX = handle
    if (handle->+8 == 0) return 0;
    RI = handle->+0xc; if (RI == 0) return 1;     // exe quirk: outMat left untouched
    n = findBone(T, id); if (n == 0) return 0;
    if (outRef) outRef := n's bone_ref;            // always 0 in this card
    outMat[0..15] = n->local;
    return 1;

### 0x4e5d00 Text.setPos(FF)V
    getArgs(ctx, &self, &x, &y);
    h = renderinst(self);                          // 0x4ecfc0 -> ESI, then MOV ECX,ESI
    rot = 3x3 identity;  pos = { x, y, 0.0f };
    RenderInst__setBoneMatrix(h, RenderInst__getBoneId(h, "bone00"), 0, &pos, rot);

### 0x4e58e0 Text.getPos()Ljava.lang.Vector3;
    getArgs(ctx, &self, &tmp, &tmp);               // 3 out slots, only self used
    h = renderinst(self);
    if (!RenderInst__getBoneMatrix(h, RenderInst__getBoneId(h, "bone00"), 0, m)) return null;
    v = new java.lang.Vector3;
    v.x = m[12] * 0.1f; v.y = m[13] * 0.1f; v.z = m[14] * 0.1f;   // MULSS [0x5935d4]
    return v;

### 0x4e5a20 Text.getPos(Ljava.lang.Vector3;)Ljava.lang.Vector3;
    getArgs(ctx, &self, &v);
    h = renderinst(self);
    if (!RenderInst__getBoneMatrix(h, RenderInst__getBoneId(h, "bone00"), 0, m)) return null;
    v.x = m[12] * 0.1f; v.y = m[13] * 0.1f; v.z = m[14] * 0.1f;   // no null check on v
    return v;

### 0x4e2290 RenderRef.getBoneId(Ljava.lang.String;)I
    getArgs(ctx, &self, &name);
    h = Native__instanceToHandle(self);            // 0x4ecc70
    if (h) return RenderInst__getBoneId(h, name);  // ECX = h
    print "vm_renderref_getboneid: scripted resource " + "%d"(h) + " not found in file \"" + file + "\" at line " + line;
    return 0;

### 0x4e8f40 Text__create — the bone part (rest is card 067)
    rot = 3x3 identity;                             // first statement
    ... createInstance("r_text", 0 bones); bind(h) ...
    RenderInst__setBoneMatrix(h, RenderInst__getBoneId(h, "bone00"), 0, &v3 /*(x,y,0)*/, rot);

Round trip: `setPos(x,y)` stores `(10x, 10y, 0)`, `getPos()` returns `(x, y, 0)`.

## Changes
1. `src/game/runtime.h`, `Runtime::Resource`: delete the line
   `float text_pos[3] = {};            // INVENTED: bone00 matrix translation, raw (x,y,0)`
   and add in its place:
   ```cpp
   // Render-instance bone (RenderController, 0x51b050): typed-data bone list at
   // T+0x1f4, kept sorted by name (_stricmp). INVENTED: lives on the bound Resource.
   struct Bone {
       uint32_t id = 0;        // +0x44
       std::string name;       // +0x24, at most 31 chars
       float local[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};  // +0x64..+0xa0
   };
   std::vector<Bone> bones;    // list order = name order
   ```
   (`<vector>`/`<string>` are already included there; check.)
2. `src/script/natives.cpp`, after `text_renderinst` (~line 5027), add:
   - `int bone_stricmp(const std::string &a, const std::string &b)` — the MSVC
     `_stricmp` above (ASCII tolower, unsigned chars, difference at first mismatch/NUL).
     `str_ieq` (~line 688) is equality only; do not change it.
   - `uint32_t bone_id(Runtime::Resource *t, const std::string &name)` — 0x51b940 exactly as
     above (walk `t->bones` in order, `lo/hi`, insert at index; stored name =
     `name.substr(0, 31)`). Comment `// 0x51b940 RenderInstData__boneId`.
   - `Runtime::Resource::Bone *find_bone(Runtime::Resource *t, uint32_t id)` — 0x51c450.
   - `uint32_t ri_get_bone_id(Runtime::Handle *h, const std::string &name)` — 0x4e77f0:
     `return (h && h->target) ? bone_id(h->target, name) : 0;` (`h == null`: INVENTED
     safety; the exe dereferences it.)
   - `void ri_set_bone_pos(Runtime::Handle *h, uint32_t id, const float pos[3])` — 0x4ec470
     with identity rot: `if (!h || h->id == 0 || !h->target) return;`
     `if (auto *b = find_bone(h->target, id))` set `local` to identity with
     `local[12..14] = pos[0..2] * 10.0f` (and `local[15] = 1`).
     `shortcut:` comments for the unmodelled parts: bone_ref (0x4e84c0, always 0 here), the
     dirty list/`+0x16c |= 0x1800`, `n+0x4c` identity bit, `n+0x100`, `T+0x1c0 = DAT_005dadbc`.
   - `bool ri_get_bone_matrix(Runtime::Handle *h, uint32_t id, float out[16])` — 0x4ea7f0:
     `!h || h->id == 0` → false; `!h->target` → false (`INVENTED:` the exe returns 1 with
     `out` uninitialised); `find_bone` null → false; else copy `local` → true.
3. `tx_create`: replace the three `r->text_pos[...] = ...;` lines with
   `const float p[3] = {x, y, 0.0f};`
   `ri_set_bone_pos(h, ri_get_bone_id(h, "bone00"), p);  // 0x4e8f40 bone00`
   (after `bind`, so `h->target == r`). Update its comment block: drop "the bone00 write"
   from the `shortcut:` and the `FUN_004e77f0/FUN_004ec470 (bone00)` UNRESOLVED entry.
4. New natives (place after `tx_update`), each with `// 0xADDR java...` comment:
   - `tx_set_pos` 0x4e5d00: `need_rt`, `receiver_object`, `want(...,3)`, `get_f(a[1])`,
     `get_f(a[2])`; `h = text_renderinst(vm, o)`; `float p[3] = {x, y, 0.0f};`
     `ri_set_bone_pos(h, ri_get_bone_id(h, "bone00"), p);` return null.
   - `tx_get_pos` 0x4e58e0: `need_rt`, `receiver_object`; `h = text_renderinst`; `float m[16];`
     if `!ri_get_bone_matrix(h, ri_get_bone_id(h, "bone00"), m)` → null; else
     `float v[3] = {m[12] * 0.1f, m[13] * 0.1f, m[14] * 0.1f};` and
     `new_point(vm, "java.lang.Vector3", v, false, err)` (null on failure → `return false`).
   - `tx_get_pos_into` 0x4e5a20: as above, but `want(...,2)`; the target is `a[1]`: if it is
     not an object → `*out = null` (`INVENTED:` the exe faults on null); else
     `vec_write(vm, obj, v, err)` and `*out = a[1]`.
   - `rr_get_bone_id` 0x4e2290: `want(...,2)`, `get_o(a[0])`, `get_s(a[1])`;
     `h = game_rt()->handle(o)`; `!h` → `std::fprintf(stderr, "vm_renderref_getboneid: scripted resource not found\n")`
     (`INVENTED:` handle number/file/line omitted, same as `rr_*` at ~line 1382) and return int 0;
     else `Value::make_int((int32_t)ri_get_bone_id(h, *name))`.
5. Registry rows (`"java.render.Text", "setPos"` / both `"getPos"`, ~lines 5592/5593/5596;
   `"java.util.resource.RenderRef", "getBoneId"` ~line 5700): point at the new functions,
   `engine = false`. Leave every other stub row alone.
6. `grep -rn text_pos src/` must be empty afterwards.

## Files
- edit: `src/game/runtime.h` (`Runtime::Resource::Bone`, `bones`; remove `text_pos`)
- edit: `src/script/natives.cpp` (helpers, `tx_create` bone write, 4 natives, 4 registry rows)
- read only: notes/12-render.md §9 "Render-instance bones"

## Done when
- every new function carries its exe address comment; the Resource mapping is `INVENTED:`,
  unmodelled exe work is `shortcut:`
- `text_pos` is gone; `Text.create` makes bone00 (id 0x8000) with translation `(10x,10y,0)`;
  `setPos(x,y)` then `getPos()` gives `(x·10·0.1f, y·10·0.1f, 0)`; `getPos` on an unbound
  `renderinst` returns null
- `getBoneId("a")`, `getBoneId("b")`, `getBoneId("A")` on one instance return 0x8000,
  0xbfff, 0x8000 (that is 0x51b940's arithmetic, `(0x8000+0xffff)>>1`); this is a check
  you do by reading your code against the pseudocode, **not** a unit test to add
- `make -C src clean check` green; no new unit tests or hand-made expected values (CLAUDE.md)
- card frontmatter: status implemented, branch name

## UNRESOLVED
- `RenderInst__setBoneRef` 0x4e84c0 / `RenderInstData__linkBoneRef` 0x4e8460 /
  `FUN_0051aa30` / `FUN_0047df80` (bone_ref attach, used by `RenderRef.setMatrix(int, ResourceRef, …)`)
  are only outlined in notes/12; not needed here because every caller in this card passes 0.
- The dirty list (`T+0x260`), the second node matrix `+0xc0` and `T+0x1c0` consumers are
  untraced (render side).
- `Render__createInstance` with bone count N pre-creates `bone00..` from an uninitialised
  stack matrix in the decompile (`local_c4`); src's `Runtime::instantiate` makes no bones
  (Text passes 0, so no effect here).
- `getArgs` conversion of a `String` argument to the `char*` that 0x51b940 compares is
  taken from the other natives' convention (notes/06), not re-derived; a null String is
  rejected by `get_s` (the exe would hand NULL to `_stricmp`, which returns 0x7fffffff).
