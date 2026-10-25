---
id: 190
status: todo
branch:
depends: [189]
---
# render/input: screen point -> world ray -> pick (0x4eb000, camera matrices 0x461dd0/0x461c20)

## Goal
`Runtime::screen_pick(cx, cy, PickHit*)`: for a cursor position in [-1,1]^2 (cursor typed
+0xa4/+0xa8, y up) find the first drawn viewport/camera whose ray hits something via
card 189's `Runtime::raycast`. Needs the camera's parent and viewport, which `cam_create`
(src/script/natives.cpp, 0x4d8de0) currently drops. Background: notes/18 s7 (line 223).

## Exe evidence (cleaned)
### 0x4eb000 pick(pos[2], ref_out, body_out, local_point_out, normal_out, index_out, t_out, far_out, near_out, sub_out)
```
clear all outputs (index = -1, t = 0)
for vp in drawn viewports (0x421220: game+0x174 list, list order):
    u = (pos.x - (2*vp.x - 1)) / vp.w - 1          // vp+0xd4 x, +0xdc w (relative rect)
    v = (pos.y - (2*vp.y - 1)) / vp.h - 1          // vp+0xd8 y, +0xe0 h
    if u < -1 or u > 1 or v < -1 or v > 1: continue
    v = -v
    for cam in vp.cameras (0x465100 first, 0x4210d0 next; list order):
        far  = unproject(cam, {u, v, 1.0}) * 0.1   // 0x421e20 -> 0x461c20
        near = unproject(cam, {u, v, 0.0}) * 0.1
        t = 1.0
        if raycast({far, near}, cam+0xac0 root, &t):   // 0x4fe1a0 (card 189: from=far, to=near)
            copy ref/body/point/normal/index/sub/t from the raycast result
            far_out = far; near_out = near
            return 1
return 0
```
Picked world point later used by the cursor (0x4b8430): `near + (far - near) * t`.
### 0x461c20 unproject(cam, p) (row vectors, D3D layout, m[r][c] at base + (r*4+c)*4)
```
IP = cam+0x45c (inverse projection), CM = cam+0x39c (camera matrix, camera->world)
w  = 1 / (p.x*IP[0][3] + p.y*IP[1][3] + p.z*IP[2][3] + IP[3][3])
q.x = (p.x*IP[0][0] + p.y*IP[1][0] + p.z*IP[2][0] + IP[3][0]) * w   (same for y, z with column 1, 2)
out.x = q.x*CM[0][0] + q.y*CM[1][0] + q.z*CM[2][0] + CM[3][0]     (same for y, z)
```
### 0x461dd0 camera setup (the parts the pick needs)
```
CM = camera instance's bone matrix (instance T+0x1f4 first bone +0xc0..+0xfc), identity if none
fov = type+0x10; aspect = type+0xc, or the viewport aspect (call argument) when type+0xc == 0
zn = type+0x14; k = sqrt(sqrt(cos(fov*pi/360) / sin(fov*pi/360))); zf = type+0x18 * k
P  = D3DXMatrixPerspectiveFovRH(fov*0.017453292, aspect, zn, zf)    // vf32 0x422e80, cam+0x41c
IP = inverse(P)                                                      // 0x567630 -> cam+0x45c
root (cam+0xac0) = camera instance's +0xfc node (its parent)         // tail of 0x461dd0
```
Sanity (from the shipped scripts, no check to write): Osd.java's camera has aov 60 (create
gets 30 = fov), sits at z = 5.48*vpHeight; tan(15 deg)*5.48 = 1.469 = Osd.SCALE_3D, so the
unprojected z=0 plane maps [-1,1] onto the gadget coordinates `convertTextCoordinates`.

## src mapping
- `cam_create` (0x4d8de0, natives.cpp ~6735): args are a[1] parent ResourceRef, a[2]
  Viewport, a[3] pri (the current `camera_viewport_id = a[3]` is the pri: rename the field
  to `camera_pri`, add `Resource *camera_viewport` from a[2] via value_resource, and set the
  new resource's `parent` to a[1]'s resource (only the pointer; INVENTED placement)).
- drawn viewports: Resources with `viewport_active` (INVENTED order: creation id ascending;
  exe order = game+0x174 list). Rect = `viewport_rect[0..3]` (x, y, w, h). Cameras of a
  viewport: camera Resources with `camera_viewport == vp`, creation order.
- type data: camera_fov / camera_aspect / camera_near / camera_far (already *10, as the exe).
  viewport aspect fallback: use the same value `vp_get_aspect` returns.
- CM: INVENTED from the camera Resource: rotation from `ori` (Ypr, same helper as
  GameRef.getOri users), translation = `pos * 10` if `gr_set_matrix` wrote a pose, else the
  bone00 position cam_create stores (`ri_set_bone_pos(.., {0,0,3})`, already render units).
  Reason: the exe keeps render-space matrices (x10) and scales the ray by 0.1.
- matrix inverse: use the existing Mat4 inverse in src/render/device.h if there is one, else
  a plain 4x4 inverse (comment `// 0x567630`).

## Files
- edit: src/script/natives.cpp (cam_create fields), src/game/runtime.h (camera_pri,
  camera_viewport, `bool screen_pick(float cx, float cy, PickHit *hit, float far_out[3], float near_out[3])`),
  src/game/runtime.cpp (screen_pick, unproject, camera matrices)
- read only: src/render/device.h (mat4_perspective_fov_rh is the same D3DX RH formula)

## Done when
- `// 0xADDR` comments; `INVENTED:` on viewport/camera order, CM source, parent placement.
- `make -C src clean check` green; no unit tests / hand-made values (card 191 runs it on the
  shipped main menu).
- frontmatter: status implemented, branch card/190.
