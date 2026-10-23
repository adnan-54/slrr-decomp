---
id: 077
status: implemented
branch: card/077
depends: [075]
---
# render/script: RenderRef.create + both setMatrix overloads

## Goal
Implement the three RenderRef natives that drive render-instance creation and bone
placement: `create(ResourceRef parent, RenderRef type, String name)V` 0x4e2140,
`setMatrix(Vector3 pos, Ypr ori)V` 0x4e2c80 and
`setMatrix(int bone, ResourceRef boneRef, Vector3 pos, Ypr ori)V` 0x4e2a50.
They use the bone table from card 075 (`Runtime::Resource::Bone`, `bones`,
`find_bone`, `ri_get_bone_id`, `bone_id`); 075 must land first. Full trace:
notes/12 §9 "RenderRef", "Render-instance bones". Do **not** change
`RenderRef.getBoneId` (card 075) or any other RenderRef stub row.

## Exe evidence (cleaned pseudocode; ECX plumbing checked in assembly)
Mapping to src (same as cards 062/067/075): the exe's script handle
`{+8 id, +0xc RI}` = `Runtime::Handle` (`h->id`, `h->target`); `T = RI->vf[+0xc]`
(typed render-instance data) has no src layer, so the bound
`Runtime::Resource` stands in for T (`INVENTED:` mapping). Residency
(`RI+0x7c != 1 -> vf[+0x14]`) and the `0x51f4f0` typed fetch are skipped: src has
no typed layer that can fail. Constants: `0x41200000` = 10.0f.

### 0x4e2140 java.util.resource.RenderRef.create(ResourceRef,RenderRef,String)V
    getArgs(ctx, &self, &parent, &type, &name);        // asm: slots -4,-8,-0xc,-0x10
    h = Native__instanceToHandle(self);                // 0x4ecc70; ECX later
    if (h == 0 || parent == 0 || type == 0)
        log "vm_renderref_create: scripted resource %d not found in file ... line ...";
    else if (parent->+0xc != 0) {                      // parent handle unbound -> silent return
        RI = Render__createInstance(parent, type, name, 0);   // 0x524840, boneCount 0
        Native__bind(h, RI);                           // 0x40cac0, ECX = h
        RenderInst__getBoneId(h, "bone00");            // 0x4e77f0, ECX = h; creates id 0x8000
    }
Assembly: `MOV ECX,ESI; PUSH EAX; CALL 0x40cac0` then
`PUSH 0x5a2658 ("bone00"); MOV ECX,ESI; CALL 0x4e77f0`. The String argument
reaches 0x524840 as a `char*`; Java null -> NULL -> the default name
"_renderinst". `getArgs` order (self, parent, type, name) is confirmed by the
push slots in `0x4e2140`; parent is `[EBP-0x8]`, type `[EBP-0xc]`, name
`[EBP-0x10]`.

