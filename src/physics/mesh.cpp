// Mesh collision queries (implementation of physics/mesh.h).
//
// Contact normals follow the engine convention: B -> A in world space. The
// gather functions work in the mesh's local frame and always produce the
// normal pointing from the mesh toward the primitive; mesh_contacts flips it
// when the mesh is shape A.
#include "physics/mesh.h"

#include <math.h>

#include <algorithm>

#include "mesh/scx.h"

namespace {

float comp(Vec3 v, int i)
{
    return i == 0 ? v.x : (i == 1 ? v.y : v.z);
}

bool aabb_overlap(Vec3 amin, Vec3 amax, Vec3 bmin, Vec3 bmax)
{
    return amin.x <= bmax.x && bmin.x <= amax.x && amin.y <= bmax.y && bmin.y <= amax.y &&
           amin.z <= bmax.z && bmin.z <= amax.z;
}

void expand(Vec3 *mn, Vec3 *mx, Vec3 p)
{
    if (p.x < mn->x)
        mn->x = p.x;
    if (p.y < mn->y)
        mn->y = p.y;
    if (p.z < mn->z)
        mn->z = p.z;
    if (p.x > mx->x)
        mx->x = p.x;
    if (p.y > mx->y)
        mx->y = p.y;
    if (p.z > mx->z)
        mx->z = p.z;
}

Vec3 triangle_normal(Vec3 a, Vec3 b, Vec3 c)
{
    return vec_normalize(vec_cross(b - a, c - a), Vec3{0.0f, 1.0f, 0.0f});
}

// Closest point on a triangle (Ericson, Real-Time Collision Detection).
Vec3 closest_on_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c)
{
    Vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = vec_dot(ab, ap), d2 = vec_dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f)
        return a;
    Vec3 bp = p - b;
    float d3 = vec_dot(ab, bp), d4 = vec_dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3)
        return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return a + ab * v;
    }
    Vec3 cp = p - c;
    float d5 = vec_dot(ab, cp), d6 = vec_dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6)
        return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return a + ac * w;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && d4 - d3 >= 0.0f && d5 - d6 >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + (c - b) * w;
    }
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom, w = vc * denom;
    return a + ab * v + ac * w;
}

bool barycentric(Vec3 p, Vec3 a, Vec3 b, Vec3 c, float *bu, float *bv, float *bw)
{
    Vec3 v0 = b - a, v1 = c - a, v2 = p - a;
    float d00 = vec_dot(v0, v0), d01 = vec_dot(v0, v1), d11 = vec_dot(v1, v1);
    float d20 = vec_dot(v2, v0), d21 = vec_dot(v2, v1);
    float den = d00 * d11 - d01 * d01;
    if (fabsf(den) < 1e-20f)
        return false;
    *bv = (d11 * d20 - d01 * d21) / den;
    *bw = (d00 * d21 - d01 * d20) / den;
    *bu = 1.0f - *bv - *bw;
    return true;
}

bool point_in_triangle(Vec3 p, Vec3 a, Vec3 b, Vec3 c)
{
    float u, v, w;
    return barycentric(p, a, b, c, &u, &v, &w) && u >= -1e-6f && v >= -1e-6f && w >= -1e-6f;
}

// True when the closest point sits in the triangle's face (not an edge/vertex).
bool face_region(Vec3 q, Vec3 a, Vec3 b, Vec3 c)
{
    float u, v, w;
    return barycentric(q, a, b, c, &u, &v, &w) && u > 1e-4f && v > 1e-4f && w > 1e-4f;
}

void triangle_bounds(const CollisionMesh &m, uint32_t ti, Vec3 *mn, Vec3 *mx)
{
    const CollisionMesh::Triangle &t = m.triangles[ti];
    *mn = m.vertices[t.v[0]];
    *mx = *mn;
    expand(mn, mx, m.vertices[t.v[1]]);
    expand(mn, mx, m.vertices[t.v[2]]);
}

