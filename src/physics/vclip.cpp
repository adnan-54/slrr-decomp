// V-Clip polyhedra and closest-feature queries (implementation of vclip.h).
// Addresses are the exe functions the notes describe (notes/11 §2/§3).
#include "physics/vclip.h"

#include <math.h>
#include <stdio.h>

#include <map>
#include <utility>

namespace {

// INVENTED: vmboot harness counter (card 165); the exe only prints.
int g_vclip_overruns = 0;

Vec3 to_world(const Pose &pose, Vec3 local)
{
    return pose.pos + mat_vec(pose_rotation(pose), local);
}

Vec3 to_local(const Pose &pose, Vec3 world)
{
    return mat_tvec(pose_rotation(pose), world - pose.pos);
}

Vec3 normalize_or(Vec3 v, Vec3 fallback)
{
    return vec_normalize(v, fallback);
}

// World geometry of one feature, as the distance routines read it.
struct FeatureGeom {
    int kind = POLY_V;
    Vec3 p{0.0f, 0.0f, 0.0f};   // V point / E tail / F plane point
    Vec3 h{0.0f, 0.0f, 0.0f};   // E head
    Vec3 dir{0.0f, 0.0f, 0.0f}; // E direction
    Vec3 n{0.0f, 0.0f, 0.0f};   // F outward normal
};

FeatureGeom feature_geom(const Poly &poly, const Pose &pose, const PolyFeature &f)
{
    Mat34 r = pose_rotation(pose);
    FeatureGeom g;
    g.kind = f.kind;
    g.p = pose.pos + mat_vec(r, poly.verts[f.vertex]);
    g.h = pose.pos + mat_vec(r, poly.verts[f.head]);
    g.dir = mat_vec(r, f.dir);
    g.n = mat_vec(r, f.normal);
    return g;
}

Vec3 closest_on_segment(Vec3 p, Vec3 a, Vec3 b)
{
    Vec3 ab = b - a;
    float den = vec_dot(ab, ab);
    if (den <= 1e-12f)
        return a;
    float t = vec_dot(p - a, ab) / den;
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    return a + ab * t;
}

// Closest points between two segments (Math__segSegClosest2 0x509740).
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
            float den = a * e - b * b;
            s = den != 0.0f ? (b * f - c * e) / den : 0.0f;
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

struct Hit {
    Vec3 pa{0.0f, 0.0f, 0.0f}; // witness on A
    Vec3 pb{0.0f, 0.0f, 0.0f}; // witness on B
    Vec3 n{0.0f, 1.0f, 0.0f};  // world, B -> A
    float d = 0.0f;            // signed distance
};

void hit_normal(Hit *h)
{
    Vec3 d = h->pa - h->pb;
    float len = vec_length(d);
    h->n = len > 1e-8f ? d * (1.0f / len) : Vec3{0.0f, 1.0f, 0.0f};
}

// VClip__distVF 0x50c400: plane distance, witness = point - d * n.
void hit_vf(FeatureGeom v, FeatureGeom f, bool v_is_a, Hit *h)
{
    float d = vec_dot(f.n, v.p - f.p);
    Vec3 on_plane = v.p - f.n * d;
    if (v_is_a) {
        h->pa = v.p;
        h->pb = on_plane;
        h->n = f.n; // face is B, so B -> A is the outward normal
    } else {
        h->pa = on_plane;
        h->pb = v.p;
        h->n = -f.n; // face is A, so B -> A points into the face
    }
    h->d = d;
}

// E-F distance as VClip__stepEF 0x50aa60 reports it: the crossing point when
// the endpoints straddle the plane, else the end the routine selects (d0 when
// d0 <= 0, otherwise d1), never the smaller |d| of two same-side endpoints.
void hit_ef(FeatureGeom e, FeatureGeom f, bool e_is_a, Hit *h)
{
    float d0 = vec_dot(f.n, e.p - f.p);
    float d1 = vec_dot(f.n, e.h - f.p);
    Vec3 on_edge;
    if (d0 * d1 <= 0.0f) {
        float den = d0 - d1;
        float t = fabsf(den) > 1e-12f ? d0 / den : 0.0f;
        on_edge = e.p + (e.h - e.p) * t;
        // stepEF 0x50aa60 returns the depth of the end behind the plane.
        h->d = d0 <= 0.0f ? d0 : d1;
    } else {
        on_edge = d0 <= 0.0f ? e.p : e.h;
        h->d = d0 <= 0.0f ? d0 : d1;
    }
    Vec3 on_plane = on_edge - f.n * vec_dot(f.n, on_edge - f.p);
    if (e_is_a) {
        h->pa = on_edge;
        h->pb = on_plane;
        h->n = f.n;
    } else {
        h->pa = on_plane;
        h->pb = on_edge;
        h->n = -f.n;
    }
}

Hit distance_pair(const Poly &poly_a, const Pose &pose_a, const PolyFeature &fa,
                  const Poly &poly_b, const Pose &pose_b, const PolyFeature &fb)
{
    FeatureGeom a = feature_geom(poly_a, pose_a, fa);
    FeatureGeom b = feature_geom(poly_b, pose_b, fb);
    Hit h;
    if (a.kind == POLY_V && b.kind == POLY_V) {
        h.pa = a.p;
        h.pb = b.p;
        h.d = vec_length(h.pa - h.pb);
        hit_normal(&h);
    } else if (a.kind == POLY_V && b.kind == POLY_E) {
        h.pa = a.p;
        h.pb = closest_on_segment(a.p, b.p, b.h);
        h.d = vec_length(h.pa - h.pb);
        hit_normal(&h);
    } else if (a.kind == POLY_E && b.kind == POLY_V) {
        h.pa = closest_on_segment(b.p, a.p, a.h);
        h.pb = b.p;
        h.d = vec_length(h.pa - h.pb);
        hit_normal(&h);
    } else if (a.kind == POLY_V && b.kind == POLY_F) {
        hit_vf(a, b, true, &h);
    } else if (a.kind == POLY_F && b.kind == POLY_V) {
        hit_vf(b, a, false, &h);
    } else if (a.kind == POLY_E && b.kind == POLY_E) {
        segment_closest(a.p, a.h, b.p, b.h, &h.pa, &h.pb);
        h.d = vec_length(h.pa - h.pb);
        hit_normal(&h);
    } else if (a.kind == POLY_E && b.kind == POLY_F) {
        hit_ef(a, b, true, &h);
    } else if (a.kind == POLY_F && b.kind == POLY_E) {
        hit_ef(b, a, false, &h);
    } else {
        // F-F is never measured; the walk replaces A's face by an edge first.
        h.pa = a.p;
        h.pb = b.p;
        h.d = vec_length(h.pa - h.pb);
        hit_normal(&h);
    }
    return h;
}

// True Voronoi-region test (notes/11 §2): p must satisfy n.(p - point) >= 0
// for every plane of the feature. Returns the neighbour to step to, or -1.
// INVENTED: when several planes are violated this picks the largest violation;
// the notes give only the half-space test, not the exe's plane order.
int outside_region(const Pose &pose, const PolyFeature &f, Vec3 world_point)
{
    Vec3 p = to_local(pose, world_point);
    int step = -1;
    float worst = 0.0f;
    for (const PolyPlane &pl : f.planes) {
        float v = vec_dot(pl.normal, p - pl.point);
        if (v < 0.0f && v < worst) {
            worst = v;
            step = pl.neighbour;
        }
    }
    return step;
}

// Poly__buildFromFaces 0x50e6d0: capacity nv + nf + nf*3/2, one F per face,
// V/E features shared between faces, E-V and E-F plane pairs.
Poly build_from_faces(const std::vector<Vec3> &verts,
                      const std::vector<std::vector<uint32_t>> &faces)
{
    Poly poly;
    poly.verts = verts;
    const size_t nf = faces.size();
    const size_t nv = poly.verts.size();
    poly.features.reserve(nv + nf + nf * 3 / 2);

    std::map<uint32_t, int> vertex_feature;
    std::map<std::pair<uint32_t, uint32_t>, int> edge_feature;

    auto add_plane = [](PolyFeature &f, int neighbour, Vec3 point, Vec3 normal) {
        PolyPlane p;
        p.neighbour = neighbour;
        p.point = point;
        p.normal = normal;
        f.planes.push_back(p);
    };

    for (const std::vector<uint32_t> &face : faces) {
        const int count = static_cast<int>(face.size());
        if (count < 3 || count > 7)
            continue;
        Vec3 v0 = poly.verts[face[0]];
        Vec3 v1 = poly.verts[face[1]];
        Vec3 v2 = poly.verts[face[2]];
        Vec3 n = normalize_or(vec_cross(v1 - v0, v2 - v0), Vec3{0.0f, 1.0f, 0.0f});
        Vec3 q = v0 + (v1 - v0) * 0.25f + (v2 - v0) * 0.25f;

        PolyFeature ff;
        ff.kind = POLY_F;
        ff.vertex = static_cast<int>(face[0]);
        ff.normal = n;
        const int fi = static_cast<int>(poly.features.size());
        poly.features.push_back(ff);

        auto vertex_of = [&](uint32_t vi) {
            auto it = vertex_feature.find(vi);
            if (it != vertex_feature.end())
                return it->second;
            PolyFeature vf;
            vf.kind = POLY_V;
            vf.vertex = static_cast<int>(vi);
            int index = static_cast<int>(poly.features.size());
            poly.features.push_back(vf);
            vertex_feature[vi] = index;
            return index;
        };

        for (int i = 0; i < count; i++) {
            uint32_t vi = face[i];
            uint32_t vj = face[(i + 1) % count];
            int ti = vertex_of(vi);
            int hj = vertex_of(vj);
            std::pair<uint32_t, uint32_t> key(vi < vj ? vi : vj, vi < vj ? vj : vi);
            auto it = edge_feature.find(key);
            int ei;
            if (it == edge_feature.end()) {
                PolyFeature ef;
                ef.kind = POLY_E;
                ef.vertex = static_cast<int>(vi);
                ef.head = static_cast<int>(vj);
                ef.dir = normalize_or(poly.verts[vj] - poly.verts[vi],
                                      Vec3{1.0f, 0.0f, 0.0f});
                ei = static_cast<int>(poly.features.size());
                poly.features.push_back(ef);
                edge_feature[key] = ei;
                // E-V plane pairs: E gets {tail V, +dir} and {head V, -dir}.
                add_plane(poly.features[ei], ti, poly.verts[vi], poly.features[ei].dir);
                add_plane(poly.features[ei], hj, poly.verts[vj], -poly.features[ei].dir);
                add_plane(poly.features[ti], ei, poly.verts[vi], -poly.features[ei].dir);
                add_plane(poly.features[hj], ei, poly.verts[vj], poly.features[ei].dir);
            } else {
                ei = it->second;
            }
            // E-F plane pair: m = dir x n_face, flipped so the face interior
            // q lies on +m. F gets +m, E gets -m (Feature__addPlanePair).
            const PolyFeature &ef = poly.features[ei];
            Vec3 m = vec_cross(ef.dir, n);
            Vec3 tail = poly.verts[ef.vertex];
            if (vec_dot(m, q - tail) < 0.0f)
                m = -m;
            // 0x50e6d0 stores m = dir x n as is (not renormalized).
            add_plane(poly.features[fi], ei, tail, m);
            add_plane(poly.features[ei], fi, tail, -m);
        }
    }
    return poly;
}

} // namespace

