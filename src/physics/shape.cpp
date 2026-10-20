// Collision shapes and contact generation (notes/11-collision-shapes.md).
// The exe runs V-Clip over point/segment/box polyhedra; this port computes the
// same signed separations and witness points directly per kind pair. Box-box
// and box/plane generate a four-point manifold so resting contacts are stable.
#include <math.h>

#include "physics/physics.h"

static const float kContactSlop = 0.02f; // notes/11 §4: d < rA+rB+0.02 is a contact

float Shape::bounding_radius() const
{
    switch (type) {
    case SHAPE_SPHERE:
        return radius + 0.05f;
    case SHAPE_CAPSULE_X:
    case SHAPE_CAPSULE_Y:
    case SHAPE_CAPSULE_Z:
        return half_length + radius + 0.05f;
    case SHAPE_BOX:
        return sqrtf(half_extents.x * half_extents.x + half_extents.y * half_extents.y +
                     half_extents.z * half_extents.z) + 0.05f;
    default:
        return 0.0f;
    }
}

bool shape_kind_resolved(int type)
{
    switch (type) {
    case SHAPE_SPHERE:
    case SHAPE_CAPSULE_X:
    case SHAPE_CAPSULE_Y:
    case SHAPE_CAPSULE_Z:
    case SHAPE_BOX:
    case SHAPE_PLANE:
        return true;
    default:
        // TODO(notes/11 §1): SHAPE_MESH (6) needs the .scx loader, Poly__buildMesh
        // and the V-Clip feature walk before it can produce contacts. Shape 7 and
        // 0 are skipped by Collide__bodyPair in the exe as well.
        return false;
    }
}

static bool is_capsule(int type)
{
    return type == SHAPE_CAPSULE_X || type == SHAPE_CAPSULE_Y || type == SHAPE_CAPSULE_Z;
}

static void capsule_endpoints(const Shape &s, const Pose &p, Vec3 *e0, Vec3 *e1)
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

// Core points of a shape: sphere centre, capsule segment ends, box vertices.
// Mesh kinds are TODO and return 0 points.
static int shape_core_points(const Shape &s, const Pose &p, Vec3 *pts)
{
    switch (s.type) {
    case SHAPE_SPHERE:
        pts[0] = p.pos;
        return 1;
    case SHAPE_CAPSULE_X:
    case SHAPE_CAPSULE_Y:
    case SHAPE_CAPSULE_Z:
        capsule_endpoints(s, p, &pts[0], &pts[1]);
        return 2;
    case SHAPE_BOX: {
        Mat34 r = pose_rotation(p);
        for (int i = 0; i < 8; i++) {
            Vec3 l{(i & 1) ? s.half_extents.x : -s.half_extents.x,
                   (i & 2) ? s.half_extents.y : -s.half_extents.y,
                   (i & 4) ? s.half_extents.z : -s.half_extents.z};
            pts[i] = p.pos + mat_vec(r, l);
        }
        return 8;
    }
    default:
        return 0;
    }
}

static float shape_core_radius(const Shape &s)
{
    return (s.type == SHAPE_SPHERE || is_capsule(s.type)) ? s.radius : 0.0f;
}

static void plane_frame(const Shape &s, const Pose &p, Vec3 *point, Vec3 *normal)
{
    Mat34 r = pose_rotation(p);
    *normal = vec_normalize(mat_vec(r, s.plane_normal), Vec3{0.0f, 1.0f, 0.0f});
    *point = p.pos + mat_vec(r, s.plane_point);
}

static void set_hit(ShapeHit *h, Vec3 pa, Vec3 pb, Vec3 n, float sep, int fa, int fb)
{
    h->point_a = pa;
    h->point_b = pb;
    h->normal = n;
    h->separation = sep;
    h->feature_a = fa;
    h->feature_b = fb;
}

// Closest point on segment [a,b] to p (used by capsule queries).
static Vec3 closest_on_segment(Vec3 p, Vec3 a, Vec3 b)
{
    Vec3 ab = b - a;
    float denom = vec_dot(ab, ab);
    if (denom <= 1e-12f)
        return a;
    float t = vec_dot(p - a, ab) / denom;
    if (t < 0.0f)
        t = 0.0f;
    if (t > 1.0f)
        t = 1.0f;
    return a + ab * t;
}