int32_t build_node(CollisionMesh &m, const std::vector<Vec3> &centroids, uint32_t first,
                   uint32_t count)
{
    int32_t index = static_cast<int32_t>(m.nodes.size());
    m.nodes.emplace_back();
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    Vec3 cmn = mn, cmx = mx;
    for (uint32_t i = first; i < first + count; i++) {
        uint32_t ti = m.order[i];
        Vec3 tmn, tmx;
        triangle_bounds(m, ti, &tmn, &tmx);
        expand(&mn, &mx, tmn);
        expand(&mn, &mx, tmx);
        expand(&cmn, &cmx, centroids[ti]);
    }
    m.nodes[index].min = mn;
    m.nodes[index].max = mx;
    if (count <= 4) {
        m.nodes[index].first = first;
        m.nodes[index].count = count;
        return index;
    }
    Vec3 ext = cmx - cmn;
    int axis = ext.x >= ext.y ? (ext.x >= ext.z ? 0 : 2) : (ext.y >= ext.z ? 1 : 2);
    uint32_t mid = count / 2;
    std::nth_element(m.order.begin() + first, m.order.begin() + first + mid,
                     m.order.begin() + first + count, [&centroids, axis](uint32_t a, uint32_t b) {
                         return comp(centroids[a], axis) < comp(centroids[b], axis);
                     });
    int32_t left = build_node(m, centroids, first, mid);
    int32_t right = build_node(m, centroids, first + mid, count - mid);
    m.nodes[index].left = left;
    m.nodes[index].right = right;
    return index;
}

void query_aabb(const CollisionMesh &m, Vec3 qmin, Vec3 qmax, std::vector<uint32_t> *out)
{
    if (m.nodes.empty())
        return;
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const CollisionMesh::Node &n = m.nodes[stack[--sp]];
        if (!aabb_overlap(n.min, n.max, qmin, qmax))
            continue;
        if (n.count > 0) {
            for (uint32_t i = n.first; i < n.first + n.count; i++)
                out->push_back(m.order[i]);
        } else {
            if (n.left >= 0 && sp < 64)
                stack[sp++] = static_cast<uint32_t>(n.left);
            if (n.right >= 0 && sp < 64)
                stack[sp++] = static_cast<uint32_t>(n.right);
        }
    }
}

bool ray_aabb(Vec3 o, Vec3 d, Vec3 mn, Vec3 mx, float max_t, float *tmin_out)
{
    float tmin = 0.0f, tmax = max_t;
    for (int i = 0; i < 3; i++) {
        float oi = comp(o, i), di = comp(d, i);
        float lo = comp(mn, i), hi = comp(mx, i);
        if (fabsf(di) < 1e-8f) {
            if (oi < lo || oi > hi)
                return false;
            continue;
        }
        float inv = 1.0f / di;
        float t1 = (lo - oi) * inv, t2 = (hi - oi) * inv;
        if (t1 > t2) {
            float tmp = t1;
            t1 = t2;
            t2 = tmp;
        }
        if (t1 > tmin)
            tmin = t1;
        if (t2 < tmax)
            tmax = t2;
        if (tmin > tmax)
            return false;
    }
    *tmin_out = tmin;
    return true;
}

bool ray_triangle(Vec3 o, Vec3 d, Vec3 a, Vec3 b, Vec3 c, float max_t, float *tout)
{
    Vec3 e1 = b - a, e2 = c - a, p = vec_cross(d, e2);
    float det = vec_dot(e1, p);
    if (fabsf(det) < 1e-9f)
        return false;
    float inv = 1.0f / det;
    Vec3 tv = o - a;
    float u = vec_dot(tv, p) * inv;
    if (u < 0.0f || u > 1.0f)
        return false;
    Vec3 q = vec_cross(tv, e1);
    float v = vec_dot(d, q) * inv;
    if (v < 0.0f || u + v > 1.0f)
        return false;
    float t = vec_dot(e2, q) * inv;
    if (t < 0.0f || t > max_t)
        return false;
    *tout = t;
    return true;
}