Poly poly_from_triangles(const CollisionMesh &mesh)
{
    std::vector<std::vector<uint32_t>> faces;
    faces.reserve(mesh.triangles.size());
    for (const CollisionMesh::Triangle &t : mesh.triangles)
        faces.push_back({t.v[0], t.v[1], t.v[2]});
    return build_from_faces(mesh.vertices, faces);
}

Poly poly_triangle(Vec3 v0, Vec3 v1, Vec3 v2, Vec3 n)
{
    (void)n; // 0x50f000's first call derives the normals from the winding.
    return build_from_faces({v0, v1, v2}, {{0, 1, 2}, {0, 2, 1}});
}

PolyCounts poly_counts(const Poly &p)
{
    PolyCounts c;
    for (const PolyFeature &f : p.features) {
        if (f.kind == POLY_V)
            c.verts++;
        else if (f.kind == POLY_E)
            c.edges++;
        else if (f.kind == POLY_F)
            c.faces++;
    }
    c.closed = c.verts - c.edges + c.faces == 2;
    for (const PolyFeature &f : p.features) {
        if (f.kind != POLY_E)
            continue;
        int adjacent_faces = 0;
        for (const PolyPlane &pl : f.planes)
            if (pl.neighbour >= 0 &&
                pl.neighbour < static_cast<int>(p.features.size()) &&
                p.features[pl.neighbour].kind == POLY_F)
                adjacent_faces++;
        if (adjacent_faces != 2)
            c.closed = false;
    }
    return c;
}