// Closest points between two segments (VClip__distEE2 / Math__segSegClosest2
// 0x509740), both parameters clamped to the segments.
static void segment_closest(Vec3 p1, Vec3 q1, Vec3 p2, Vec3 q2, Vec3 *c1, Vec3 *c2)
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
            t = 0.0f;
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

// One-witness result; normal points from the second shape to the first.
struct PairHit {
    Vec3 pa, pb, n;
    float sep;
};

static PairHit sphere_capsule_hit(Vec3 centre, float rs, Vec3 e0, Vec3 e1, float rc)
{
    Vec3 q = closest_on_segment(centre, e0, e1);
    Vec3 d = centre - q;
    float dist = vec_length(d);
    Vec3 n = vec_normalize(d, Vec3{0.0f, 1.0f, 0.0f});
    PairHit h;
    h.pa = centre - n * rs;
    h.pb = q + n * rc;
    h.n = n;
    h.sep = dist - rs - rc;
    return h;
}

// Sphere vs OBB. Normal points box -> sphere; separation is the surface distance.
static PairHit sphere_box_hit(Vec3 centre, float rs, const Shape &box, const Pose &bp)
{
    Mat34 r = pose_rotation(bp);
    Vec3 l = mat_tvec(r, centre - bp.pos);
    Vec3 he = box.half_extents;
    Vec3 q{l.x < -he.x ? -he.x : (l.x > he.x ? he.x : l.x),
           l.y < -he.y ? -he.y : (l.y > he.y ? he.y : l.y),
           l.z < -he.z ? -he.z : (l.z > he.z ? he.z : l.z)};
    Vec3 d = l - q;
    float dist = vec_length(d);
    PairHit h;
    if (dist > 1e-6f) {
        h.n = mat_vec(r, d * (1.0f / dist));
        h.pb = bp.pos + mat_vec(r, q);
        h.pa = centre - h.n * rs;
        h.sep = dist - rs;
    } else {
        // Centre inside the box: use the nearest face and a negative separation.
        const float lv[3] = {l.x, l.y, l.z};
        const float hv[3] = {he.x, he.y, he.z};
        float best = 1e30f;
        int k = 0;
        for (int i = 0; i < 3; i++) {
            float gap = hv[i] - fabsf(lv[i]);
            if (gap < best) {
                best = gap;
                k = i;
            }
        }
        float sgn = lv[k] >= 0.0f ? 1.0f : -1.0f;
        Vec3 nl{0.0f, 0.0f, 0.0f};
        Vec3 fl = l;
        if (k == 0) {
            nl.x = sgn;
            fl.x = sgn * hv[0];
        } else if (k == 1) {
            nl.y = sgn;
            fl.y = sgn * hv[1];
        } else {
            nl.z = sgn;
            fl.z = sgn * hv[2];
        }
        h.n = mat_vec(r, nl);
        h.pb = bp.pos + mat_vec(r, fl);
        h.pa = centre - h.n * rs;
        h.sep = -(best + rs);
    }
    return h;
}

static int emit_single(PairHit h, ShapeHit *hits, int max_hits, float slop, int fa, int fb)
{
    if (max_hits <= 0 || (slop >= 0.0f && h.sep >= slop))
        return 0;
    set_hit(&hits[0], h.pa, h.pb, h.n, h.sep, fa, fb);
    return 1;
}

