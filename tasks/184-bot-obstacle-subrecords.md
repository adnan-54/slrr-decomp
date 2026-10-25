---
id: 184
status: implemented
branch: card/184
depends: [183]
---
# `native bot` obstacle sub-records from the contact body's shape (vf14 0x20 body: 0x482350 points, 0x481d20 edges)

## Why (notes/19 s8.2)
Each obstacle record (card 183) carries a chain of 0x40-byte sub-records describing the other body as circles
along its long axis plus 4 corner "edge" wedges; card 185 turns them into solver circles each tick. Static bodies
(type 1) store world coordinates once; dynamic ones (type 2) store body-local coordinates and are transformed
every tick.

## Exe evidence
Sub-record (0x40): `+0 next`, `[1..3] P` (point / edge corner C), `[4..6] A`, `[7..9] B`, `[10..12] n`
(edge direction, y forced 0, xz unit), `[0xd] ang` (-1 = point record), `[0xe] r`, `[0xf] h` (half height).
**Order: every new sub-record is pushed at the head of the chain** (`sub.next = rec.head; rec.head = sub`), so the
walk sees them in reverse creation order. `xf(p)` = world point `R*p + pos` of rec.body (state rows +0xa4/+0xb4/
+0xc4, pos +0x64), `xr(v)` = `R*v`; used only when `rec.type != 2`. `margin` = bot `+0x2594` (half_width).
```
0x482350 point(p, r, h):  sub.P = (rec.type == 2) ? p : xf(p); sub.r = r; sub.h = h; sub.ang = -1.0

0x481d20 edge(C, d /*0.1*/, h, E, s, margin):
  u = C - E (3D); inv = 1/|u|; nn = u * inv
  sub.ang = asin((s - d) * inv)                      // 0x570ac1
  k = (d + margin) / (s - d)
  A = C + u * k
  B = E + nn * (s + margin)
  if (rec.type == 2) { sub.P = C; sub.A = A; sub.B = B; sub.n = nn }
  else               { sub.P = xf(C); sub.A = xf(A); sub.B = xf(B); sub.n = xr(nn) }
  sub.n.y = 0; l = |sub.n|; if (l != 0) sub.n /= l
  sub.r = d; sub.h = h

vf14 0x20, after the record type is known (shape = rec.body def+0x90, kind at +8):
  kind 1 sphere: point((0,0,0), r = shape+0xc, h = shape+0xc)          // bound radius = r + 0.05
  kind 5 box:    X,Y,Z = shape+0xb4,+0xb8,+0xbc (half extents)  -> boxlike
  kind 6 mesh:   X,Y,Z = shape+0x14,+0x18,+0x1c (AABB half extents incl. +0.05, body frame) -> boxlike
  other kinds:   no sub-records
  boxlike:
    if (|X - Z| < 0.5) { point((0,0,0), r = (Z <= X ? X : Z), h = Y); done }
    if (Z <= X) { ax = (1,0,0); s = Z; L = X } else { ax = (0,0,1); s = X; L = Z }
    e = L - s
    P1 = ( e*ax.x, 0,  e*ax.z);  point(P1, s, Y)
    P2 = (-e*ax.x, 0, -e*ax.z);  point(P2, s, Y)
    if (s > 0.5) {
      edge(( X, 0,  Z), 0.1, Y, P1,                     s, margin)
      edge((-X, 0,  Z), 0.1, Y, ax.x <= 0 ? P1 : P2,    s, margin)
      edge(( X, 0, -Z), 0.1, Y, ax.x <= 0 ? P2 : P1,    s, margin)
      edge((-X, 0, -Z), 0.1, Y, P2,                     s, margin)
    }
    f = (L + L) - (s + s)
    n = (int)floor((double)(f / (margin + margin + s + s)))
    if (n != 0) { step = f / (float)(n + 1); t = s - L
                  repeat n times { t += step; point((t*ax.x, 0, t*ax.z), s, Y) } }
```

## Files
- edit: src/game/car.h — `BotObstacle`: `struct Sub { Vec3 P, A, B, n; float ang = -1, r = 0, h = 0; };
  std::vector<Sub> subs;` (document that index order = exe chain order, i.e. newest first, or push_front-style)
- edit: src/game/car.cpp — in `bot_trigger_event` code 0x20 (card 183): build the subs from
  `other->def.shape` (src `Shape`: `SHAPE_SPHERE` radius/bounding_radius(), `SHAPE_BOX` half_extents,
  `SHAPE_MESH` mesh bounds -> half extents + 0.05). `INVENTED:` if the root body's own shape is not 1/5/6 (src
  compound roots keep their hull in `collision_children`), use its first collision child shape; note it.
- read only: src/physics/physics.h, src/physics/mesh.h

## Done when
- `// 0x482350`, `// 0x481d20`, `// 0x480850 0x20 shape` comments; INVENTED items marked
- `make -C src clean check` green; no new unit tests or hand-made expected values
- card frontmatter: status implemented, branch card/184