// Closest points between two segments (same clamping as physics/shape.cpp).
void segment_closest(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, Vec3 *c1, Vec3 *c2)
{
    Vec3 d1 = q1 - p1, d2 = q2 - p2, r = p1 - p2;
    float a = vec_dot(d1, d1), e = vec_dot(d2, d2), f = vec_dot(d2, r);
    float s = 0.0f, t = 0.0f;
    if (a <= 1e-12f && e <= 1e-12f) {
        s = t = 0.0f;
    } else if (a <= 1e-12f) {
        t = f / e;
        t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    } else {
        float c = vec_dot(d1, r);
        if (e <= 1e-12f) {
            s = -c / a;
            s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
        } else {
            float b = vec_dot(d1, d2);
            float denom = a * e - b * b;
            s = denom != 0.0f ? (b * f - c * e) / denom : 0.0f;
            s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
            t = (b * s + f) / e;
            if (t < 0.0f) {
                t = 0.0f;
                s = -c / a;
                s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
            } else if (t > 1.0f) {
                t = 1.0f;
                s = (b - c) / a;
                s = s < 0.0f ? 0.0f : (s > 1.0f ? 1.0f : s);
            }
        }
    }
    *c1 = p1 + d1 * s;
    *c2 = p2 + d2 * t;
}

// Closest points between a segment and a triangle. The minimum is one of:
// an endpoint against the triangle, the segment against an edge, or the
// segment crossing the triangle plane inside the face.
struct SegmentTri {
    Vec3 seg{0.0f, 0.0f, 0.0f}, tri{0.0f, 0.0f, 0.0f};
    float dist2 = 1e30f;
    bool face = false;
};

void consider_seg(SegmentTri *best, Vec3 s, Vec3 t, bool face)
{
    Vec3 d = s - t;
    float d2 = vec_dot(d, d);
    if (d2 < best->dist2) {
        best->dist2 = d2;
        best->seg = s;
        best->tri = t;
        best->face = face;
    }
}

SegmentTri segment_triangle_closest(Vec3 s0, Vec3 s1, Vec3 a, Vec3 b, Vec3 c)
{
    SegmentTri best;
    Vec3 q = closest_on_triangle(s0, a, b, c);
    consider_seg(&best, s0, q, face_region(q, a, b, c));
    q = closest_on_triangle(s1, a, b, c);
    consider_seg(&best, s1, q, face_region(q, a, b, c));
    const Vec3 e[3] = {a, b, c};
    for (int i = 0; i < 3; i++) {
        Vec3 cs, ct;
        segment_closest(s0, s1, e[i], e[(i + 1) % 3], &cs, &ct);
        consider_seg(&best, cs, ct, false);
    }
    Vec3 n = vec_cross(b - a, c - a);
    float denom = vec_dot(n, s1 - s0);
    if (fabsf(denom) > 1e-9f) {
        float t = vec_dot(n, a - s0) / denom;
        if (t >= 0.0f && t <= 1.0f) {
            Vec3 p = s0 + (s1 - s0) * t;
            if (point_in_triangle(p, a, b, c))
                consider_seg(&best, p, p, true);
        }
    }
    return best;
}

struct Cand {
    Vec3 pa; // primitive surface witness (mesh-local)
    Vec3 pb; // mesh witness (mesh-local)
    Vec3 n;  // unit, mesh -> primitive (mesh-local)
    float sep = 0.0f;
    uint32_t triangle = 0;
};

void add_cand(std::vector<Cand> *out, Vec3 pa, Vec3 pb, Vec3 n, float sep, uint32_t tri,
              float slop)
{
    if (slop < 0.0f || sep < slop)
        out->push_back(Cand{pa, pb, n, sep, tri});
}