static int pair_plane_other(const Shape &plane, const Pose &pp, const Shape &other, const Pose &op,
                            bool plane_is_a, ShapeHit *hits, int max_hits, float slop)
{
    Vec3 pts[8];
    int np = shape_core_points(other, op, pts);
    if (np == 0)
        return 0;
    Vec3 p, n;
    plane_frame(plane, pp, &p, &n);
    // n_op points from the plane to the other body (B -> A when the plane is B).
    float side = vec_dot(op.pos - p, n) < 0.0f ? -1.0f : 1.0f;
    Vec3 n_op = n * side;
    float radius = shape_core_radius(other);
    ShapeHit cand[8];
    int nc = 0;
    for (int i = 0; i < np; i++) {
        float sep = vec_dot(pts[i] - p, n_op) - radius;
        if (slop >= 0.0f && sep >= slop)
            continue;
        Vec3 on_plane = pts[i] - n * vec_dot(pts[i] - p, n);
        Vec3 on_other = pts[i] - n_op * radius;
        ShapeHit &h = cand[nc];
        if (plane_is_a) {
            // A = plane, B = other: normal is other -> plane.
            set_hit(&h, on_plane, on_other, -n_op, sep, i, i);
        } else {
            set_hit(&h, on_other, on_plane, n_op, sep, i, i);
        }
        nc++;
    }
    for (int i = 1; i < nc; i++) { // insertion sort, deepest first
        ShapeHit key = cand[i];
        int j = i - 1;
        while (j >= 0 && cand[j].separation > key.separation) {
            cand[j + 1] = cand[j];
            j--;
        }
        cand[j + 1] = key;
    }
    int take = nc < max_hits ? nc : max_hits;
    for (int i = 0; i < take; i++)
        hits[i] = cand[i];
    return take;
}

static int pair_sphere_sphere(const Shape &a, const Pose &pa, const Shape &b, const Pose &pb,
                              ShapeHit *hits, int max_hits, float slop)
{
    Vec3 d = pa.pos - pb.pos;
    float dist = vec_length(d);
    Vec3 n = vec_normalize(d, Vec3{0.0f, 1.0f, 0.0f});
    PairHit h;
    h.pa = pa.pos - n * a.radius;
    h.pb = pb.pos + n * b.radius;
    h.n = n;
    h.sep = dist - a.radius - b.radius;
    return emit_single(h, hits, max_hits, slop, 0, 0);
}

static int pair_sphere_capsule(const Shape &sph, const Pose &sp, const Shape &cap,
                               const Pose &cp, bool sphere_is_a, ShapeHit *hits, int max_hits,
                               float slop)
{
    Vec3 e0, e1;
    capsule_endpoints(cap, cp, &e0, &e1);
    PairHit h = sphere_capsule_hit(sp.pos, sph.radius, e0, e1, cap.radius);
    if (!sphere_is_a) {
        Vec3 t = h.pa;
        h.pa = h.pb;
        h.pb = t;
        h.n = -h.n;
    }
    return emit_single(h, hits, max_hits, slop, 0, 0);
}

static int pair_sphere_box(const Shape &sph, const Pose &sp, const Shape &box, const Pose &bp,
                           bool sphere_is_a, ShapeHit *hits, int max_hits, float slop)
{
    PairHit h = sphere_box_hit(sp.pos, sph.radius, box, bp);
    if (!sphere_is_a) {
        Vec3 t = h.pa;
        h.pa = h.pb;
        h.pb = t;
        h.n = -h.n;
    }
    return emit_single(h, hits, max_hits, slop, 0, 0);
}

static int pair_capsule_capsule(const Shape &a, const Pose &pa, const Shape &b, const Pose &pb,
                                ShapeHit *hits, int max_hits, float slop)
{
    Vec3 a0, a1, b0, b1;
    capsule_endpoints(a, pa, &a0, &a1);
    capsule_endpoints(b, pb, &b0, &b1);
    Vec3 ca, cb;
    segment_closest(a0, a1, b0, b1, &ca, &cb);
    Vec3 d = ca - cb;
    float dist = vec_length(d);
    Vec3 n;
    if (dist > 1e-6f) {
        n = d * (1.0f / dist);
    } else {
        // Parallel/overlapping axes: the documented E-E normal dA x dB degenerates.
        n = vec_normalize(vec_cross(a1 - a0, b1 - b0), Vec3{0.0f, 1.0f, 0.0f});
    }
    PairHit h;
    h.pa = ca - n * a.radius;
    h.pb = cb + n * b.radius;
    h.n = n;
    h.sep = dist - a.radius - b.radius;
    return emit_single(h, hits, max_hits, slop, 0, 0);
}

