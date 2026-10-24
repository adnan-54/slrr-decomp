// V-Clip polyhedra and closest-feature queries (notes/11 §2/§3).
//
// The exe turns a kind-6 .scx hull into a Poly with Poly__buildFromFaces
// 0x50e6d0, turns one PHYS soup face into a two-face Poly with 0x50f000, walks
// the closest features with VClip__closestFeatures 0x50d230, and re-checks a
// cached feature pair with Phys__shapeDistance 0x50d9d0.
#pragma once
#include <vector>

#include "physics/mesh.h"
#include "physics/physics.h"

// Feature kinds of the V-Clip feature tree (shape+0x20, notes/11 §2).
enum PolyFeatureKind { POLY_V = 1, POLY_E = 2, POLY_F = 3 };

// VPlane (0x14): neighbour feature and the half-space n.(p - point) >= 0 that
// bounds this feature's Voronoi region.
struct PolyPlane {
    int neighbour = -1;
    Vec3 point{0.0f, 0.0f, 0.0f};
    Vec3 normal{0.0f, 0.0f, 0.0f};
};

// Feature (0x20): kind plus the geometry the distance routines read.
struct PolyFeature {
    int kind = POLY_V;
    int vertex = 0;   // V: the vertex; E: tail; F: first face vertex
    int head = 0;     // E: head
    Vec3 dir{0.0f, 0.0f, 0.0f};    // E: unit tail -> head
    Vec3 normal{0.0f, 0.0f, 0.0f}; // F: unit outward normal
    std::vector<PolyPlane> planes;
};

// Poly stores vertices in its body's local frame, exactly as the .scx loader
// leaves them (metres).
// INVENTED: C++ storage layout; the exe's fields are notes/11 §2.
struct Poly {
    std::vector<Vec3> verts;
    std::vector<PolyFeature> features;
};

// 0x50e6d0 over a kind-6 hull: every SCX face is a triangle {3, i0, i1, i2},
// processed in file order (coplanar faces are not merged).
Poly poly_from_triangles(const CollisionMesh &mesh);

// 0x50f000: the PHYS face and its reversed twin (table {3,i0,i1,i2,padding,n}
// and {3,i0,i2,i1,padding,-n}). The first call lets Poly__buildFromFaces
// derive both normals from the winding; the later in-place refresh writes the
// stored PHYS normal, which agrees with the winding, so `n` needs no separate
// path here.
Poly poly_triangle(Vec3 v0, Vec3 v1, Vec3 v2, Vec3 n);

// Feature counts and the closed-polyhedron invariants the hull corpus check
// asserts: every E has exactly two adjacent F and V - E + F = 2.
struct PolyCounts {
    int verts = 0, edges = 0, faces = 0;
    bool closed = false;
};
PolyCounts poly_counts(const Poly &p);

// 0x50d230 VClip__closestFeatures. Features are read from *feature_a /
// *feature_b (null or out of range -> that poly's first feature) and written
// back. Witness points are body-local, `normal` is world and points from B to
// A, and the return is the signed distance (negative = penetration). On
// overrun (nfeatA + 2*nfeatB steps) prints "Voronoi error (infinite loop)" and
// returns 1e6.
float vclip_closest(const Pose &pose_a, const Poly &poly_a,
                    const Pose &pose_b, const Poly &poly_b,
                    int *feature_a, int *feature_b,
                    Vec3 *point_a, Vec3 *point_b, Vec3 *normal);

// 0x50d9d0 Phys__shapeDistance: one validity test on the cached features,
// between the two poses. Returns the signed distance, or 0.0 when the feature
// pair needs an update (result 1; the caller drops the contact).
float vclip_check(const Pose &pose_a, const Poly &poly_a,
                  const Pose &pose_b, const Poly &poly_b,
                  int feature_a, int feature_b);

// INVENTED: harness counter, incremented next to the "Voronoi error" print;
// read only by vmboot (card 165). The exe has no such counter.
int vclip_overruns();