void gather_sphere(const CollisionMesh &m, Vec3 centre, float radius, float slop,
                   std::vector<Cand> *out)
{
    float margin = slop >= 0.0f ? slop : 0.05f;
    float reach = radius + margin;
    Vec3 qmin{centre.x - reach, centre.y - reach, centre.z - reach};
    Vec3 qmax{centre.x + reach, centre.y + reach, centre.z + reach};
    std::vector<uint32_t> tris;
    query_aabb(m, qmin, qmax, &tris);
    for (uint32_t ti : tris) {
        const CollisionMesh::Triangle &t = m.triangles[ti];
        Vec3 a = m.vertices[t.v[0]], b = m.vertices[t.v[1]], c = m.vertices[t.v[2]];
        Vec3 q = closest_on_triangle(centre, a, b, c);
        Vec3 face = triangle_normal(a, b, c);
        Vec3 d = centre - q;
        float dist = vec_length(d);
        Vec3 n;
        float sep;
        if (face_region(q, a, b, c)) {
            n = face;
            sep = vec_dot(d, n) - radius;
        } else if (dist > 1e-6f) {
            n = d * (1.0f / dist);
            sep = dist - radius;
        } else {
            n = face;
            sep = -radius;
        }
        add_cand(out, centre - n * radius, q, n, sep, ti, slop);
    }
}

void capsule_segment(const Shape &s, const Pose &p, Vec3 *e0, Vec3 *e1)
{
    Vec3 axis{0.0f, 0.0f, 0.0f};
    if (s.type == SHAPE_CAPSULE_X)
        axis.x = 1.0f;
    else if (s.type == SHAPE_CAPSULE_Y)
        axis.y = 1.0f;
    else
        axis.z = 1.0f;
    Vec3 w = mat_vec(pose_rotation(p), axis) * s.half_length;
    *e0 = p.pos - w;
    *e1 = p.pos + w;
}

void gather_capsule(const CollisionMesh &m, Vec3 e0, Vec3 e1, float radius, float slop,
                    std::vector<Cand> *out)
{
    float margin = slop >= 0.0f ? slop : 0.05f;
    float reach = radius + margin;
    Vec3 qmin{fminf(e0.x, e1.x) - reach, fminf(e0.y, e1.y) - reach,
              fminf(e0.z, e1.z) - reach};
    Vec3 qmax{fmaxf(e0.x, e1.x) + reach, fmaxf(e0.y, e1.y) + reach,
              fmaxf(e0.z, e1.z) + reach};
    std::vector<uint32_t> tris;
    query_aabb(m, qmin, qmax, &tris);
    for (uint32_t ti : tris) {
        const CollisionMesh::Triangle &t = m.triangles[ti];
        Vec3 a = m.vertices[t.v[0]], b = m.vertices[t.v[1]], c = m.vertices[t.v[2]];
        SegmentTri h = segment_triangle_closest(e0, e1, a, b, c);
        Vec3 face = triangle_normal(a, b, c);
        Vec3 n;
        float sep;
        if (h.face) {
            n = face;
            sep = vec_dot(h.seg - h.tri, n) - radius;
        } else {
            float dist = sqrtf(h.dist2);
            if (dist > 1e-6f) {
                n = (h.seg - h.tri) * (1.0f / dist);
                sep = dist - radius;
            } else {
                n = face;
                sep = -radius;
            }
        }
        add_cand(out, h.seg - n * radius, h.tri, n, sep, ti, slop);
    }
}