// Capsule vs box: the swept sphere is sampled at both ends and the middle
// (shortcut: a segment crossing the box between samples is not detected; upgrade
// to a segment-OBB distance query if capsules need full coverage).
static int pair_capsule_box(const Shape &cap, const Pose &cp, const Shape &box, const Pose &bp,
                            bool capsule_is_a, ShapeHit *hits, int max_hits, float slop)
{
    Vec3 e0, e1;
    capsule_endpoints(cap, cp, &e0, &e1);
    Vec3 samples[3];
    int ns;
    if (vec_length(e1 - e0) < 1e-6f) {
        samples[0] = e0;
        ns = 1;
    } else {
        samples[0] = e0;
        samples[1] = (e0 + e1) * 0.5f;
        samples[2] = e1;
        ns = 3;
    }
    PairHit ph[3];
    for (int i = 0; i < ns; i++)
        ph[i] = sphere_box_hit(samples[i], cap.radius, box, bp);
    for (int i = 1; i < ns; i++) { // insertion sort, deepest first
        PairHit key = ph[i];
        int j = i - 1;
        while (j >= 0 && ph[j].sep > key.sep) {
            ph[j + 1] = ph[j];
            j--;
        }
        ph[j + 1] = key;
    }
    int count = 0;
    for (int i = 0; i < ns && count < max_hits; i++) {
        if (slop >= 0.0f && ph[i].sep >= slop)
            continue;
        if (capsule_is_a) {
            set_hit(&hits[count], ph[i].pa, ph[i].pb, ph[i].n, ph[i].sep, i, i);
        } else {
            set_hit(&hits[count], ph[i].pb, ph[i].pa, -ph[i].n, ph[i].sep, i, i);
        }
        count++;
    }
    return count;
}

// ---- box vs box (SAT + reference-face clipping) ----

struct Obb {
    Vec3 pos;
    Vec3 axis[3];
    float half[3];
};

static Obb make_obb(const Shape &s, const Pose &p)
{
    Obb o;
    o.pos = p.pos;
    o.half[0] = s.half_extents.x;
    o.half[1] = s.half_extents.y;
    o.half[2] = s.half_extents.z;
    Mat34 r = pose_rotation(p);
    for (int i = 0; i < 3; i++)
        o.axis[i] = Vec3{r.m[i], r.m[4 + i], r.m[8 + i]};
    return o;
}

static Vec3 obb_support(const Obb &o, Vec3 d)
{
    Vec3 p = o.pos;
    for (int i = 0; i < 3; i++) {
        float s = vec_dot(o.axis[i], d) >= 0.0f ? 1.0f : -1.0f;
        p += o.axis[i] * (o.half[i] * s);
    }
    return p;
}

static void clip_poly(Vec3 *pts, int *count, Vec3 normal, float offset, Vec3 origin)
{
    Vec3 out[16];
    int n = *count, on = 0;
    for (int i = 0; i < n; i++) {
        Vec3 p = pts[i], q = pts[(i + 1) % n];
        float dp = vec_dot(normal, p - origin) - offset;
        float dq = vec_dot(normal, q - origin) - offset;
        if (dp <= 0.0f)
            out[on++] = p;
        if ((dp < 0.0f && dq > 0.0f) || (dp > 0.0f && dq < 0.0f)) {
            float t = dp / (dp - dq);
            out[on++] = p + (q - p) * t;
        }
    }
    *count = on;
    for (int i = 0; i < on; i++)
        pts[i] = out[i];
}

