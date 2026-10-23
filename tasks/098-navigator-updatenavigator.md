---
id: 098
status: draft
branch:
depends: []
---
# script: java.game.Navigator.updateNavigator — GPS camera clamp/pose

## Goal
Replace the `java.game.Navigator.updateNavigator` stub row
(`src/script/natives.cpp:7276`) with the port of 0x4df470.

The native is the per-frame GPS/minimap camera driver; it is **not** related to
the traffic graph or route code (`src/game/traffic.*`, `notes/19` are not
involved). It:

1. resolves `cam` (java.render.Camera) and `vp` (Viewport) fields and the
   GameRef car argument; any of them unbound/unresolved -> return;
2. reads the car position, adds the receiver's `offsetX`/`offsetZ`;
3. in mode 0 only, clamps that position to the receiver's `top/left/size`
   rectangle (using the camera bone's y translation) and writes the resulting
   offset corrections back to `offsetX`/`offsetZ`;
4. writes the camera `bone00` local matrix: mode 0 = translation from the
   (clamped) position, mode 1 = yaw-only pose from the car orientation, any
   other mode = unchanged;
5. then updates the `dynamarker`/`marker` symbol bones. That part needs three
   render services src does not have (see SERVICE MISSING); the native returns
   `void`, so skipping it keeps the camera pose above.

Mode: 0 = `TYPE_MINIMAP`, 1 = `TYPE_ROUTER`/`TYPE_MINIMAP_STILL` (shipped
`game/Navigator.java`; it calls `updateNavigator(car, 1)` for ROUTER/STILL, `0`
for MINIMAP).

## Exe evidence (cleaned pseudocode, renamed fields)
`getArgs` 0x4b1af0 for `(Ljava.util.resource.GameRef;I)V`: receiver (Navigator
script object) at [-0x10], GameRef car at [-0x30], int mode at [-0x34]. All
`getInstanceField` names/addresses below are from the immediate strings
(`read_bytes`); `dd_get_f`/`dd_set_f` are src's 0x4ec980/0x4ed080.

Float constants (read from the exe):
`0x5935d0` = 0.01, `0x593738` = 5.0, `0x59373c` = 10.0, `0x59385c` = 100.0,
`0x5947b0` = 8.0, `0x59ab70` = 1.3333334, `0x5a6858` = 4.5, `0xbfc00000` = -1.5.

### 0x4df470 Navigator.updateNavigator(GameRef car, int mode)V
    cam = getObjectField(self, "cam" @0x5a5e30, "java.render.Camera" @0x5a5e04);
    if (!cam) return;                                  // 0x4ecae0, logs and 0
    camH = getIntField(cam, <native-handle field>);     // 0x4ecc70
    vp   = getResourceNative(self, "vp" @0x5a5e78);     // 0x4ecfc0 -> handle
    if (camH->id == 0) return;                          // [camH+8]
    if (carH->id == 0) return;                          // [carH+8]
    if (vp == 0) return;

    pos = car.position;                                 // 0x4ea7a0 (Game__getInfo id 2)
    ori = car.orientationYpr;                           // 0x4ea660 -> 0x569970
    boneId = getBoneId(camH, "bone00" @0x5a5ee4);       // 0x4e77f0
    getBoneMatrix(camH, boneId, &bm);                   // 0x4ea7f0, 64 bytes

    offX = getF(self, "offsetX" @0x5a5f20);             // 0x4ec980
    offZ = getF(self, "offsetZ" @0x5a5ef8);
    pos.x += offX;
    pos.z += offZ;

    if (mode == 0) {                                    // jnz skips the clamp
        top  = getF(self, "top"  @0x5a5f6c) * 100.0;
        left = getF(self, "left" @0x5a5f90) * 100.0;
        size = getF(self, "size" @0x5a5fa4) * 100.0 * 8.0;
        h    = bm[13] * 5.0;                    // bone y translation (x10 units)
        k    = h * 1.3333334;
        loZ = top + h;              hiZ = (size + top) - h;
        loX = left + k;             hiX = (size + left) - k;
        // Z first, then X. comiss/jbe ordering = plain > / else-if (NaN-safe:
        // unordered takes the lo branch in both).
        if (loZ > pos.z) {
            setF(self, "offsetZ" @0x5a6064, (loZ - pos.z) + offZ); pos.z = loZ;
        } else if (hiZ < pos.z) {
            setF(self, "offsetZ" @0x5a6030, (hiZ - pos.z) + offZ); pos.z = hiZ;
        }
        if (loX > pos.x) {
            setF(self, "offsetX" @0x5a6134, (loX - pos.x) + offX); pos.x = loX;
        } else if (hiX < pos.x) {
            setF(self, "offsetX" @0x5a610c, (hiX - pos.x) + offX); pos.x = hiX;
        }
    }

    switch (mode) {
    case 0:
        bm[12] = pos.x * 0.01 * 10.0;   // two multiplies, in this order
        bm[14] = pos.z * 0.01 * 10.0;   // bm[13] keeps the bone's value
        break;
    case 1: {
        // 0x4ea660 wrote {yaw,pitch,roll}; the exe then overwrites pitch/roll:
        float ang[3] = {carYaw, -1.5, 0.0};
        Mat34 R; euler_to_mat(&R, ang);                     // 0x5666d0
        // (m[0]+m[1])*0.0 terms in the exe are compiler artifacts of the
        // matrix product; the stored values are exactly:
        pos3 = { R.m[2]*4.5 + pos.x*0.01,
                 R.m[6]*4.5,
                 R.m[10]*4.5 + pos.z*0.01 };
        Matrix4__fromRotPosX10(&bm, &R, &pos3);             // 0x567570, writes bm
        break;
    }
    default: break;                                        // bm unchanged
    }
    RenderInst__setBoneMatrix(camH, boneId, 0, &bm);       // 0x4ec3b0

    // marker loops here (see SERVICE MISSING), then return void.

### 0x567570 Matrix4__fromRotPosX10 (__thiscall, ECX = dst)
    dst[0]=rot[0]; dst[1]=rot[4]; dst[2]=rot[8];  dst[3]=0;
    dst[4]=rot[1]; dst[5]=rot[5]; dst[6]=rot[9];  dst[7]=0;
    dst[8]=rot[2]; dst[9]=rot[6]; dst[10]=rot[10]; dst[11]=0;
    dst[12]=pos[0]*10.0; dst[13]=pos[1]*10.0; dst[14]=pos[2]*10.0; dst[15]=1.0;

(transposes the 3x3; already implemented inline in
`ri_set_bone_matrix_ypr`, src/script/natives.cpp:5421-5440.)

### 0x4ec3b0 RenderInst__setBoneMatrix(ECX = handle, id, unused, matrix)
    if (handle->id == 0) return 0;
    typed = handle->typed; if (!typed) return 1;
    bone = findBone(id);                                // 0x51c450
    if (bone) { if (matrix) 0x4ec0f0(bone, matrix); 0x4e84c0(); }
    typed[?+0x1c0] = DAT_005dadbc;                       // frame counter
    return 1;

0x4ec0f0 copies the 64-byte matrix into the bone node, moves the node to the
typed dirty list (typed+0x260/+0x270) and sets typed+0x16c |= 0x1800. Same tail
as the existing `ri_set_bone_matrix_ypr` (0x4ec540) port.

### 0x569970 Math__matToYpr (ECX = out[3], arg = world matrix)
    if (m[10] == 0 && m[2] == 0) { out = {atan2(m[0], -m[1]), PI, 0}; return; }
    out[0] = atan2(m[2], m[10]);          // yaw
    out[2] = atan2(m[4], m[5]);           // roll
    out[1] = ...                          // pitch (not used here)

Mode 1 only consumes `out[0]`. `GameRef.getOri` (0x4dac40) is the same
0x4ea660 -> 0x569970 path and is already mapped to `Resource::ori` in src
(`gr_get_ori`, natives.cpp:1881), so use `car->ori[0]` as carYaw.

## Changes
All in `src/script/natives.cpp` (1 file).

1. Immediately before `ri_set_bone_matrix_ypr` (line 5418), add the two
   helpers, then replace that function's inline matrix writes with
   `mat_from_rot_pos_x10(b->local, rot, pos);` (the existing
   `t->text_flags |= 0x1800;` line stays):
   ```cpp
   // 0x567570 Matrix4__fromRotPosX10 (ECX = m): transposed 3x3 of rot,
   // translation pos*10, w = 1.
   void mat_from_rot_pos_x10(float m[16], const Mat34 &rot, const float pos[3])
   {
       m[0] = rot.m[0]; m[1] = rot.m[4]; m[2]  = rot.m[8];  m[3]  = 0.0f;
       m[4] = rot.m[1]; m[5] = rot.m[5]; m[6]  = rot.m[9];  m[7]  = 0.0f;
       m[8] = rot.m[2]; m[9] = rot.m[6]; m[10] = rot.m[10]; m[11] = 0.0f;
       m[12] = pos[0] * 10.0f; m[13] = pos[1] * 10.0f;
       m[14] = pos[2] * 10.0f; m[15] = 1.0f;
   }

   // 0x4ec3b0 RenderInst__setBoneMatrix: store the local matrix and mark the
   // typed data dirty (0x4ec0f0 -> +0x16c |= 0x1800).
   // shortcut: 0x4e84c0 bone_ref and the typed+0x1c0 frame counter are not
   // modelled (same as ri_set_bone_matrix_ypr).
   void ri_set_bone_matrix(Runtime::Handle *h, uint32_t id, const float m[16])
   {
       if (!h || h->id == 0 || !h->target) return;
       if (auto *b = find_bone(h->target, id)) {
           for (int i = 0; i < 16; i++) b->local[i] = m[i];
           h->target->text_flags |= 0x1800;
       }
   }
   ```
2. After `gr_set_fog` (ends line 6276), before the `// ---- GroundRef
   service-less natives` section, add:
   ```cpp
   // ---- java.game.Navigator -------------------------------------------------

   // 0x4df470 Navigator.updateNavigator(GameRef car, int mode)V
   // mode 0 clamps the car position to the map rect and writes the offsets back;
   // then the camera bone00 matrix is set (mode 0 translation, mode 1 yaw pose).
   bool nav_update_navigator(Vm &vm, const Args &a, Value *out, std::string *err)
   {
       if (!need_rt("Navigator.updateNavigator", err)) return false;
       Object *self;
       int32_t mode;
       if (!receiver_object("Navigator.updateNavigator", a, &self, err) ||
           !want("Navigator.updateNavigator", a, 3, err) ||
           !get_i("Navigator.updateNavigator", a[2], &mode, err))
           return false;
       *out = Value::make_null();

       // 0x4ecae0 getObjectField(self, "cam", "java.render.Camera")
       std::string e;
       Value fv;
       Object *cam = vm.get_instance_field(self, "cam", &fv, &e) &&
                             fv.kind == Value::Obj ? fv.obj : nullptr;
       if (!cam || !game_rt()->class_is_a(object_class_name(cam), "java.render.Camera"))
           return true;                              // exe returns void
       Runtime::Handle *cam_h = game_rt()->handle(cam);       // 0x4ecc70
       Runtime::Handle *car = value_handle(a[1]);
       Runtime::Handle *vp = nullptr;
       if (vm.get_instance_field(self, "vp", &fv, &e) && fv.kind == Value::Obj)
           vp = value_handle(fv);                     // 0x4ecfc0
       if (!cam_h || cam_h->id == 0 || !car || car->id == 0 || !vp)
           return true;

       Runtime::Resource *cr = car->target;
       if (!cr) return true;   // INVENTED: unbound handle, exe reads zeros
       float pos[3] = {cr->pos[0], cr->pos[1], cr->pos[2]};    // 0x4ea7a0
       uint32_t bone = ri_get_bone_id(cam_h, "bone00");        // 0x4e77f0
       float bm[16] = {};      // INVENTED: exe reads uninitialized stack when
                               // the bone is missing; zeroed here
       ri_get_bone_matrix(cam_h, bone, bm);                    // 0x4ea7f0

       float off_x = dd_get_f(vm, self, "offsetX");            // 0x4ec980
       float off_z = dd_get_f(vm, self, "offsetZ");
       pos[0] += off_x;
       pos[2] += off_z;

       if (mode == 0) {                                        // clamp
           float top = dd_get_f(vm, self, "top") * 100.0f;
           float left = dd_get_f(vm, self, "left") * 100.0f;
           float size = dd_get_f(vm, self, "size") * 100.0f * 8.0f;
           float h = bm[13] * 5.0f;
           float k = h * 1.3333334f;
           float lo_z = top + h, hi_z = (size + top) - h;
           float lo_x = left + k, hi_x = (size + left) - k;
           if (lo_z > pos[2]) {
               dd_set_f(vm, self, "offsetZ", (lo_z - pos[2]) + off_z);
               pos[2] = lo_z;
           } else if (hi_z < pos[2]) {
               dd_set_f(vm, self, "offsetZ", (hi_z - pos[2]) + off_z);
               pos[2] = hi_z;
           }
           if (lo_x > pos[0]) {
               dd_set_f(vm, self, "offsetX", (lo_x - pos[0]) + off_x);
               pos[0] = lo_x;
           } else if (hi_x < pos[0]) {
               dd_set_f(vm, self, "offsetX", (hi_x - pos[0]) + off_x);
               pos[0] = hi_x;
           }
       }

       const float *m = bm;
       float m1[16];
       if (mode == 0) {
           bm[12] = pos[0] * 0.01f * 10.0f;
           bm[14] = pos[2] * 0.01f * 10.0f;
       } else if (mode == 1) {
           // 0x4ea660 -> 0x569970 yields {yaw,pitch,roll}; the exe replaces
           // pitch/roll with -1.5/0 before building the pose.
           // INVENTED mapping: the car yaw is the resource's ypr y
           // (same mapping as GameRef.getOri, natives.cpp:1881).
           float ang[3] = {cr->ori[0], -1.5f, 0.0f};
           Mat34 r = {};
           euler_to_mat(&r, ang);                              // 0x5666d0
           float p[3] = {r.m[2] * 4.5f + pos[0] * 0.01f,
                         r.m[6] * 4.5f,
                         r.m[10] * 4.5f + pos[2] * 0.01f};
           mat_from_rot_pos_x10(m1, r, p);                     // 0x567570
           m = m1;
       }
       ri_set_bone_matrix(cam_h, bone, m);                     // 0x4ec3b0

       // SERVICE MISSING: the "dynamarker"/"marker" loops after 0x4ec3b0 need
       // 0x422320 (find the viewport node in the GfxEngine lists +0x198/+0x14c
       // by node+0x34 == vp->id), 0x421260/0x465100 (viewport scale),
       // 0x4d88a0 (world->screen projected point via 0x421670/0x421e20, with
       // x clamped to [-0.9,0.9] and y to [-0.85,0.85]), 0x459290 and the
       // render typed data +0xb0..+0xc8. src has none of these; the exe's
       // effect there is moving marker symbol bones. The native returns void,
       // so skipping the loops keeps the camera pose above.
       return true;
   }
   ```
3. Registry row 7276:
   ```cpp
   {"java.game.Navigator", "updateNavigator", "(Ljava.util.resource.GameRef;I)V", nav_update_navigator, false},
   ```

## Files
- edit: src/script/natives.cpp (helpers after `ri_set_bone_matrix_ypr`;
  `nav_update_navigator` after `gr_set_fog`; registry row 7276)
- read only: src/engine/math.h (`euler_to_mat`, `Mat34`)
- read only: src/game/runtime.h (`Resource::pos/ori`, `Bone::local`, `Handle`)

## Done when
- `nav_update_navigator` has the `// 0x4df470` comment; helpers carry
  `// 0x567570` and `// 0x4ec3b0`; every non-exe choice is `INVENTED:` and the
  marker loops are marked `SERVICE MISSING:` with the 0x4d88a0/0x422320 note
- registry row 7276 uses the function and ends in `false`
- `make -C src clean check` green; no new unit tests or hand-made expected
  values (constants above are read from the exe, field names from its strings)
- card frontmatter: status implemented, branch name

## UNRESOLVED
- `DAT_007a2620` is the runtime-interned field-name pointer for the native
  handle int field read by 0x4ecc70/0x4ecfc0; its file value is 0 (set up at
  runtime), so the literal name is unknown. Mapped to `game_rt()->handle(obj)`
  (= src's `value_handle`), the same stand-in used by the Camera natives.
- Mode 1's yaw provenance: 0x4ea660 gets GameRef info id 1, resolves the typed
  render object and calls 0x569970 on its world matrix. The matrix builder used
  by `GameRef.setMatrix` (0x56a000 -> 0x56a050) was not traced, so the port
  uses `Resource::ori[0]`, the established src stand-in for the same
  getInfo-id-1 ypr (`gr_get_ori` 0x4dac40).
- The exe's `(m[0]+m[1])*0.0` / `(m[4]+m[5])*0.0` / `(m[8]+m[9])*0.0` terms in
  the mode-1 pose are dropped as compiler artifacts. Raw pseudo lines:
  `local_c = (local_60 + local_64) * 0.0 + local_5c * 4.5;`
  `local_8 = (local_50 + local_54) * 0.0 + local_4c * 4.5 + local_8 * 0.01;`
  `local_10 = (local_70 + local_74) * 0.0 + local_6c * 4.5 + local_10 * 0.01;`
  They only differ when the matrix entries are NaN/Inf.
- Marker loops not ported (services above). Their recorded structure, for a
  later card: `dynamarker` vector -> per element `symbol`/`mover`; mover pos
  (0x4ea7a0), yaw info id 4 (0x47c640) * 0.017453292, euler_to_mat, pos *0.1
  with y = 0.031, project 0x4d88a0, pos *0.1, 0x567570, bone00 of symbol via
  0x4e77f0/0x4ec3b0, then 0x459290 on symbol+0xc. `marker` vector uses the
  symbol's `pos` Vector3 fields (x/y/z) and y = 0.01, no mover, and sets
  symbol typed data +0xb0 = 2 and +0xb4..+0xc8 (values recorded as zeros,
  +0xc8 = 200) when symbol+0xc != 0.
- 0x4ecae0's class test may be exact-class rather than is-a; the port uses
  `class_is_a`. Only a script Camera subclass would differ.