namespace {

// DAT_006060e0: the side whose feature the step routines move first. A global
// in the exe, never reset by 0x50d230, so it carries over between queries.
int g_side = 0;

// The walk state of 0x50d230 (DAT_0060617c poses, DAT_00606174 polys,
// DAT_00606184 features), indexed by side.
struct Walk {
    const Pose *pose[2];
    const Poly *poly[2];
    int feat[2];

    const PolyFeature &f(int s) const { return poly[s]->features[feat[s]]; }
    const PolyFeature &nb(int s, int i) const { return poly[s]->features[i]; }
    Vec3 vert(int s, int v) const { return poly[s]->verts[v]; }
    // A point of side `from` in the frame of side `to` (Phys__transformPoint 0x50e2d0).
    Vec3 xf(int from, int to, Vec3 p) const
    {
        return to_local(*pose[to], to_world(*pose[from], p));
    }
};

float plane_eval(const PolyPlane &pl, Vec3 p)
{
    return vec_dot(pl.normal, p - pl.point);
}

// VClip__clipEdge 0x508fe0: clip segment t->h (feature frame) against planes
// [from, to) of `f`. lo/hi are in/out; n_lo/n_hi are reset (-1 = none).
bool clip_edge(Vec3 t, Vec3 h, const PolyFeature &f, int *n_lo, int *n_hi,
               float *lo, float *hi, int from, int to)
{
    *n_lo = *n_hi = -1;
    for (int i = from; i < to; i++) {
        const PolyPlane &pl = f.planes[i];
        float dt = plane_eval(pl, t);
        float dh = plane_eval(pl, h);
        if (dt >= 0.0f) {
            if (dh < 0.0f) {
                float l = dt / (dt - dh);
                if (l < *hi) {
                    *hi = l;
                    *n_hi = pl.neighbour;
                    if (!(*lo <= l))
                        return false;
                }
            }
        } else {
            if (dh < 0.0f) {
                *n_lo = *n_hi = pl.neighbour;
                return false;
            }
            float l = dt / (dt - dh);
            if (l > *lo) {
                *lo = l;
                *n_lo = pl.neighbour;
                if (!(l <= *hi))
                    return false;
            }
        }
    }
    return true;
}

// d/dlambda of |x(lambda) - V|^2 sign: (x(lambda) - V) . u.
float vertex_deriv(const Walk &w, int s, int vfeat, Vec3 t, Vec3 u, float lam)
{
    return vec_dot(t + u * lam - w.vert(s, w.nb(s, vfeat).vertex), u);
}

// D_F(x(lambda)) * (n_F . u) for a face neighbour.
float face_deriv(const Walk &w, int s, int ffeat, Vec3 t, Vec3 u, float lam)
{
    const PolyFeature &fc = w.nb(s, ffeat);
    Vec3 p = w.vert(s, fc.vertex);
    return vec_dot(fc.normal, t + u * lam - p) * vec_dot(fc.normal, u);
}

// The other side's edge, in side s's frame.
void other_edge(const Walk &w, int s, Vec3 *t, Vec3 *h)
{
    const PolyFeature &e = w.f(s ^ 1);
    *t = w.xf(s ^ 1, s, w.vert(s ^ 1, e.vertex));
    *h = w.xf(s ^ 1, s, w.vert(s ^ 1, e.head));
}

// VClip__checkVV 0x50d050.
int check_vv(Walk &w)
{
    for (int pass = 0; pass < 2; pass++) {
        int o = g_side ^ 1;
        Vec3 p = w.xf(g_side, o, w.vert(g_side, w.f(g_side).vertex));
        const PolyFeature &v = w.f(o);
        for (const PolyPlane &pl : v.planes)
            if (plane_eval(pl, p) < 0.0f) {
                w.feat[o] = pl.neighbour;
                return 1;
            }
        g_side = o;
    }
    return 0;
}

// VClip__stepVE 0x50be40 (g_side = V).
int step_ve(Walk &w)
{
    const int s = g_side, o = s ^ 1;
    Vec3 p = w.xf(s, o, w.vert(s, w.f(s).vertex));
    const PolyFeature &e = w.f(o);
    for (const PolyPlane &pl : e.planes)
        if (plane_eval(pl, p) < 0.0f) {
            w.feat[o] = pl.neighbour;
            return 1;
        }
    Vec3 t, h;
    other_edge(w, s, &t, &h);
    Vec3 u = h - t;
    float lo = 0.0f, hi = 1.0f;
    int nlo, nhi;
    const PolyFeature &v = w.f(s);
    clip_edge(t, h, v, &nlo, &nhi, &lo, &hi, 0, (int)v.planes.size());
    if (nlo == nhi) {
        if (nlo >= 0) {
            w.feat[s] = nlo;
            return 1;
        }
    } else if (nlo >= 0 && vertex_deriv(w, s, w.feat[s], t, u, lo) > 0.0f) {
        w.feat[s] = nlo;
        return 1;
    }
    if (nhi >= 0 && vertex_deriv(w, s, w.feat[s], t, u, hi) < 0.0f) {
        w.feat[s] = nhi;
        return 1;
    }
    return 0;
}

// VClip__handleLocalMin 0x50b7b0: p is the vertex in the face side's frame.
int handle_local_min(Walk &w, Vec3 p)
{
    const int o = g_side ^ 1;
    const Poly &poly = *w.poly[o];
    float best = -1e8f;
    int pick = -1;
    for (int i = 0; i < (int)poly.features.size(); i++) {
        const PolyFeature &fc = poly.features[i];
        if (fc.kind != POLY_F)
            continue;
        float d = vec_dot(fc.normal, p - poly.verts[fc.vertex]);
        if (best < d) {
            best = d;
            pick = i;
        }
    }
    if (poly.features.empty())
        return 2;
    if (0.0f < best) {
        w.feat[o] = pick;
        return 1;
    }
    if (pick >= 0)
        w.feat[o] = pick;
    return 2;
}

// VClip__stepVF 0x50c830 (g_side = V).
int step_vf(Walk &w)
{
    const int s = g_side, o = s ^ 1;
    const PolyFeature &vf = w.f(s);
    Vec3 p = w.xf(s, o, w.vert(s, vf.vertex));
    const PolyFeature &fc = w.f(o);
    if (!fc.planes.empty()) {
        float min = 1.0f;
        int step = -1;
        for (const PolyPlane &pl : fc.planes) {
            float v = plane_eval(pl, p);
            if (v < min) {
                min = v;
                step = pl.neighbour;
            }
        }
        if (min < 0.0f) {
            w.feat[o] = step;
            return 1;
        }
    }
    const Vec3 fp = w.vert(o, fc.vertex);
    const float d = vec_dot(fc.normal, p - fp);
    const float ad = fabsf(d);
    for (const PolyPlane &pl : vf.planes) {
        const PolyFeature &e = w.nb(s, pl.neighbour);
        int other = e.vertex == vf.vertex ? e.head : e.vertex;
        Vec3 q = w.xf(s, o, w.vert(s, other));
        float dq = vec_dot(fc.normal, q - fp);
        if (fabsf(dq) < ad || dq * d < 0.0f) {
            w.feat[s] = pl.neighbour;
            return 1;
        }
    }
    if (d <= 0.0f)
        return handle_local_min(w, p);
    return 0;
}

// VClip__stepEE 0x50a410.
int step_ee(Walk &w)
{
    for (int pass = 0;;) {
        const int s = g_side;
        Vec3 t, h;
        other_edge(w, s, &t, &h);
        Vec3 u = h - t;
        float lo = 0.0f, hi = 1.0f;
        int nlo, nhi;
        const PolyFeature &e = w.f(s);
        clip_edge(t, h, e, &nlo, &nhi, &lo, &hi, 0, 2);
        if (nlo == nhi) {
            if (nlo >= 0) {
                w.feat[s] = nlo;
                return 1;
            }
        } else if (nlo >= 0 && vertex_deriv(w, s, nlo, t, u, lo) > 0.0f) {
            w.feat[s] = nlo;
            return 1;
        }
        if (nhi >= 0 && vertex_deriv(w, s, nhi, t, u, hi) < 0.0f) {
            w.feat[s] = nhi;
            return 1;
        }
        // E-F planes, lambda interval carried over from the V-E clip.
        clip_edge(t, h, e, &nlo, &nhi, &lo, &hi, 2, (int)e.planes.size());
        if (nlo == nhi) {
            if (nlo >= 0) {
                w.feat[s] = nlo;
                return 1;
            }
        } else if (nlo >= 0 && face_deriv(w, s, nlo, t, u, lo) > 0.0f) {
            w.feat[s] = nlo;
            return 1;
        }
        if (nhi >= 0 && face_deriv(w, s, nhi, t, u, hi) < 0.0f) {
            w.feat[s] = nhi;
            return 1;
        }
        g_side = 1 - g_side;
        if (++pass > 1)
            return 0;
    }
}

// stepEF's boundary walk: after leaving face plane i at vertex `v`, the next
// plane is i+1 (wrapping) when its edge holds v, else i-1 (wrapping).
int next_face_plane(const Walk &w, int o, const PolyFeature &fc, int i, int v)
{
    const int last = (int)fc.planes.size() - 1;
    int next = last <= i ? 0 : i + 1;
    const PolyFeature &e = w.nb(o, fc.planes[next].neighbour);
    if (e.planes[0].neighbour != v && e.planes[1].neighbour != v)
        next = i > 0 ? i - 1 : last;
    return next;
}

// VClip__stepEF 0x50aa60 (g_side = E). 1 = moved, 2 = penetration.
int step_ef(Walk &w)
{
    const int s = g_side, o = s ^ 1;
    const PolyFeature &e = w.f(s);
    const PolyFeature &fc = w.f(o);
    Vec3 t = w.xf(s, o, w.vert(s, e.vertex));
    Vec3 h = w.xf(s, o, w.vert(s, e.head));
    Vec3 u = h - t;
    const Vec3 fp = w.vert(o, fc.vertex);
    const int np = (int)fc.planes.size();
    float lo = 0.0f, hi = 1.0f;
    int nlo, nhi;
    if (clip_edge(t, h, fc, &nlo, &nhi, &lo, &hi, 0, np)) {
        float dlo = vec_dot(fc.normal, t + u * lo - fp);
        float dhi = vec_dot(fc.normal, t + u * hi - fp);
        if (dhi * dlo < 0.0f)
            return 2;
        if (dlo * vec_dot(fc.normal, u) < 0.0f) {
            if (nhi < 0) {
                w.feat[s] = e.planes[1].neighbour;
                return 1;
            }
            w.feat[o] = nhi;
        } else {
            if (nlo < 0) {
                w.feat[s] = e.planes[0].neighbour;
                return 1;
            }
            w.feat[o] = nlo;
        }
        return 1;
    }
    // Clip failed: walk the face boundary from the plane that excluded the edge.
    int i = 0;
    while (i < np && fc.planes[i].neighbour != nlo)
        i++;
    if (i >= np)
        i = 0; // INVENTED: guard; the exe scans unbounded (nlo is always a face plane)
    int cur = -1, prev = -1, result = -1;
    bool clipped = false, touched = false; // local_14 / local_10
    for (;;) {
        int next = i + 1;
        lo = 0.0f;
        hi = 1.0f;
        prev = cur;
        cur = i;
        clip_edge(t, h, fc, &nlo, &nhi, &lo, &hi, i, i + 1);
        if (nlo == nhi && nlo < 0) {
            result = prev;
            if (touched || clipped)
                break;
            if (np - 1 <= i)
                next = 0;
        } else {
            clipped = true;
            if (nlo == nhi) {
                lo = 0.0f;
                hi = 1.0f;
            } else if (nlo < 0) {
                lo = hi;
                hi = 1.0f;
            } else {
                hi = lo;
                lo = 0.0f;
            }
            const PolyFeature &edge = w.nb(o, fc.planes[i].neighbour);
            clip_edge(t, h, edge, &nlo, &nhi, &lo, &hi, 0, 2);
            if (nlo == nhi && nlo >= 0) {
                next = next_face_plane(w, o, fc, i, nlo);
            } else if (hi < lo) {
                result = prev;
                if (touched)
                    break;
                next = next_face_plane(w, o, fc, i, nlo < 0 ? nhi : nlo);
            } else {
                touched = true;
                if (nlo >= 0 && vertex_deriv(w, o, nlo, t, u, lo) > 0.0f) {
                    next = next_face_plane(w, o, fc, i, nlo);
                } else {
                    result = i;
                    if (nhi < 0 || vertex_deriv(w, o, nhi, t, u, hi) >= 0.0f)
                        break;
                    next = next_face_plane(w, o, fc, i, nhi);
                }
            }
        }
        i = next;
        result = prev;
        if (next == prev)
            break;
    }
    w.feat[o] = fc.planes[result].neighbour;
    return 1;
}

void write_back(const Walk &w, int *feature_a, int *feature_b)
{
    if (feature_a)
        *feature_a = w.feat[0];
    if (feature_b)
        *feature_b = w.feat[1];
}

} // namespace