### 0x524840 Render__createInstance(parentHandle, typeHandle, name, boneCount) — used part
    descriptor type = 3 (ins_render);
    name default "_renderinst" when name == 0, else copy <= 31 chars (0x569340);
    create the instance under the parent, RI+0x7c = 3;
    RI+0x84 |= 0x200;  DAT_00773ad0++;  RI+0x98 = 1.0f;   // +0x98 unmodelled
    set type / script class (FUN_0051e090 approximates instantiate's type arg);
    bounds copied from type data;
    instance kind RI+0x11c from the type's resource type
        (6 -> 6, 0xc -> 1, 0xd -> 2, 0xe -> 3 + scene registration, 0x14 -> 5);
    bone loop over boneCount (0 here, so no bones): 0x51b940/0x51c450 per name.
So create always yields RI+0x7c = 3 (the value `RenderRef.getTypeID` checks).

### 0x4ec540 RenderInst__setBoneMatrixYpr(id, boneRef, pos*, ypr*) — ECX = handle
    if (handle->+8 == 0) return 0;
    RI = handle->+0xc;
    if (RI != 0) {
        (make resident; typed T = RI->vf[+0xc](1.0f); T == 0 -> nothing)
        n = RenderInstData__findBone(T, id);           // 0x51c450
        if (n) {
            rot = (ypr ? 0x5666d0(ypr) : 0x5663c0());  // 3x3 euler matrix / identity
            if (pos) {                                 // always non-null from both natives
                m = Matrix4__fromRotPosX10(rot, pos);  // 0x567570
                RenderInstData__setBoneLocal(T, n, m); // 0x4ec0f0
            }
            RenderInst__setBoneRef(handle, T, n, boneRef);   // 0x4e84c0, untraced
        }
        T->+0x1c0 = DAT_005dadbc;                      // frame counter
    }
    return 1;
`0x4ec0f0` = `0x4ec160` (copy 16 floats to `n+0x64..+0xa0`, `n+0x100 = 1`;
`n+0x4c` bit 1 = matrix is identity) + move n to the dirty-list tail
(`T+0x260`/`T+0x270`) + `T+0x16c |= 0x1800`. Note the return: 0 only when
`handle+8 == 0`; every other path returns 1 (typed-fetch failure included).

### 0x5666d0 Euler {y,p,r} -> 3x3 (ECX = out, param = in)
`fVar1..6` are read as `sin(a0), cos(a0), sin(a1), cos(a1), sin(a2), cos(a2)`:
`0x570fcb` calls `0x57e440`, whose body is `fsin`; `0x570e7e` calls `0x57e3f0`,
whose body is `fcos`. With `sy,cy` for a0, `sp,cp` for a1, `sr,cr` for a2:
```
m[0] = cy*cr + sy*sp*sr      m[1] = sy*sp*cr - cy*sr     m[2]  = cp*sy
m[4] = cp*sr                 m[5] = cp*cr                m[6]  = -sp
m[8] = sr*sp*cy - cr*sy      m[9] = sp*cy*cr + sr*sy     m[10] = cp*cy
```
`m[3], m[7], m[11]` are not written (src already has this function:
`euler_to_mat` in `src/engine/math.h`, whose comment asks to verify the
sin/cos mapping — the addresses above answer it).

### 0x567570 Matrix4__fromRotPosX10(rot, pos) — ECX = out
    out[0]=rot[0]; out[1]=rot[4]; out[2]=rot[8];  out[3]=0;
    out[4]=rot[1]; out[5]=rot[5]; out[6]=rot[9];  out[7]=0;
    out[8]=rot[2]; out[9]=rot[6]; out[10]=rot[10]; out[11]=0;
    out[12]=pos[0]*10.0f; out[13]=pos[1]*10.0f; out[14]=pos[2]*10.0f;
    out[15]=1.0f;
i.e. the 3x3 is **transposed** (`mat_transpose` 0x567380 does the same 3x3
mapping) and the translation is scaled by 10.

### 0x4e2c80 java.util.render.RenderRef.setMatrix(Vector3,Ypr)V
    getArgs(ctx, &self=-0x18, &pos=-0x4, &ypr=-0x8);
    h = Native__instanceToHandle(self);
    if (h != 0) {
        pos = {0,0,0}; ypr = {0,0,0};
        if (posObj)  { pos.x  = Instance__getFloatField(posObj, "x"); ... "y","z"; }
        if (yprObj)  { ypr.v  = Instance__getFloatField(yprObj, "y"); ... "p","r"; }
        Ypr__set3(&stackYpr, ypr.y, ypr.p, ypr.r);     // 0x569b40 (a 3-float store)
        RenderInst__setBoneMatrixYpr(RenderInst__getBoneId(h, "bone00"), 0,
                                     &pos, &stackYpr); // ECX = h for both calls
    } else log "vm_renderref_setmatrix_d: scripted resource %d not found ...";
Field-name bytes: pos 0x5a29f4 "x", 0x5a2a10 "y", 0x5a2a1c "z"; ypr 0x5a2a50
"y", 0x5a2a60 "p", 0x5a2a7c "r"; bone name "bone00" 0x5a2a8c.
`Instance__getFloatField` 0x4ec980 returns 0 for a missing/non-float field and
does not log. 0x569b40 only stores the three floats; the matrix is built inside
0x4ec540.

### 0x4e2a50 java.util.render.RenderRef.setMatrix(ILjava.util.resource.ResourceRef;Vector3,Ypr)V
    getArgs(ctx, &self=-0x1c, &bone=-0x2c, &boneRef=-0x28, &pos=-0xc, &ypr=-0x10);
    h = Native__instanceToHandle(self);
    if (h != 0) { same pos/ypr field reads (x,y,z / y,p,r); same 0x569b40;
        RenderInst__setBoneMatrixYpr(boneId, boneRef, &pos, &ypr); }   // no getBoneId
    else log "vm_renderref_setmatrix: scripted resource %d not found ...";
Field-name bytes: pos 0x5a27f8/0x5a27fc/0x5a2800; ypr
0x5a2804/0x5a2808/0x5a280c. The boneRef handle goes straight into 0x4ec540 ->
0x4e84c0; that is the only consumer (see UNRESOLVED).

### 0x4e84c0 RenderInst__setBoneRef(handle, T, n, ref) — outline only
`ref == 0`: clear only when `n+0x5c` (stored ref id) != 0 -> destroy `n+0xbc`
(`vf0(1)`), `RI+0x84 &= ~0x4000`, `linkBoneRef(n, zeroHandle)`.
Otherwise when `n+0x5c != ref->id`: release as above, `linkBoneRef(n, ref)` (bind
n's embedded handle `n+0x54` to `ref->target`, append n to the dirty list), then
if `ref->target` has typed data: `n+0xbc = FUN_0051aa30(handle)`; else `n+0xbc = 0`;
always `FUN_0047df80(ref)`. For a fresh bone with ref 0 this is a no-op.

## Changes
1. `src/engine/math.h` line ~125: record the verified sin/cos mapping
   (`0x570fcb` -> 0x57e440 `fsin`, `0x570e7e` -> 0x57e3f0 `fcos`) instead of
   "inferred ... verify". No code change.
2. `src/script/natives.cpp`, next to card 075's `ri_get_bone_matrix`, add:
   ```cpp
   // 0x4ec980 over a Vector3 {x,y,z} or Ypr {y,p,r}: a missing argument or field
   // yields 0.0f; read through dd_get_f, which never fails.
   static void get3(Vm &vm, const Value &v, const char *a, const char *b,
                    const char *c, float out[3])
   {
       Object *o = v.kind == Value::Obj ? v.obj : nullptr;  // INVENTED: non-object -> null
       out[0] = o ? dd_get_f(vm, o, a) : 0.0f;
       out[1] = o ? dd_get_f(vm, o, b) : 0.0f;
       out[2] = o ? dd_get_f(vm, o, c) : 0.0f;
   }

   // 0x4ec540 RenderInst__setBoneMatrixYpr: bone `id`'s local matrix from ypr
   // (y,p,r, same order as the exe's stack vector 0x569b40) and pos (x10).
   void ri_set_bone_matrix_ypr(Runtime::Handle *h, uint32_t id,
                               const float pos[3], const float ypr[3])
   {
       if (!h || h->id == 0 || !h->target) return;   // exe: handle+8 == 0 -> 0
       Runtime::Resource *t = h->target;
       Runtime::Resource::Bone *b = find_bone(t, id);
       if (!b) return;
       Mat34 rot = {};                    // 0x5666d0 (euler_to_mat)
       euler_to_mat(&rot, ypr);
       float *m = b->local;               // 0x567570: transpose 3x3, pos*10, w=1
       m[0] = rot.m[0];  m[1] = rot.m[4];  m[2]  = rot.m[8];  m[3]  = 0.0f;
       m[4] = rot.m[1];  m[5] = rot.m[5];  m[6]  = rot.m[9];  m[7]  = 0.0f;
       m[8] = rot.m[2];  m[9] = rot.m[6];  m[10] = rot.m[10]; m[11] = 0.0f;
       m[12] = pos[0] * 10.0f; m[13] = pos[1] * 10.0f; m[14] = pos[2] * 10.0f;
       m[15] = 1.0f;
       t->text_flags |= 0x1800;           // 0x4ec0f0, typed data +0x16c
       // shortcut: 0x4ec0f0's dirty-list move (T+0x260/T+0x270), n+0x4c
       // identity bit, n+0x100, the T+0x1c0 frame counter and the 0x4e84c0
       // bone_ref attach are unmodelled; src has no render side reading them.
   }
   ```
3. Three natives (place after `tx_update`, before the Viewport section):
   ```cpp
   // 0x4e2140 RenderRef.create(ResourceRef parent, RenderRef type, String name)V
   bool rr_create(Vm &, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("RenderRef.create", err)) return false;
       Object *o;
       if (!receiver_object("RenderRef.create", a, &o, err) ||
           !want("RenderRef.create", a, 4, err))
           return false;
       Runtime::Handle *h = game_rt()->handle(o);
       Runtime::Handle *parent = value_handle(a[1]);
       Runtime::Handle *type = value_handle(a[2]);
       *out = Value::make_null();
       if (!h || !parent || !type) {
           // INVENTED: the exe appends the id, file and line (no frame here).
           std::fprintf(stderr, "vm_renderref_create: scripted resource not found\n");
           return true;
       }
       // parent handle unbound: the exe returns silently (no log). Type unbound
       // would fault in 0x51e090; INVENTED safety treats it the same as unbound.
       if (!parent->target || !type->target) return true;
       std::string name = a[3].kind == Value::Str ? a[3].s : std::string("_renderinst");
       Runtime::Resource *r = game_rt()->instantiate(type->target, parent->target,
                                                    name, "", false, err);
       if (!r) return false;
       r->type = 3;        // 0x524840 descriptor kind 3 (ins_render)
       r->flags |= 0x200;  // RI+0x84
       game_rt()->bind(h, r);           // 0x40cac0 (ECX = self handle)
       ri_get_bone_id(h, "bone00");     // 0x4e77f0 -> 0x51b940, id 0x8000
       return true;
   }

   // 0x4e2c80 RenderRef.setMatrix(Vector3 pos, Ypr ori)V
   bool rr_set_matrix(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("RenderRef.setMatrix", err)) return false;
       Object *o;
       if (!receiver_object("RenderRef.setMatrix", a, &o, err) ||
           !want("RenderRef.setMatrix", a, 3, err))
           return false;
       Runtime::Handle *h = game_rt()->handle(o);
       *out = Value::make_null();
       if (!h) {
           // INVENTED: the exe appends the id, file and line.
           std::fprintf(stderr, "vm_renderref_setmatrix_d: scripted resource not found\n");
           return true;
       }
       float pos[3], ypr[3];
       get3(vm, a[1], "x", "y", "z", pos);
       get3(vm, a[2], "y", "p", "r", ypr);
       ri_set_bone_matrix_ypr(h, ri_get_bone_id(h, "bone00"), pos, ypr);
       return true;
   }

   // 0x4e2a50 RenderRef.setMatrix(int bone, ResourceRef boneRef, Vector3 pos, Ypr ori)V
   bool rr_set_matrix_bone(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("RenderRef.setMatrix", err)) return false;
       Object *o;
       int32_t bone;
       if (!receiver_object("RenderRef.setMatrix", a, &o, err) ||
           !want("RenderRef.setMatrix", a, 5, err) ||
           !get_i("RenderRef.setMatrix", a[1], &bone, err))
           return false;
       Runtime::Handle *h = game_rt()->handle(o);
       *out = Value::make_null();
       if (!h) {
           // INVENTED: the exe appends the id, file and line.
           std::fprintf(stderr, "vm_renderref_setmatrix: scripted resource not found\n");
           return true;
       }
       float pos[3], ypr[3];
       get3(vm, a[3], "x", "y", "z", pos);
       get3(vm, a[4], "y", "p", "r", ypr);
       // a[2] (boneRef) only feeds 0x4e84c0, whose effects are unmodelled.
       ri_set_bone_matrix_ypr(h, (uint32_t)bone, pos, ypr);
       return true;
   }
   ```
4. Registry rows (natives.cpp ~6040/6042/6043): point `"RenderRef","create"`,
   `"RenderRef","setMatrix"` (5-arg) and `"RenderRef","setMatrix"` (3-arg) at
   `rr_create`, `rr_set_matrix_bone`, `rr_set_matrix`; `engine = false`. Leave
   `"RenderRef","getBoneId"` (075) and every other stub row untouched.

## Files
- edit: `src/script/natives.cpp` (two helpers, three natives, three registry rows)
- edit: `src/engine/math.h` (comment only, verified sin/cos mapping)
- read only: notes/12-render.md §9 "RenderRef" / "Render-instance bones"

## Done when
- every function carries its exe address; the Resource-as-T substitution is
  `INVENTED:`, the unmodelled render-side work is `shortcut:`/UNRESOLVED
- `create(parent, type, name)` on a bound receiver: new instance `type = 3`,
  `flags & 0x200`, handle rebound, and `getBoneId("bone00")` on it returns
  0x8000 (0x51b940 arithmetic from card 075); a missing self/parent/type
  argument logs `vm_renderref_create: ...`; an unbound parent argument does
  nothing and does not log
- `setMatrix(pos, ori)` writes bone00's `local` as the **transpose** of
  `euler_to_mat(ypr)` with translation `pos*10` and `[15] = 1`; the 5-arg form
  writes the same matrix into the bone id given in `a[1]` (no bone00 lookup)
- check by reading the code against 0x5666d0/0x567570/0x4ec540, **not** a unit
  test (CLAUDE.md: no hand-made expected values)
- `make -C src clean check` green; no new unit tests
- card frontmatter: status implemented, branch name

## UNRESOLVED
- `RenderInst__setBoneRef` 0x4e84c0 / `linkBoneRef` 0x4e8460 / `FUN_0051aa30`
  (`n+0xbc`, `RI+0x84 |= 0x4000`) / `FUN_0047df80` are only outlined; the
  boneRef argument of 0x4e2a50 is therefore parsed but not modelled. Nothing in
  src reads bone refs (no render side).
- `0x4ec0f0`'s dirty list (`T+0x260`), the `n+0x4c` identity bit, `n+0x100`,
  the second bone matrix `n+0xc0`, and the frame counter `T+0x1c0 =
  DAT_005dadbc` have no consumer in src.
- `Render__createInstance` 0x524840 internals not ported: bounds from the type
  data, `DAT_00773ad0++`, `RI+0x98 = 1.0f`, the `+0x11c` instance-kind switch
  and the scene registration for type 0xe (`0x4d5f50` ×2, `0x523be0`), bone
  pre-creation for boneCount N (always 0 here). `FUN_0051e090` (relink type) is
  approximated by `Runtime::instantiate`'s type/script_class handling.
- `getArgs` converting String -> `char*` and ResourceRef/RenderRef -> handle is
  taken from notes/06, not re-derived.
- An explicit empty String name becomes `"_gameinst"` in
  `Runtime::instantiate`; the exe would store `""` (exact-name edge case).
- notes/12 calls 0x569b40 "Ypr -> matrix"; the disassembly shows it only stores
  three floats into the stack vector, and 0x4ec540's 0x5666d0 does the matrix.