// Closest point on an axis-aligned box; `*inside` gives the nearest face gap.
Vec3 closest_on_box(Vec3 l, Vec3 he, bool *inside, float *gap)
{
    Vec3 q{l.x < -he.x ? -he.x : (l.x > he.x ? he.x : l.x),
           l.y < -he.y ? -he.y : (l.y > he.y ? he.y : l.y),
           l.z < -he.z ? -he.z : (l.z > he.z ? he.z : l.z)};
    Vec3 d = l - q;
    if (vec_dot(d, d) > 0.0f) {
        *inside = false;
        return q;
    }
    *inside = true;
    const float lv[3] = {l.x, l.y, l.z};
    const float hv[3] = {he.x, he.y, he.z};
    *gap = hv[0] - fabsf(lv[0]);
    for (int i = 1; i < 3; i++) {
        float g = hv[i] - fabsf(lv[i]);
        if (g < *gap)
            *gap = g;
    }
    return q;
}

void gather_box(const CollisionMesh &m, Vec3 pos, const Mat34 &rot, Vec3 he, float slop,
                std::vector<Cand> *out)
{
    float margin = slop >= 0.0f ? slop : 0.05f;
    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    Vec3 corners[8];
    for (int i = 0; i < 8; i++) {
        Vec3 l{(i & 1) ? he.x : -he.x, (i & 2) ? he.y : -he.y, (i & 4) ? he.z : -he.z};
        corners[i] = pos + mat_vec(rot, l);
        expand(&mn, &mx, corners[i]);
    }
    Vec3 qmin{mn.x - margin, mn.y - margin, mn.z - margin};
    Vec3 qmax{mx.x + margin, mx.y + margin, mx.z + margin};
    std::vector<uint32_t> tris;
    query_aabb(m, qmin, qmax, &tris);
    for (uint32_t ti : tris) {
        const CollisionMesh::Triangle &t = m.triangles[ti];
        Vec3 a = m.vertices[t.v[0]], b = m.vertices[t.v[1]], c = m.vertices[t.v[2]];
        Vec3 face = triangle_normal(a, b, c);

        // Box corners against the triangle.
        for (int i = 0; i < 8; i++) {
            Vec3 q = closest_on_triangle(corners[i], a, b, c);
            Vec3 d = corners[i] - q;
            float dist = vec_length(d);
            Vec3 n;
            float sep;
            if (face_region(q, a, b, c)) {
                n = face;
                sep = vec_dot(d, n);
            } else if (dist > 1e-6f) {
                n = d * (1.0f / dist);
                sep = dist;
            } else {
                n = face;
                sep = 0.0f;
            }
            add_cand(out, corners[i], q, n, sep, ti, slop);
        }

        // Triangle vertices against the box.
        const Vec3 verts[3] = {a, b, c};
        for (int i = 0; i < 3; i++) {
            Vec3 v = verts[i];
            Vec3 l = mat_tvec(rot, v - pos);
            bool inside = false;
            float gap = 0.0f;
            Vec3 ql = closest_on_box(l, he, &inside, &gap);
            if (inside) {
                Vec3 n = face;
                if (vec_dot(n, pos - v) < 0.0f)
                    n = -n;
                add_cand(out, v, v, n, -gap, ti, slop);
            } else {
                float dist = vec_length(l - ql);
                if (dist > 1e-6f) {
                    Vec3 n = mat_vec(rot, (ql - l) * (1.0f / dist));
                    add_cand(out, pos + mat_vec(rot, ql), v, n, dist, ti, slop);
                }
            }
        }
    }
}

} // namespace