float vclip_closest(const Pose &pose_a, const Poly &poly_a,
                    const Pose &pose_b, const Poly &poly_b,
                    int *feature_a, int *feature_b,
                    Vec3 *point_a, Vec3 *point_b, Vec3 *normal)
{
    const int nfa = static_cast<int>(poly_a.features.size());
    const int nfb = static_cast<int>(poly_b.features.size());
    if (nfa == 0 || nfb == 0)
        return 1e6f;
    Walk w{{&pose_a, &pose_b}, {&poly_a, &poly_b}, {0, 0}};
    w.feat[0] = feature_a && *feature_a >= 0 && *feature_a < nfa ? *feature_a : 0;
    w.feat[1] = feature_b && *feature_b >= 0 && *feature_b < nfb ? *feature_b : 0;

    auto finish = [&](const Hit &h) {
        write_back(w, feature_a, feature_b);
        if (point_a)
            *point_a = to_local(pose_a, h.pa);
        if (point_b)
            *point_b = to_local(pose_b, h.pb);
        if (normal)
            *normal = h.n;
        return h.d;
    };
    auto measure = [&]() {
        return distance_pair(poly_a, pose_a, w.f(0), poly_b, pose_b, w.f(1));
    };

    for (int step = 0; step <= nfa + 2 * nfb; step++) {
        const int code = w.f(g_side).kind * 0x100 + w.f(g_side ^ 1).kind;
        int r;
        switch (code) {
        case 0x202:
            if (step_ee(w) == 0)
                return finish(measure());
            break;
        case 0x101:
            if (check_vv(w) == 0)
                return finish(measure());
            break;
        case 0x201:
        case 0x102:
            if (code == 0x201)
                g_side ^= 1;
            if (step_ve(w) == 0)
                return finish(measure());
            break;
        case 0x301:
        case 0x103:
            if (code == 0x301)
                g_side ^= 1;
            r = step_vf(w);
            if (r == 0 || r == 2)
                return finish(measure());
            break;
        case 0x302:
        case 0x203:
            if (code == 0x302)
                g_side ^= 1;
            // stepEF never returns 0; 2 = the edge crosses the face (depth of
            // the end behind it, witness at the crossing).
            if (step_ef(w) == 2)
                return finish(measure());
            break;
        case 0x303:
            w.feat[0] = w.f(0).planes[0].neighbour;
            break;
        default:
            return 1e6f;
        }
    }

    fprintf(stderr, "Voronoi error (infinite loop)\n");
    g_vclip_overruns++;
    write_back(w, feature_a, feature_b);
    return 1e6f;
}

