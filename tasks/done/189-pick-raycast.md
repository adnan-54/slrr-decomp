---
id: 189
status: merged
branch: card/189
depends: []
---
# physics: PhysicsRef box/sphere shapes + segment raycast over the resource tree (pick core)

## Goal
Port the exe's pick ray cast so a later card (190) can turn a screen point into the Osd
gadget under the mouse. Today `PhysicsRef.createBox/createSphere` (src/script/natives.cpp
`ph_create_box`/`ph_create_sphere`) drop the size. Store the shape, then add
`Runtime::raycast(root, from, to, PickHit*)` = 0x4fe1a0 + 0x4fddf0 + 0x4fc140 for kind-2
(physics) instances with box/sphere shapes. Background: notes/18 s7 (line 223).

## Exe evidence (cleaned)
### 0x4e16a0 / 0x4e1880 PhysicsRef.createBox(parent,x,y,z,alias) / createSphere(parent,r,alias)
0x4f7d60 builds the shape: box half extents = x*0.5f, y*0.5f, z*0.5f; sphere radius = r.
Shape kinds (shape+8): 1 sphere, 5 box, 6/7 poly hull. The body pose is what
`PhysicsRef.setMatrix` sets (src keeps it in `physics_poses()[r]` pos + ypr).

### 0x4fe1a0 raycast(seg[6] = from xyz, to xyz, root_link, float *t_out)
```
from = seg[0..2]; to = seg[3..5]; d = to - from; len = |d|
dir = d / max(len, 1e-6)          // unit, from -> to
best_t = 1.0; hit = none; first_only = (t_out == NULL)
clear hit ref/body/point/normal; hit_index = -1; hit_sub = -1
node = root_link.node; if (!node) return 0
r = walk(node)                     // 0x4fddf0
if first_only: return r
if best_t < 1.0:
   point = (from - to) * best_t + to          // world point (t counts from `to`)
   if hit_body: point = R^T * (point - body_pos)   // body-local, R = body state rotation
   *t_out = best_t; return 1
return 0
```
### 0x4fddf0 walk(node)
```
if node.kind == 1 (render/group instance, src: not a physics instance):
    // 0x4fd110 triangle test of the node's own mesh: NOT ported (see INVENTED)
    if (node.flags & 0x40) == 0:                 // src Resource::flags = RI+0x84
        for child in node.children (list order):
            if child.kind not in {1,2}: continue
            // bounds cull by child+0xb0 (0 sphere 0x516640, 1 box 0x516450, 2 none)
            if walk(child) && first_only: return 1
elif node.kind == 2 (physics instance):
    body = node's body; if body && (body.flags & 0x4020) == 0 && body has a shape:
        if body_hit(body): return 1
return 0
```
### 0x4fc140 body_hit(body)
```
shape = body.shape
// bounding sphere first: 0x5007c0(from, dir, len, body_pos, shape.bound_radius)
t0 = ray_sphere(from, dir, len, body_pos, R); if !(0 <= t0 <= best_t) return 0
for child body in body.children: if body_hit(child) return 1     // child bodies first
switch shape.kind:
  case 5 (box): a = to_local(from); b = to_local(to)              // 0x4af9a0: R^T*(p - pos)
       t = slab(a, b, hx, hy, hz); if t <= 0.0: return 0         // 0x500580; NOTE <= 0
       /* fall through */
  case 1 (sphere: t = t0 from above):
       if t < best_t: best_t = t; hit_ref = body owner link (body+0x44); hit_body = body;
                      hit_index = 0; hit_sub = 0; normal = 0
       break
  case 6/7: per-face 0x4fd6d0: NOT ported
return 0
```
### 0x5007c0 ray_sphere(o, dir, len, c, r)  (returns t in [0,1] from the `to` end, or -1)
```
v = c - o; p = dot(dir, v)
if p < -r or p > len + r: return -1
r2 = r*r; q = |v|^2 - p*p
if q <= r2 and (p >= 0 or p*p + q <= r2) and (p <= len or (p-len)^2 + q <= r2):
    t = ((len - p) - sqrt(r2 - q)) / len; return max(t, 0)
return -1
```
### 0x500580 slab(a, b, hx, hy, hz)  (segment a->b in box-local space; t from b toward a)
```
per axis k (h = hx/hy/hz): reject (return -1) unless (a.k <= h or b.k <= h) and (-h <= a.k or -h <= b.k)
d = a - b
per axis: if d.k == 0: lo_k = 0, hi_k = 1
          else s = sign(d.k) (0x4b1ef0: 0 -> 0, <0 -> -1, else 1)
               lo_k = (-s*h - b.k) / d.k; hi_k = (s*h - b.k) / d.k
lo = max(lo_x, lo_y, lo_z); hi = min(hi_x, hi_y, hi_z)   // order below
if lo <= hi: return max(lo, 0)  else return -1
```
Careful with the exe's init: `lo = (d.y==0 ? 0 : lo_y)`, `hi = (d.y==0 ? 1 : hi_y)`, then
`hi = min(hi, hi_x)`, `hi = min(hi, hi_z)`, `lo = max(lo, lo_x)`, `lo = max(lo, lo_z)` (x/z
use 0/1 when their d is 0). Keep that order.

## Field mapping for src
- kind 2 = a Runtime::Resource with `type == 2` (INSTANCE_PHYSICS, set by ph_create_box);
  kind 1 = any other instance node. Children = `Resource::children`.
- body pose = `physics_poses()[res]` (pos, ypr; rotation from ypr with the same helper
  ph_set_matrix / ph_get_ori use). body flags 0x4020: none modelled -> treat as 0.
- hit_ref (body+0x44): the instance's **parent** Resource (Group.java receives the cursor
  events, see card 191). hit_body = the physics instance Resource (its `id` is the
  "physics id" token).
- Shape storage: add to Runtime::Resource `int pick_shape = 0; float pick_half[3]; float
  pick_radius;` (INVENTED storage; exe keeps it in the body's shape). bound_radius for a box:
  INVENTED = |half extents| (shape+0xc not traced).

## Files
- edit: src/game/runtime.h (shape fields, `struct PickHit { Resource *ref, *body; float t; float point[3]; }`,
  `bool raycast(Resource *root, const float from[3], const float to[3], PickHit *hit)`),
  src/game/runtime.cpp (raycast/walk/body_hit/ray_sphere/slab), src/script/natives.cpp
  (ph_create_box/ph_create_sphere store the shape).

## Done when
- each ported function carries `// 0xADDR` comments; `INVENTED:` on: kind-1 triangle and
  kind-6/7 hull tests skipped, child-bounds cull skipped (src has no node bounds), bound
  radius, shape storage, hit_ref = parent.
- `make -C src clean check` green; no new unit tests or hand-made expected values
  (card 191 exercises it on the shipped menus).
- frontmatter: status implemented, branch card/189.