static int pair_box_box(const Shape &sa, const Pose &pa, const Shape &sb, const Pose &pb,
                        ShapeHit *hits, int max_hits, float slop)
{
    Obb a = make_obb(sa, pa), b = make_obb(sb, pb);
    float best_overlap = 1e30f;
    Vec3 best_axis{0.0f, 1.0f, 0.0f};
    int best_ref = -2; // -2 none, -1 edge-edge, 0 A face, 1 B face
    int best_index = 0;

    // 15 SAT axes: 3 + 3 face normals, 9 edge cross products. The minimum
    // overlap axis is the contact normal; it stays slightly negative for a
    // pair within the keep slop, so a resting stack keeps its pair alive.
    auto test_axis = [&](Vec3 axis, int ref, int index) {
        float len2 = vec_dot(axis, axis);
        if (len2 < 1e-12f)
            return;
        axis = axis * (1.0f / sqrtf(len2));
        float ra = a.half[0] * fabsf(vec_dot(a.axis[0], axis)) +
                   a.half[1] * fabsf(vec_dot(a.axis[1], axis)) +
                   a.half[2] * fabsf(vec_dot(a.axis[2], axis));
        float rb = b.half[0] * fabsf(vec_dot(b.axis[0], axis)) +
                   b.half[1] * fabsf(vec_dot(b.axis[1], axis)) +
                   b.half[2] * fabsf(vec_dot(b.axis[2], axis));
        float d = vec_dot(b.pos - a.pos, axis);
        float overlap = ra + rb - fabsf(d);
        if (overlap < best_overlap) {
            best_overlap = overlap;
            best_axis = axis;
            best_ref = ref;
            best_index = index;
        }
    };
    for (int i = 0; i < 3; i++)
        test_axis(a.axis[i], 0, i);
    for (int i = 0; i < 3; i++)
        test_axis(b.axis[i], 1, i);
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++)
            test_axis(vec_cross(a.axis[i], b.axis[j]), -1, 0);
    if (best_ref == -2)
        return 0;
    // Unfiltered queries (slop < 0) still limit witnesses to near-contact.
    float limit = slop >= 0.0f ? slop : 0.05f;
    if (best_overlap < -limit)
        return 0;

    Vec3 n = best_axis; // B -> A
    if (vec_dot(a.pos - b.pos, n) < 0.0f)
        n = -n;

    if (best_ref == -1) {
        // Edge-edge: support points along the separating axis (witness points of
        // the closest features are not recovered, one contact is emitted).
        Vec3 pA = obb_support(a, -n);
        Vec3 pB = obb_support(b, n);
        float sep = vec_dot(pA - pB, n);
        if (max_hits <= 0 || (slop >= 0.0f && sep >= slop))
            return 0;
        set_hit(&hits[0], pA, pB, n, sep, 0, 0);
        return 1;
    }

    const Obb &ref = best_ref == 0 ? a : b;
    const Obb &inc = best_ref == 0 ? b : a;
    int k = best_index;
    Vec3 n_ref = best_ref == 0 ? -n : n; // outward from the reference face
    Vec3 fc = ref.pos + n_ref * ref.half[k];
    Vec3 u = ref.axis[(k + 1) % 3], v = ref.axis[(k + 2) % 3];
    float hu = ref.half[(k + 1) % 3], hv = ref.half[(k + 2) % 3];

    int j = 0;
    float best_dot = -1.0f;
    for (int i = 0; i < 3; i++) {
        float dd = fabsf(vec_dot(inc.axis[i], n_ref));
        if (dd > best_dot) {
            best_dot = dd;
            j = i;
        }
    }
    Vec3 n_inc = inc.axis[j] * (vec_dot(inc.axis[j], n_ref) > 0.0f ? -1.0f : 1.0f);
    Vec3 fi = inc.pos + n_inc * inc.half[j];
    Vec3 iu = inc.axis[(j + 1) % 3], iv = inc.axis[(j + 2) % 3];
    float hiu = inc.half[(j + 1) % 3], hiv = inc.half[(j + 2) % 3];

    Vec3 poly[16];
    poly[0] = fi + iu * hiu + iv * hiv;
    poly[1] = fi - iu * hiu + iv * hiv;
    poly[2] = fi - iu * hiu - iv * hiv;
    poly[3] = fi + iu * hiu - iv * hiv;
    int np = 4;
    clip_poly(poly, &np, u, hu, fc);
    clip_poly(poly, &np, -u, hu, fc);
    clip_poly(poly, &np, v, hv, fc);
    clip_poly(poly, &np, -v, hv, fc);

    struct Cand {
        Vec3 p, proj;
        float sep;
    };
    Cand cand[16];
    int nc = 0;
    for (int i = 0; i < np; i++) {
        float sep = vec_dot(poly[i] - fc, n_ref);
        if (slop >= 0.0f && sep >= slop)
            continue;
        cand[nc].p = poly[i];
        cand[nc].proj = poly[i] - n_ref * sep;
        cand[nc].sep = sep;
        nc++;
    }
    if (nc == 0 && slop < 0.0f) { // clipping produced nothing; keep the face centre
        float sep = vec_dot(fi - fc, n_ref);
        cand[0].p = fi;
        cand[0].proj = fi - n_ref * sep;
        cand[0].sep = sep;
        nc = 1;
    }
    for (int i = 1; i < nc; i++) { // insertion sort, deepest first
        Cand key = cand[i];
        int m = i - 1;
        while (m >= 0 && cand[m].sep > key.sep) {
            cand[m + 1] = cand[m];
            m--;
        }
        cand[m + 1] = key;
    }
    int take = nc < max_hits ? nc : max_hits;
    for (int i = 0; i < take; i++) {
        if (best_ref == 0)
            set_hit(&hits[i], cand[i].proj, cand[i].p, n, cand[i].sep, i, i);
        else
            set_hit(&hits[i], cand[i].p, cand[i].proj, n, cand[i].sep, i, i);
    }
    return take;
}