std::shared_ptr<const CollisionMesh> build_collision_mesh(const mesh::Mesh &src)
{
    std::vector<Vec3> verts;
    std::vector<CollisionMesh::Triangle> tris;
    for (const mesh::Section &s : src.sections) {
        uint32_t base = static_cast<uint32_t>(verts.size());
        for (const mesh::Vec3 &p : s.positions)
            verts.push_back(Vec3{p.x * mesh::kCollisionScale, p.y * mesh::kCollisionScale,
                                 p.z * mesh::kCollisionScale});
        size_t ntri = s.indices.empty() ? s.indices16.size() / 3 : s.indices.size() / 3;
        for (size_t i = 0; i < ntri; i++) {
            uint32_t i0 = s.indices.empty() ? s.indices16[i * 3 + 0] : s.indices[i * 3 + 0];
            uint32_t i1 = s.indices.empty() ? s.indices16[i * 3 + 1] : s.indices[i * 3 + 1];
            uint32_t i2 = s.indices.empty() ? s.indices16[i * 3 + 2] : s.indices[i * 3 + 2];
            if (i0 >= s.positions.size() || i1 >= s.positions.size() ||
                i2 >= s.positions.size())
                continue;
            tris.push_back(CollisionMesh::Triangle{{base + i0, base + i1, base + i2}});
        }
    }
    return build_collision_mesh(verts, tris);
}

std::shared_ptr<const CollisionMesh> build_collision_mesh(
    const std::vector<Vec3> &verts, const std::vector<CollisionMesh::Triangle> &tris)
{
    std::shared_ptr<CollisionMesh> out = std::make_shared<CollisionMesh>();
    out->vertices = verts;
    out->triangles.reserve(tris.size());
    for (const CollisionMesh::Triangle &t : tris) {
        if (t.v[0] >= verts.size() || t.v[1] >= verts.size() || t.v[2] >= verts.size())
            continue;
        Vec3 a = verts[t.v[0]], b = verts[t.v[1]], c = verts[t.v[2]];
        Vec3 cr = vec_cross(b - a, c - a);
        if (vec_dot(cr, cr) <= 1e-16f)
            continue; // degenerate face
        out->triangles.push_back(t);
    }
    if (out->triangles.empty())
        return std::shared_ptr<const CollisionMesh>();

    Vec3 mn{1e30f, 1e30f, 1e30f}, mx{-1e30f, -1e30f, -1e30f};
    for (const Vec3 &v : out->vertices) {
        expand(&mn, &mx, v);
        float r = vec_length(v);
        if (r > out->radius)
            out->radius = r;
    }
    out->bounds_min = mn;
    out->bounds_max = mx;

    size_t n = out->triangles.size();
    std::vector<Vec3> centroids(n);
    out->order.resize(n);
    for (size_t i = 0; i < n; i++) {
        const CollisionMesh::Triangle &t = out->triangles[i];
        centroids[i] = (out->vertices[t.v[0]] + out->vertices[t.v[1]] +
                        out->vertices[t.v[2]]) *
                       (1.0f / 3.0f);
        out->order[i] = static_cast<uint32_t>(i);
    }
    out->nodes.reserve(2 * n);
    build_node(*out, centroids, 0, static_cast<uint32_t>(n));
    return out;
}

bool mesh_raycast(const CollisionMesh &m, const Pose &pose, Vec3 origin, Vec3 dir,
                  float max_dist, MeshRayHit *hit)
{
    if (m.nodes.empty() || max_dist < 0.0f)
        return false;
    Mat34 rot = pose_rotation(pose);
    Vec3 lo = mat_tvec(rot, origin - pose.pos);
    Vec3 ld = mat_tvec(rot, dir);
    float best = max_dist;
    uint32_t best_tri = 0;
    bool found = false;
    uint32_t stack[64];
    int sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const CollisionMesh::Node &n = m.nodes[stack[--sp]];
        float nt;
        if (!ray_aabb(lo, ld, n.min, n.max, best, &nt))
            continue;
        if (n.count > 0) {
            for (uint32_t i = n.first; i < n.first + n.count; i++) {
                uint32_t ti = m.order[i];
                const CollisionMesh::Triangle &t = m.triangles[ti];
                float tt;
                if (ray_triangle(lo, ld, m.vertices[t.v[0]], m.vertices[t.v[1]],
                                 m.vertices[t.v[2]], best, &tt)) {
                    best = tt;
                    best_tri = ti;
                    found = true;
                }
            }
        } else {
            if (n.left >= 0 && sp < 64)
                stack[sp++] = static_cast<uint32_t>(n.left);
            if (n.right >= 0 && sp < 64)
                stack[sp++] = static_cast<uint32_t>(n.right);
        }
    }
    if (!found)
        return false;
    const CollisionMesh::Triangle &t = m.triangles[best_tri];
    Vec3 nrm = triangle_normal(m.vertices[t.v[0]], m.vertices[t.v[1]], m.vertices[t.v[2]]);
    if (vec_dot(nrm, ld) > 0.0f)
        nrm = -nrm;
    hit->point = pose.pos + mat_vec(rot, lo + ld * best);
    hit->normal = mat_vec(rot, nrm);
    hit->distance = best;
    hit->triangle = best_tri;
    return true;
}