int vclip_overruns()
{
    return g_vclip_overruns;
}

float vclip_check(const Pose &pose_a, const Poly &poly_a,
                  const Pose &pose_b, const Poly &poly_b,
                  int feature_a, int feature_b,
                  Vec3 *point_a, Vec3 *point_b, Vec3 *normal)
{
    // INVENTED: one pass reproducing the checkVV/VE/VF/EE/EF result codes of
    // notes/11 §3; the exe bodies are not transcribed line by line.
    if (feature_a < 0 || feature_a >= static_cast<int>(poly_a.features.size()) ||
        feature_b < 0 || feature_b >= static_cast<int>(poly_b.features.size()))
        return 0.0f;
    const PolyFeature &fa = poly_a.features[feature_a];
    const PolyFeature &fb = poly_b.features[feature_b];
    if (fa.kind == POLY_F && fb.kind == POLY_F)
        return 0.0f;

    Hit h = distance_pair(poly_a, pose_a, fa, poly_b, pose_b, fb);
    // 0x50d9d0 writes the witnesses (body-local) and the world normal of the
    // cached pair; 0x4f9af0 keeps them on the refreshed contact.
    if (point_a)
        *point_a = to_local(pose_a, h.pa);
    if (point_b)
        *point_b = to_local(pose_b, h.pb);
    if (normal)
        *normal = h.n;

    // VF: a vertex below the face plane is penetration (result 2) and still
    // yields a distance; otherwise a violated face plane invalidates the pair.
    if ((fa.kind == POLY_V && fb.kind == POLY_F) ||
        (fa.kind == POLY_F && fb.kind == POLY_V)) {
        if (h.d < 0.0f)
            return h.d;
    }
    // VV/VE/EE only report a distance for a valid pair (result 0).
    if (outside_region(pose_a, fa, h.pb) >= 0 ||
        outside_region(pose_b, fb, h.pa) >= 0)
        return 0.0f;
    // EF reports a distance only on penetration (result 2).
    if ((fa.kind == POLY_E && fb.kind == POLY_F) ||
        (fa.kind == POLY_F && fb.kind == POLY_E))
        return h.d <= 0.0f ? h.d : 0.0f;
    return h.d;
}