static int collide_pair(const Shape &a, const Pose &pa, const Shape &b, const Pose &pb,
                        ShapeHit *hits, int max_hits, float slop)
{
    if (max_hits <= 0)
        return 0;
    if (!shape_kind_resolved(a.type) || !shape_kind_resolved(b.type))
        return 0; // TODO: see shape_kind_resolved
    if (a.type == SHAPE_PLANE && b.type == SHAPE_PLANE)
        return 0; // TODO: the exe has no plane-plane pair either
    if (a.type == SHAPE_PLANE)
        return pair_plane_other(a, pa, b, pb, true, hits, max_hits, slop);
    if (b.type == SHAPE_PLANE)
        return pair_plane_other(b, pb, a, pa, false, hits, max_hits, slop);

    // Broad phase (notes/11 §4): bounding radii already include the 0.05 slack.
    Vec3 dp = pb.pos - pa.pos;
    float reach = a.bounding_radius() + b.bounding_radius() + 0.05f;
    if (vec_dot(dp, dp) > reach * reach)
        return 0;

    if (a.type == SHAPE_SPHERE && b.type == SHAPE_SPHERE)
        return pair_sphere_sphere(a, pa, b, pb, hits, max_hits, slop);
    if (a.type == SHAPE_SPHERE && is_capsule(b.type))
        return pair_sphere_capsule(a, pa, b, pb, true, hits, max_hits, slop);
    if (is_capsule(a.type) && b.type == SHAPE_SPHERE)
        return pair_sphere_capsule(b, pb, a, pa, false, hits, max_hits, slop);
    if (is_capsule(a.type) && is_capsule(b.type))
        return pair_capsule_capsule(a, pa, b, pb, hits, max_hits, slop);
    if (a.type == SHAPE_SPHERE && b.type == SHAPE_BOX)
        return pair_sphere_box(a, pa, b, pb, true, hits, max_hits, slop);
    if (a.type == SHAPE_BOX && b.type == SHAPE_SPHERE)
        return pair_sphere_box(b, pb, a, pa, false, hits, max_hits, slop);
    if (is_capsule(a.type) && b.type == SHAPE_BOX)
        return pair_capsule_box(a, pa, b, pb, true, hits, max_hits, slop);
    if (a.type == SHAPE_BOX && is_capsule(b.type))
        return pair_capsule_box(b, pb, a, pa, false, hits, max_hits, slop);
    if (a.type == SHAPE_BOX && b.type == SHAPE_BOX)
        return pair_box_box(a, pa, b, pb, hits, max_hits, slop);
    return 0;
}

bool shape_distance(const Shape &a, const Pose &pa, const Shape &b, const Pose &pb, ShapeHit *out)
{
    return collide_pair(a, pa, b, pb, out, 1, -1.0f) > 0; // no slop filter
}

int collide_shapes(const Shape &a, const Pose &pa, const Shape &b, const Pose &pb, ShapeHit *hits,
                   int max_hits)
{
    return collide_pair(a, pa, b, pb, hits, max_hits, kContactSlop);
}

int collide_shapes_within(const Shape &a, const Pose &pa, const Shape &b, const Pose &pb,
                          ShapeHit *hits, int max_hits, float slop)
{
    return collide_pair(a, pa, b, pb, hits, max_hits, slop);
}