int mesh_contacts(const CollisionMesh &m, const Pose &mesh_pose, const Shape &prim,
                  const Pose &prim_pose, bool mesh_is_a, ShapeHit *hits, int max_hits,
                  float slop)
{
    if (m.empty() || max_hits <= 0)
        return 0;
    Mat34 rot = pose_rotation(mesh_pose);
    Vec3 origin = prim_pose.pos - mesh_pose.pos;
    Vec3 pos = mat_tvec(rot, origin);

    std::vector<Cand> cands;
    switch (prim.type) {
    case SHAPE_SPHERE:
        gather_sphere(m, pos, prim.radius, slop, &cands);
        break;
    case SHAPE_CAPSULE_X:
    case SHAPE_CAPSULE_Y:
    case SHAPE_CAPSULE_Z: {
        Vec3 e0, e1;
        capsule_segment(prim, prim_pose, &e0, &e1);
        gather_capsule(m, mat_tvec(rot, e0 - mesh_pose.pos), mat_tvec(rot, e1 - mesh_pose.pos),
                       prim.radius, slop, &cands);
        break;
    }
    case SHAPE_BOX: {
        Mat34 wrt, prim_rot, trans;
        mat_transpose(&wrt, &rot);
        prim_rot = pose_rotation(prim_pose);
        mat_mul(&trans, &wrt, &prim_rot);
        gather_box(m, pos, trans, prim.half_extents, slop, &cands);
        break;
    }
    default:
        return 0;
    }

    // Flip witnesses and normals back to world. The mesh is A when mesh_is_a;
    // otherwise it is B and the gathered mesh -> primitive normal is B -> A.
    std::sort(cands.begin(), cands.end(),
              [](const Cand &a, const Cand &b) { return a.sep < b.sep; });
    Cand kept[4];
    int count = 0;
    for (const Cand &c : cands) {
        if (count >= max_hits || count >= 4)
            break;
        if (slop >= 0.0f && c.sep >= slop)
            continue;
        bool dup = false;
        for (int i = 0; i < count; i++) {
            Vec3 da = kept[i].pa - c.pa, db = kept[i].pb - c.pb;
            if (vec_dot(da, da) < 1e-8f && vec_dot(db, db) < 1e-8f) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        kept[count++] = c;
    }
    for (int i = 0; i < count; i++) {
        Vec3 pa = mesh_pose.pos + mat_vec(rot, kept[i].pa);
        Vec3 pb = mesh_pose.pos + mat_vec(rot, kept[i].pb);
        Vec3 n = mat_vec(rot, kept[i].n);
        if (mesh_is_a) {
            hits[i].point_a = pb;
            hits[i].point_b = pa;
            hits[i].normal = -n;
            hits[i].feature_a = static_cast<int>(kept[i].triangle);
            hits[i].feature_b = 0;
        } else {
            hits[i].point_a = pa;
            hits[i].point_b = pb;
            hits[i].normal = n;
            hits[i].feature_a = 0;
            hits[i].feature_b = static_cast<int>(kept[i].triangle);
        }
        hits[i].separation = kept[i].sep;
    }
    return count;
}
