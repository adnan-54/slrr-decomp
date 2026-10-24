// V-Clip polyhedra and closest-feature queries (implementation of vclip.h).
// Addresses are the exe functions the notes describe (notes/11 §2/§3).
#include "physics/vclip.h"

#include <math.h>
#include <stdio.h>

#include <map>
#include <utility>

namespace {

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

// Signed distance of a world point to a face feature (world).
float face_distance(const Poly &poly, const Pose &pose, const PolyFeature &f, Vec3 world_point)
{
    Vec3 q = to_world(pose, poly.verts[f.vertex]);
    Mat34 r = pose_rotation(pose);
    return vec_dot(mat_vec(r, f.normal), world_point - q);
}

int vertex_feature_of(const Poly &poly, int vertex)
{
    for (int i = 0; i < static_cast<int>(poly.features.size()); i++)
        if (poly.features[i].kind == POLY_V && poly.features[i].vertex == vertex)
            return i;
    return -1;
}

// checkEE 0x509f10: the other edge's closest point must be inside this edge's
// two V-E planes (planes[0] tail, planes[1] head). Returns the V to step to.
int edge_vertex_step(const Pose &pose, const PolyFeature &e, Vec3 world_point)
{
    Vec3 p = to_local(pose, world_point);
    int step = -1;
    float worst = 0.0f;
    for (size_t i = 0; i < e.planes.size() && i < 2; i++) {
        float v = vec_dot(e.planes[i].normal, p - e.planes[i].point);
        if (v < 0.0f && v < worst) {
            worst = v;
            step = e.planes[i].neighbour;
        }
    }
    return step;
}

// VClip__stepEF 0x50aa60 + VClip__clipEdge 0x508fe0. Clip the edge to the
// face's Voronoi region, then either step the edge to the closest original
// endpoint (V-F next), step the face to the boundary edge the clip hit
// (E-E/V-E next), report penetration, or leave the E-F pair valid.
// INVENTED: the notes give clipEdge's lambda interval and entering/leaving
// neighbours and stepEF's opposite-side test, not the exact branch order;
// this follows Mirtich's stepEF.
struct EStep {
    int edge_feature = -1; // V feature on the edge's poly
    int face_feature = -1; // E feature on the face's poly
    bool penetration = false;
};

EStep step_ef(const Poly &edge_poly, const Pose &edge_pose, const PolyFeature &e,
              const Poly &face_poly, const Pose &face_pose, const PolyFeature &f)
{
    EStep s;
    FeatureGeom eg = feature_geom(edge_poly, edge_pose, e);
    FeatureGeom fg = feature_geom(face_poly, face_pose, f);
    Mat34 fr = pose_rotation(face_pose);
    float lo = 0.0f, hi = 1.0f;
    int enter = -1, leave = -1;
    for (const PolyPlane &pl : f.planes) {
        Vec3 pw = face_pose.pos + mat_vec(fr, pl.point);
        Vec3 nw = mat_vec(fr, pl.normal);
        float d0 = vec_dot(nw, eg.p - pw);
        float d1 = vec_dot(nw, eg.h - pw);
        if (d0 < 0.0f && d1 < 0.0f) {
            s.face_feature = pl.neighbour;
            return s;
        }
        if (d0 < 0.0f) {
            float t = d0 / (d0 - d1);
            if (t > lo) {
                lo = t;
                enter = pl.neighbour;
            }
        } else if (d1 < 0.0f) {
            float t = d0 / (d0 - d1);
            if (t < hi) {
                hi = t;
                leave = pl.neighbour;
            }
        }
    }
    if (lo > hi) {
        s.face_feature = enter >= 0 ? enter : leave;
        return s;
    }
    float d0 = vec_dot(fg.n, eg.p - fg.p);
    float d1 = vec_dot(fg.n, eg.h - fg.p);
    float dl = d0 + (d1 - d0) * lo;
    float dh = d0 + (d1 - d0) * hi;
    if (dl * dh < 0.0f) {
        s.penetration = true;
        return s;
    }
    bool use_lo = fabsf(dl) <= fabsf(dh);
    float lam = use_lo ? lo : hi;
    if (lam <= 0.0f || lam >= 1.0f) {
        s.edge_feature = vertex_feature_of(edge_poly, lam <= 0.0f ? e.vertex : e.head);
        return s;
    }
    s.face_feature = use_lo ? enter : leave;
    return s;
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
            m = normalize_or(m, Vec3{0.0f, 1.0f, 0.0f});
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

float vclip_closest(const Pose &pose_a, const Poly &poly_a,
                    const Pose &pose_b, const Poly &poly_b,
                    int *feature_a, int *feature_b,
                    Vec3 *point_a, Vec3 *point_b, Vec3 *normal)
{
    const int nfa = static_cast<int>(poly_a.features.size());
    const int nfb = static_cast<int>(poly_b.features.size());
    if (nfa == 0 || nfb == 0)
        return 1e6f;
    int a = feature_a && *feature_a >= 0 && *feature_a < nfa ? *feature_a : 0;
    int b = feature_b && *feature_b >= 0 && *feature_b < nfb ? *feature_b : 0;
    const int budget = nfa + 2 * nfb;

    for (int step = 0; step <= budget; step++) {
        const PolyFeature &fa = poly_a.features[a];
        const PolyFeature &fb = poly_b.features[b];

        // FF: A's first plane neighbour is an edge (notes/11 §3).
        if (fa.kind == POLY_F && fb.kind == POLY_F) {
            if (fa.planes.empty())
                break;
            a = fa.planes[0].neighbour;
            continue;
        }

        Hit h = distance_pair(poly_a, pose_a, fa, poly_b, pose_b, fb);

        // E-F: stepEF 0x50aa60 clips the edge to the face region. E-E:
        // checkEE 0x509f10 tests the other edge's closest point against this
        // edge's two V-E planes, then treats "behind every adjacent face" as
        // penetration (features unchanged).
        bool ef_penetration = false;
        if ((fa.kind == POLY_E && fb.kind == POLY_F) ||
            (fa.kind == POLY_F && fb.kind == POLY_E)) {
            const bool e_is_a = fa.kind == POLY_E;
            const Poly &edge_poly = e_is_a ? poly_a : poly_b;
            const Pose &edge_pose = e_is_a ? pose_a : pose_b;
            const PolyFeature &edge = e_is_a ? fa : fb;
            const Poly &face_poly = e_is_a ? poly_b : poly_a;
            const Pose &face_pose = e_is_a ? pose_b : pose_a;
            const PolyFeature &face = e_is_a ? fb : fa;
            EStep s = step_ef(edge_poly, edge_pose, edge, face_poly, face_pose, face);
            if (s.edge_feature >= 0) {
                if (e_is_a)
                    a = s.edge_feature;
                else
                    b = s.edge_feature;
                continue;
            }
            if (s.face_feature >= 0) {
                if (e_is_a)
                    b = s.face_feature;
                else
                    a = s.face_feature;
                continue;
            }
            ef_penetration = s.penetration;
        } else if (fa.kind == POLY_E && fb.kind == POLY_E) {
            int step_a = edge_vertex_step(pose_a, fa, h.pb);
            if (step_a >= 0) {
                a = step_a;
                continue;
            }
            int step_b = edge_vertex_step(pose_b, fb, h.pa);
            if (step_b >= 0) {
                b = step_b;
                continue;
            }
            // checkEE also reports penetration when the closest point is
            // behind every adjacent face; the features stay unchanged and the
            // segment distance is returned as is.
        }

        if (!ef_penetration) {
            // V-F local minimum: a vertex below a face switches to the face with
            // the largest signed distance, else it is penetration
            // (handleLocalMin 0x50b7b0). The vertex may belong to either side.
            if ((fa.kind == POLY_V && fb.kind == POLY_F) ||
                (fa.kind == POLY_F && fb.kind == POLY_V)) {
                const bool face_is_b = fb.kind == POLY_F;
                const Poly &face_poly = face_is_b ? poly_b : poly_a;
                const Pose &face_pose = face_is_b ? pose_b : pose_a;
                Vec3 vertex_world = face_is_b ? h.pa : h.pb;
                if (h.d < 0.0f) {
                    int best = face_is_b ? b : a;
                    float best_d = h.d;
                    for (int i = 0; i < static_cast<int>(face_poly.features.size()); i++) {
                        if (face_poly.features[i].kind != POLY_F)
                            continue;
                        float d = face_distance(face_poly, face_pose,
                                                face_poly.features[i], vertex_world);
                        if (d > best_d) {
                            best_d = d;
                            best = i;
                        }
                    }
                    if (face_is_b)
                        b = best;
                    else
                        a = best;
                    if (best_d > 0.0f)
                        continue;
                    h = distance_pair(poly_a, pose_a, poly_a.features[a], poly_b, pose_b,
                                      poly_b.features[b]);
                    if (feature_a)
                        *feature_a = a;
                    if (feature_b)
                        *feature_b = b;
                    if (point_a)
                        *point_a = to_local(pose_a, h.pa);
                    if (point_b)
                        *point_b = to_local(pose_b, h.pb);
                    if (normal)
                        *normal = h.n;
                    return h.d;
                }
            }

            int step_a = outside_region(pose_a, fa, h.pb);
            if (step_a >= 0) {
                a = step_a;
                continue;
            }
            int step_b = outside_region(pose_b, fb, h.pa);
            if (step_b >= 0) {
                b = step_b;
                continue;
            }
        }

        if (feature_a)
            *feature_a = a;
        if (feature_b)
            *feature_b = b;
        if (point_a)
            *point_a = to_local(pose_a, h.pa);
        if (point_b)
            *point_b = to_local(pose_b, h.pb);
        if (normal)
            *normal = h.n;
        return h.d;
    }

    fprintf(stderr, "Voronoi error (infinite loop)\n");
    return 1e6f;
}

float vclip_check(const Pose &pose_a, const Poly &poly_a,
                  const Pose &pose_b, const Poly &poly_b,
                  int feature_a, int feature_b)
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
